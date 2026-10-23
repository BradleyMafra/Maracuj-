#!/usr/bin/env python3
"""Omit seven unused wheel command launchers from the private Windows runtime.

pip embeds the build interpreter's absolute path in these optional commands.
Maracuja calls python.exe and imports the libraries directly; it never calls
these entrypoints. Do not rewrite PE files or resolve a relative interpreter
through the working directory. Keep every library, notice and other command.
"""

import argparse
import base64
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile

LAUNCHERS = {
    "cffi-gen-src.exe": "cffi",
    "normalizer.exe": "charset_normalizer",
    "pdfplumber.exe": "pdfplumber",
    "pypdfium2.exe": "pypdfium2",
    "dumppdf.py": "pdfminer_six",
    "pdf2txt.py": "pdfminer_six",
    "vba_extract.py": "xlsxwriter",
}
MAX_RECORD_BYTES = 8 * 1024 * 1024
MAX_LAUNCHER_BYTES = 2 * 1024 * 1024


def checked_path(root, relative, *, directory=False, optional=False):
    """Accept only fixed children, without following any link or reparse point."""
    relative = Path(relative)
    if relative.is_absolute() or relative.anchor or not relative.parts:
        raise ValueError("Invalid private-runtime child path")
    candidate = root
    for part in relative.parts:
        if part in {"", ".", ".."} or ":" in part:
            raise ValueError("Invalid private-runtime child path")
        candidate /= part
        try:
            metadata = candidate.lstat()
        except FileNotFoundError:
            if optional and candidate == root / relative:
                return None
            raise
        if (stat.S_ISLNK(metadata.st_mode)
                or getattr(metadata, "st_file_attributes", 0)
                & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)):
            raise ValueError("Private-runtime paths cannot traverse links or reparse points")
    if not candidate.resolve(strict=True).is_relative_to(root):
        raise ValueError("Private-runtime path escapes staging")
    if directory != stat.S_ISDIR(metadata.st_mode) or not directory and not stat.S_ISREG(metadata.st_mode):
        raise ValueError("Unexpected private-runtime file type")
    return candidate


def replace_record(path, payload):
    descriptor, name = tempfile.mkstemp(prefix=".maracuja-record-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
        shutil.copystat(path, temporary, follow_symlinks=False)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def prune_stage(stage):
    supplied = Path(stage).absolute()
    # Check ancestors before resolve, which would otherwise hide a junction.
    # A SUBST drive is not a filesystem reparse point and remains supported.
    for path in (supplied, *supplied.parents):
        metadata = path.lstat()
        if (not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode)
                or getattr(metadata, "st_file_attributes", 0)
                & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)):
            raise ValueError("Release staging cannot traverse a link or reparse point")
    root = supplied.resolve(strict=True)
    checked_path(root, "runtime/python/python.exe")
    checked_path(root, "runtime/python/Scripts", directory=True)
    packages = checked_path(root, "runtime/python/Lib/site-packages", directory=True)
    planned_records, owned_rows, seen_owners = [], {}, set()
    for info in sorted(packages.iterdir()):
        if not info.name.endswith(".dist-info") or "-" not in info.name[:-10]:
            continue
        owner = info.name[:-10].split("-", 1)[0].lower().replace(".", "_")
        if owner not in LAUNCHERS.values():
            continue
        if owner in seen_owners:
            raise ValueError("Ambiguous private-runtime wheel ownership")
        seen_owners.add(owner)
        record = checked_path(root, info.relative_to(root) / "RECORD")
        if record.stat().st_size > MAX_RECORD_BYTES:
            raise ValueError("Wheel RECORD exceeds inspection limit")
        payload = record.read_bytes()
        text = payload.decode("utf-8")
        lines = list(io.StringIO(text, newline=""))
        reader = csv.reader(lines, strict=True)
        kept, previous = [], 0
        for row in reader:
            if len(row) != 3:
                raise ValueError("Invalid wheel RECORD row")
            entry = row[0]
            name = entry[len("../../Scripts/"):] if entry.startswith("../../Scripts/") else None
            if name in LAUNCHERS:
                if LAUNCHERS[name] != owner or name in owned_rows:
                    raise ValueError("Unexpected or duplicate wheel launcher ownership")
                owned_rows[name] = row
            else:
                kept.extend(lines[previous:reader.line_num])
            previous = reader.line_num
        replacement = "".join(kept).encode("utf-8")
        if replacement != payload:
            planned_records.append((record, replacement))

    planned_files = []
    for name in LAUNCHERS:
        path = checked_path(root, Path("runtime/python/Scripts") / name, optional=True)
        if path is None:
            continue
        row = owned_rows.get(name)
        if row is None:
            raise ValueError("Refusing to remove an unowned wheel launcher")
        size = path.stat().st_size
        if size > MAX_LAUNCHER_BYTES:
            raise ValueError("Wheel launcher exceeds inspection limit")
        payload = path.read_bytes()
        expected_hash = "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(payload).digest()).rstrip(b"=").decode("ascii")
        if row[1] != expected_hash or row[2] != str(size):
            raise ValueError("Wheel launcher does not match its RECORD")
        planned_files.append(path)

    # Every target and RECORD was validated before any mutation. Removing a
    # target first leaves a safely recoverable missing-file row if interrupted.
    for path in planned_files:
        path.unlink()
    for record, replacement in planned_records:
        replace_record(record, replacement)
    return {"removed": [path.relative_to(root).as_posix() for path in planned_files],
            "records_updated": [path.relative_to(root).as_posix() for path, _ in planned_records]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        report = prune_stage(arguments.stage)
    except (OSError, ValueError, RuntimeError, csv.Error):
        # Do not print exception messages that could contain private paths.
        print(json.dumps({"error": "windows_python_launcher_pruning_failed"}))
        return 1
    print(json.dumps(report, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
