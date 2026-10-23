#!/usr/bin/env python3
"""Remove build-account metadata from release archives without running runtimes.

Bytecode headers and executable code are retained, including sourceless modules.
Absolute traceback filenames are made relative to the packaged application and
pip's POSIX launchers locate their packaged sibling interpreter after relocation.
Use the same Python bytecode version as the packaged interpreter; incompatible
bytecode stops packaging instead of being silently deleted or published.
"""

import argparse
import importlib.util
import io
import json
import marshal
import ntpath
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import tempfile
import types
import zipfile


def anonymous_tarinfo(info):
    """Keep archive permissions/content while omitting the builder's account."""
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    # PAX values override the ordinary header if a caller supplies them.
    info.pax_headers = {key: value for key, value in info.pax_headers.items()
                        if key not in {"uid", "gid", "uname", "gname",
                                       "SCHILY.uid", "SCHILY.gid"}}
    return info


def _source_name(bytecode_name):
    """Find a stable source label even for modules with no source file."""
    path = Path(bytecode_name)
    if path.parent.name == "__pycache__":
        try:
            return Path(importlib.util.source_from_cache(str(path))).as_posix()
        except ValueError:
            pass
    return path.with_suffix(".py").as_posix()


def _relative_filename(filename, stage, fallback):
    # Synthetic filenames have intentional meanings to Python and tracebacks.
    if filename.startswith("<") and filename.endswith(">"):
        return filename
    drive, _ = ntpath.splitdrive(filename)
    if not filename.startswith(("/", "\\")) and not drive:
        return filename
    normalized = filename.replace("\\", "/")
    prefix = str(stage).replace("\\", "/").rstrip("/") + "/"
    # Windows paths are case-insensitive; POSIX paths retain their exact case.
    comparable, expected = (normalized.casefold(), prefix.casefold()) if drive else (normalized, prefix)
    if comparable.startswith(expected):
        return normalized[len(prefix):]
    # Upstream runtimes and compiled source snapshots can have a different
    # absolute prefix. The bytecode's location supplies a relocatable label.
    return fallback


def _sanitize_code(code, stage, fallback):
    changed = False
    constants = []
    for value in code.co_consts:
        if isinstance(value, types.CodeType):
            replacement = _sanitize_code(value, stage, fallback)
            changed = changed or replacement is not value
            value = replacement
        constants.append(value)
    filename = _relative_filename(code.co_filename, stage, fallback)
    if changed or filename != code.co_filename:
        return code.replace(co_filename=filename, co_consts=tuple(constants))
    return code


def sanitize_bytecode(payload, stage, bytecode_name):
    """Return (bytes, changed), preserving the PEP 552 header verbatim."""
    if len(payload) < 16 or payload[:4] != importlib.util.MAGIC_NUMBER:
        raise RuntimeError("Packaged bytecode requires a matching Python version: " + bytecode_name)
    flags = int.from_bytes(payload[4:8], "little")
    if flags & ~3:
        raise RuntimeError("Invalid packaged bytecode flags: " + bytecode_name)
    stream = io.BytesIO(payload[16:])
    try:
        code = marshal.load(stream)
    except (EOFError, ValueError, TypeError) as error:
        raise RuntimeError("Invalid packaged bytecode: " + bytecode_name) from error
    if not isinstance(code, types.CodeType):
        raise RuntimeError("Packaged bytecode does not contain a module: " + bytecode_name)
    sanitized = _sanitize_code(code, stage, _source_name(bytecode_name))
    if sanitized is code:
        return payload, False
    # Retain any trailing payload instead of assuming a cache has none.
    return payload[:16] + marshal.dumps(sanitized) + stream.read(), True


