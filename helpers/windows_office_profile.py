#!/usr/bin/env python3
"""Copy a build-generated extension registry into one private Windows job.

The cache contains only LibreOffice's bundled extension registrations. It never
contains a converted document, user preferences, history, recovery or macros.
Every conversion still receives a new profile and its own security policy.
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import zipfile
import zlib


SCHEMA = 1
MAX_FILES = 1024
MAX_BYTES = 8 * 1024 * 1024
CACHE_DIRECTORY = "runtime/office-profile"
REGISTRY_PREFIX = "user/extensions/"


def filesystem_path(path: Path) -> Path:
    """Support deep extension registry names without changing Office's URLs."""
    value = os.path.abspath(path)
    if os.name == "nt" and not value.startswith("\\\\?\\"):
        value = "\\\\?\\UNC\\" + value[2:] if value.startswith("\\\\") else "\\\\?\\" + value
    return Path(value)


def linked(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def allowed_name(name: str) -> bool:
    path = PurePosixPath(name)
    return (name == path.as_posix() and not path.is_absolute() and "\\" not in name
            and ":" not in name and not any(part in {"", ".", ".."} for part in path.parts)
            and (name == REGISTRY_PREFIX + "buildid"
                 or name.startswith(REGISTRY_PREFIX + "bundled/")
                 or name.startswith(REGISTRY_PREFIX + "shared/"))
            and all(not part.startswith(".") and not part.lower().endswith(".lock")
                    for part in path.parts))


def portable_payload(payload: bytes) -> bool:
    # Also inspect UTF-16 strings. Extension registrations use LibreOffice's
    # $BUNDLED_EXTENSIONS macros; absolute file URLs would bind a cache to its
    # builder's account and must never reach a portable distribution.
    plain = payload.replace(b"\x00", b"")
    return re.search(rb"file:(?:/|\\)|file%3a|(?<![A-Za-z0-9])[A-Za-z]:[/\\](?!/)[A-Za-z0-9_%]"
                     rb"|\\\\[A-Za-z0-9_.-]{2,}\\[A-Za-z0-9_]", plain,
                     re.IGNORECASE) is None


def runtime_identity(runtime_root: Path) -> str:
    """Fingerprint version and extension registration inputs, not their path."""
    office = filesystem_path(Path(runtime_root) / "runtime/libreoffice")
    names = ["program/version.ini", "program/bootstrap.ini", "program/fundamental.ini"]
    extension_root = office / "share/extensions"
    if not extension_root.is_dir() or linked(extension_root):
        raise ValueError("Bundled Office extensions missing")
    extension_paths = sorted(extension_root.rglob("*"))
    if any(linked(path) for path in extension_paths):
        raise ValueError("Linked Office extension")
    extension_files = [path for path in extension_paths if path.is_file()]
    if len(extension_files) > 10000:
        raise ValueError("Bundled Office extension limit exceeded")
    # Dictionary bodies do not affect registration. Include immutable filenames
    # and sizes, and hash configuration, Python source and sourceless bytecode.
    # Office regenerates source-backed .pyc files; their creation, traceback
    # sanitization or removal must not invalidate an otherwise identical cache.
    configuration = {".xml", ".xcu", ".xcs", ".rdb", ".ini", ".components", ".py", ".pyc"}
    records = []
    for name in names:
        path = office / name
        if linked(path):
            raise ValueError("Linked Office identity file")
        records.append((name, hashlib.sha256(path.read_bytes()).hexdigest()))
    for path in extension_files:
        if path.suffix.lower() == ".pyc":
            try:
                source = (Path(importlib.util.source_from_cache(str(path)))
                          if path.parent.name == "__pycache__" else path.with_suffix(".py"))
            except ValueError:
                source = None
            if source is not None and source.is_file():
                continue
        relative = path.relative_to(office).as_posix()
        record = [relative, path.stat().st_size]
        if path.suffix.lower() in configuration:
            record.append(hashlib.sha256(path.read_bytes()).hexdigest())
        records.append(record)
    return hashlib.sha256(json.dumps(records, separators=(",", ":")).encode()).hexdigest()


def read_template(runtime_root: Path) -> dict[str, bytes]:
    root = filesystem_path(Path(runtime_root))
    cache = root / CACHE_DIRECTORY
    if linked(cache) or linked(cache / "manifest.json") or linked(cache / "extensions.zip"):
        raise ValueError("Linked Office profile cache")
    manifest_path = cache / "manifest.json"
    archive_path = cache / "extensions.zip"
    if manifest_path.stat().st_size > 256 * 1024 or archive_path.stat().st_size > MAX_BYTES:
        raise ValueError("Office profile cache limit exceeded")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA or manifest.get("purpose") != "bundled-extension-registry-only":
        raise ValueError("Unknown Office profile cache")
    identity = runtime_identity(root)
    if manifest.get("runtime_identity") != identity:
        raise ValueError("Office profile cache belongs to another runtime")
    payload = archive_path.read_bytes()
    if hashlib.sha256(payload).hexdigest() != manifest.get("archive_sha256"):
        raise ValueError("Damaged Office profile cache")
    files = manifest.get("files")
    if not isinstance(files, dict) or not 1 <= len(files) <= MAX_FILES:
        raise ValueError("Invalid Office profile inventory")
    result = {}
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        entries = archive.infolist()
        if (len(entries) != len(files) or sum(entry.file_size for entry in entries) > MAX_BYTES
                or len({entry.filename.casefold() for entry in entries}) != len(entries)):
            raise ValueError("Invalid Office profile archive")
        for entry in entries:
            if (not allowed_name(entry.filename) or entry.filename not in files
                    or entry.is_dir() or entry.flag_bits & 1
                    or (entry.external_attr >> 16) & 0o170000 == 0o120000):
                raise ValueError("Unexpected Office profile entry")
            data = archive.read(entry)
            record = files[entry.filename]
            if (not isinstance(record, dict) or len(data) != record.get("bytes")
                    or hashlib.sha256(data).hexdigest() != record.get("sha256")
                    or not portable_payload(data)):
                raise ValueError("Invalid Office profile entry contents")
            result[entry.filename] = data
    if REGISTRY_PREFIX + "buildid" not in result:
        raise ValueError("Office profile build ID missing")
    return result


def copy_prepared_profile(work: Path, runtime_root: Path) -> bool:
    """Return False on an absent/stale/damaged cache; keep the fresh profile.

    Call after document_convert.write_profile(). Only a newly created extensions
    directory can be populated, and the job's security policy is never touched.
    No part of this function writes to the installed template or other jobs.
    If an I/O failure also prevents removal of a partial copy, propagate it so
    the caller aborts the job instead of opening an incomplete registry.
    """
    destination = filesystem_path(Path(work) / "profile/user/extensions")
    created = False
    try:
        files = read_template(runtime_root)
        if destination.exists() or linked(destination) or linked(destination.parent):
            return False
        destination.mkdir()
        created = True
        for name, payload in files.items():
            relative = PurePosixPath(name).relative_to(REGISTRY_PREFIX)
            path = destination.joinpath(*relative.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
        return True
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile, zlib.error, RuntimeError):
        if created:
            # This exact directory was exclusively created above inside the
            # current private job, never supplied by a template archive path.
            shutil.rmtree(destination)
        return False
