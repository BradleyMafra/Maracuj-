#!/usr/bin/env python3
"""Fail closed on build-account data in a generated release directory.

This is a release gate, not a general secret detector. It checks the current
build account's home path plus optional, privately supplied known identifiers.
Public upstream examples such as C:\\Users\\example are not treated as personal
data without an exact known identifier. No matching file contents are logged.
Run with the same Python bytecode version as the packaged interpreter.
For distributions with more than one interpreter, inspect each subtree with
its interpreter and exclude that subtree from the other inspection.
"""

import argparse
from collections import Counter
import importlib.util
import io
import json
import marshal
import ntpath
import os
from pathlib import Path, PurePosixPath
import stat
import types
import zipfile

CHUNK_SIZE = 1024 * 1024
MAX_CODE_BYTES = 64 * 1024 * 1024
MAX_NESTED_ZIP_BYTES = 64 * 1024 * 1024
ZIP_SUFFIXES = {".zip", ".whl", ".jar", ".egg"}


class KnownText:
    def __init__(self, values):
        encoded = set()
        for value in values:
            # Cover both Windows path separators and escaped JSON/source text.
            variants = {value, value.replace("\\", "/"), value.replace("/", "\\")}
            variants.update(text.replace("\\", "\\\\") for text in tuple(variants))
            for text in variants:
                for encoding in ("utf-8", "utf-16-le", "utf-16-be"):
                    encoded.add(text.encode(encoding).lower())
        self.needles = tuple(encoded)
        self.overlap = max((len(value) for value in self.needles), default=1) - 1

    def contains(self, data):
        data = data.lower()
        return any(value in data for value in self.needles)

    def in_stream(self, source):
        previous = b""
        while chunk := source.read(CHUNK_SIZE):
            data = previous + chunk
            if self.contains(data):
                return True
            previous = data[-self.overlap:] if self.overlap else b""
        return False


def load_known_text(private_json=None):
    home = str(Path.home())
    values = [home] if home not in {"/", "\\", "."} else []
    if private_json is not None:
        document = json.loads(Path(private_json).read_text(encoding="utf-8-sig"))
        if isinstance(document, dict):
            document = document.get("forbidden_text")
        if (not isinstance(document, list) or not document
                or any(not isinstance(value, str) or not 3 <= len(value) <= 4096 for value in document)):
            raise ValueError("Use a nonempty JSON array of known private strings")
        values.extend(document)
    return KnownText(values)


def _absolute_filename(name):
    if name.startswith("<") and name.endswith(">"):
        return False
    return name.startswith(("/", "\\")) or bool(ntpath.splitdrive(name)[0])


def bytecode_has_absolute_filename(payload):
    if len(payload) < 16 or payload[:4] != importlib.util.MAGIC_NUMBER:
        raise ValueError("Bytecode version does not match the release checker")
    if int.from_bytes(payload[4:8], "little") & ~3:
        raise ValueError("Invalid bytecode flags")
    code = marshal.loads(payload[16:])
    if not isinstance(code, types.CodeType):
        raise ValueError("Invalid bytecode module")
    pending = [code]
    while pending:
        current = pending.pop()
        if _absolute_filename(current.co_filename):
            return True
        pending.extend(value for value in current.co_consts if isinstance(value, types.CodeType))
    return False


def _is_reparse(metadata):
    return bool(getattr(metadata, "st_file_attributes", 0)
                & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def _scope_directory(stage, relative):
    """Require an existing child directory without links, escapes or aliases."""
    value = os.fspath(relative)
    normalized = value.replace("\\", "/")
    parts = normalized.split("/")
    if (not value or normalized.startswith("/") or ntpath.splitdrive(value)[0]
            or any(part in {"", ".", ".."} for part in parts)):
        raise ValueError("Privacy scope must be a relative directory inside staging")
    candidate = stage
    for part in parts:
        candidate /= part
        metadata = candidate.lstat()
        if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata):
            raise ValueError("Privacy scope cannot traverse a link or reparse point")
    resolved = candidate.resolve(strict=True)
    if resolved == stage or not resolved.is_relative_to(stage) or not resolved.is_dir():
        raise ValueError("Privacy scope must be a child directory inside staging")
    return resolved