def _publish_replacement(path, write):
    """Replace a generated file atomically and retain mode and timestamps."""
    descriptor, temporary = tempfile.mkstemp(prefix=".maracuja-private-", dir=path.parent)
    os.close(descriptor)
    temporary = Path(temporary)
    try:
        write(temporary)
        shutil.copystat(path, temporary, follow_symlinks=False)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def sanitize_python_launcher(payload, stage, launcher_name):
    """Relocate only pip text launchers belonging to our private POSIX Python.

    Binary launchers (including Windows PE files with appended ZIPs) are never
    edited. The privacy gate inspects them and stops a release on known paths.
    """
    name = PurePosixPath(launcher_name)
    if name.parent.as_posix() != "runtime/python/bin" or not payload.startswith(b"#!"):
        return payload, False
    lines = payload.splitlines(keepends=True)
    consumed = 1
    first = lines[0].rstrip(b"\r\n")
    if first == b"#!/bin/sh":
        # distlib uses this exact polyglot preamble for interpreter paths with
        # spaces or shebangs longer than the kernel limit.
        if len(lines) < 3 or not lines[1].startswith(b"'''exec' ") or lines[2].strip() != b"' '''":
            return payload, False
        command = lines[1][len(b"'''exec' "):].decode("utf-8")
        tokens = shlex.split(command)
        if tokens[-2:] != ["$0", "$@"]:
            return payload, False
        tokens = tokens[:-2]
        consumed = 3
    else:
        try:
            tokens = shlex.split(first[2:].decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return payload, False
    if not tokens or not PurePosixPath(tokens[0]).is_absolute():
        return payload, False
    interpreter = PurePosixPath(tokens[0])
    expected = PurePosixPath(Path(stage).as_posix()) / "runtime/python/bin"
    if interpreter.parent != expected or not re.fullmatch(r"python(?:\d+(?:\.\d+)*)?", interpreter.name):
        return payload, False
    # POSIX shell and Python both understand this standard distlib polyglot.
    # Keep interpreter options, module code, and all caller arguments intact.
    options = "".join(" " + shlex.quote(value) for value in tokens[1:])
    prefix = ("#!/bin/sh\n'''exec' \"$(dirname -- \"$0\")/" + interpreter.name
              + "\"" + options + " \"$0\" \"$@\"\n' '''\n").encode("utf-8")
    return prefix + b"".join(lines[consumed:]), True


def sanitize_stage(stage):
    stage = Path(stage).resolve(strict=True)
    if not stage.is_dir():
        raise RuntimeError("Release staging must be a directory")
    changed_files = 0
    changed_zip_entries = 0
    changed_launchers = 0
    # Do not follow links outside the generated stage.
    for path in sorted(stage.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(stage).as_posix()
        if path.parent == stage / "runtime/python/bin":
            with path.open("rb") as source:
                is_script = source.read(2) == b"#!"
            if is_script:
                payload, changed = sanitize_python_launcher(path.read_bytes(), stage, relative)
                if changed:
                    _publish_replacement(path, lambda temporary: temporary.write_bytes(payload))
                    changed_launchers += 1
        if path.suffix == ".pyc":
            payload, changed = sanitize_bytecode(path.read_bytes(), stage, relative)
            if changed:
                _publish_replacement(path, lambda temporary: temporary.write_bytes(payload))
                changed_files += 1
        elif path.suffix.lower() == ".zip":
            # Some Python distributions keep their standard library in a ZIP.
            replacements = {}
            with zipfile.ZipFile(path) as archive:
                for entry in archive.infolist():
                    if entry.filename.endswith(".pyc") and not entry.is_dir():
                        name = relative + "/" + entry.filename
                        payload, changed = sanitize_bytecode(archive.read(entry), stage, name)
                        if changed:
                            replacements[entry.filename] = payload
            if not replacements:
                continue

            def write_archive(temporary):
                with zipfile.ZipFile(path) as source, zipfile.ZipFile(temporary, "w") as output:
                    output.comment = source.comment
                    for entry in source.infolist():
                        payload = replacements.get(entry.filename)
                        if payload is not None:
                            output.writestr(entry, payload)
                        else:
                            with source.open(entry) as original, output.open(entry, "w") as copy:
                                shutil.copyfileobj(original, copy, 1024 * 1024)

            _publish_replacement(path, write_archive)
            changed_zip_entries += len(replacements)
    return {"bytecode_files_sanitized": changed_files,
            "zip_bytecode_entries_sanitized": changed_zip_entries,
            "python_launchers_sanitized": changed_launchers}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    arguments = parser.parse_args()
    print(json.dumps(sanitize_stage(arguments.stage)))


if __name__ == "__main__":
    main()