def audit_stage(stage, known_text, *, subtree=None, exclude=()):
    root = Path(stage).absolute()
    metadata = root.lstat()
    if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata):
        raise ValueError("Release stage cannot be a link or reparse point")
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("Release stage must be a directory")
    scope = _scope_directory(root, subtree) if subtree is not None else root
    excluded = {_scope_directory(root, value) for value in exclude}
    if any(path == scope or not path.is_relative_to(scope) for path in excluded):
        raise ValueError("Excluded privacy directories must be children of the selected scope")
    issues = []
    counts = Counter(files=0, symlinks=0, zip_entries=0, bytecode_files=0)

    def label(name):
        return "<redacted path>" if known_text.contains(name.encode("utf-8")) else name

    def issue(name, category):
        # Relative file names locate failures without printing their contents.
        # A private identifier can also occur in a file name: redact that name.
        issues.append({"path": label(name), "category": category})

    scope_name = scope.relative_to(root).as_posix()
    if scope != root and known_text.contains(scope_name.encode("utf-8")):
        issue(scope_name, "known_private_text_in_path")

    def check_code(source, name, size):
        counts["bytecode_files"] += 1
        if size > MAX_CODE_BYTES:
            issue(name, "bytecode_exceeds_inspection_limit")
            return
        try:
            if bytecode_has_absolute_filename(source.read(MAX_CODE_BYTES + 1)):
                issue(name, "absolute_bytecode_filename")
        except (ValueError, EOFError, TypeError):
            issue(name, "unreadable_or_incompatible_bytecode")

    def check_zip(archive, name, depth=0):
        if depth > 4:
            issue(name, "nested_archive_exceeds_inspection_limit")
            return
        if known_text.contains(archive.comment):
            issue(name, "known_private_text_in_archive_metadata")
        for entry in archive.infolist():
            counts["zip_entries"] += 1
            member = name + "!/" + entry.filename
            if known_text.contains(entry.filename.encode("utf-8") + entry.comment + entry.extra):
                issue(member, "known_private_text_in_archive_metadata")
            parts = PurePosixPath(entry.filename.replace("\\", "/"))
            if parts.is_absolute() or ".." in parts.parts or ntpath.splitdrive(entry.filename)[0]:
                issue(member, "unsafe_archive_member_path")
            mode = entry.external_attr >> 16
            if stat.S_ISLNK(mode):
                # No symlinks are expected inside the bundled Python ZIPs.
                # Resolving archive links safely requires extraction; fail closed.
                issue(member, "symlink_inside_archive")
                continue
            if entry.is_dir():
                continue
            try:
                with archive.open(entry) as source:
                    if known_text.in_stream(source):
                        issue(member, "known_private_text")
                suffix = PurePosixPath(entry.filename).suffix.lower()
                if suffix == ".pyc":
                    with archive.open(entry) as source:
                        check_code(source, member, entry.file_size)
                elif suffix in ZIP_SUFFIXES:
                    if entry.file_size > MAX_NESTED_ZIP_BYTES:
                        issue(member, "nested_archive_exceeds_inspection_limit")
                    else:
                        with archive.open(entry) as source:
                            payload = source.read(MAX_NESTED_ZIP_BYTES + 1)
                        with zipfile.ZipFile(io.BytesIO(payload)) as nested:
                            check_zip(nested, member, depth + 1)
            except (OSError, RuntimeError, zipfile.BadZipFile, NotImplementedError):
                issue(member, "unreadable_archive_member")

    def walk_error(_error):
        issue("<directory>", "unreadable_directory")

    for directory, directories, files in os.walk(scope, followlinks=False, onerror=walk_error):
        descend = []
        for filename in sorted(directories + files):
            path = Path(directory) / filename
            name = path.relative_to(root).as_posix()
            try:
                metadata = path.lstat()
                if known_text.contains(name.encode("utf-8")):
                    issue(name, "known_private_text_in_path")
                if stat.S_ISLNK(metadata.st_mode):
                    counts["symlinks"] += 1
                    target = os.readlink(path)
                    if known_text.contains(target.encode("utf-8")):
                        issue(name, "known_private_text_in_symlink")
                    try:
                        resolved = path.resolve(strict=True)
                        if Path(target).is_absolute() or not resolved.is_relative_to(root):
                            issue(name, "absolute_or_external_symlink")
                    except (OSError, RuntimeError):
                        issue(name, "broken_or_looping_symlink")
                    continue
                if _is_reparse(metadata):
                    # Junctions are not S_ISLNK on Windows. Never descend into
                    # them, even if their current target is inside staging.
                    issue(name, "unsupported_reparse_point")
                    continue
                if stat.S_ISDIR(metadata.st_mode):
                    if path not in excluded:
                        descend.append(filename)
                    continue
                if not stat.S_ISREG(metadata.st_mode):
                    issue(name, "unsupported_file_type")
                    continue
                counts["files"] += 1
                with path.open("rb") as source:
                    prefix = source.read(4)
                    source.seek(0)
                    if known_text.in_stream(source):
                        issue(name, "known_private_text")
                if path.suffix == ".pyc":
                    with path.open("rb") as source:
                        check_code(source, name, metadata.st_size)
                elif path.suffix.lower() in ZIP_SUFFIXES or prefix[:2] == b"MZ" and zipfile.is_zipfile(path):
                    with zipfile.ZipFile(path) as archive:
                        check_zip(archive, name)
            except (OSError, RuntimeError, zipfile.BadZipFile, NotImplementedError):
                issue(name, "unreadable_release_file")
        directories[:] = descend
    return {"passed": not issues, "counts": dict(counts), "issues": issues,
            "scope": label(scope_name),
            "excluded": sorted(label(path.relative_to(root).as_posix()) for path in excluded),
            "python_magic": importlib.util.MAGIC_NUMBER.hex()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--subtree", help="Only inspect this relative child directory of staging")
    parser.add_argument("--exclude", action="append", default=[],
                        help="Do not traverse this relative child directory (repeatable)")
    parser.add_argument("--forbidden-text-json", type=Path,
                        help="Private JSON array (or forbidden_text array), stored outside the stage")
    arguments = parser.parse_args()
    try:
        if arguments.forbidden_text_json and arguments.forbidden_text_json.resolve().is_relative_to(arguments.stage.resolve()):
            raise ValueError("The private identifier list must be outside the release stage")
        report = audit_stage(arguments.stage, load_known_text(arguments.forbidden_text_json),
                             subtree=arguments.subtree, exclude=arguments.exclude)
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
        # Exception messages can include local paths or private JSON values.
        print(json.dumps({"passed": False, "error": "privacy_gate_configuration_or_stage_error"}))
        return 1
    print(json.dumps(report, ensure_ascii=True, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
