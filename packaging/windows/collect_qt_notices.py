#!/usr/bin/env python3
"""Collect hash-pinned notices from the official Qt 6.8.3 module sources.

The prebuilt Windows SDK archives do not contain the modules' LICENSES trees.
This collector downloads only reviewed license/attribution documents, preserves
their exact upstream bytes and source paths, and never downloads source builds.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import tempfile
import urllib.parse
import urllib.request

QT_VERSION = "6.8.3"
QT_TAG = "v" + QT_VERSION
MODULES = {"qtbase", "qtdeclarative", "qtimageformats", "qtsvg", "qttranslations"}
MAX_NOTICE_BYTES = 2 * 1024 * 1024
LOCK_PATH = Path(__file__).with_name("qt-notices.lock.json")


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def safe_path(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("Invalid Qt notice source path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in value.split("/")):
        raise ValueError("Invalid Qt notice source path")
    return path


def source_url(module, path):
    return "https://raw.githubusercontent.com/qt/" + module + "/" + QT_TAG + "/" + urllib.parse.quote(path, safe="/")


def validate_lock(document):
    if (not isinstance(document, dict) or document.get("schema_version") != 1 or document.get("qt_version") != QT_VERSION
            or document.get("tag") != QT_TAG or set(document.get("modules", {})) != MODULES):
        raise ValueError("Unsupported Qt notice lock")
    files = document.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("Missing Qt notice files")
    seen = set()
    for record in files:
        if not isinstance(record, dict):
            raise ValueError("Invalid Qt notice record")
        module = record.get("module")
        path = record.get("path")
        safe_path(path)
        if module not in MODULES or (module, path) in seen:
            raise ValueError("Invalid or duplicate Qt notice record")
        seen.add((module, path))
        if (record.get("url") != source_url(module, path)
                or not re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", ""))
                or not re.fullmatch(r"[0-9a-f]{40}", record.get("source_blob_sha1", ""))
                or not isinstance(record.get("bytes"), int)
                or not 0 < record["bytes"] <= MAX_NOTICE_BYTES
                or record.get("kind") not in {"license", "attribution", "notice"}):
            raise ValueError("Invalid Qt notice download record")
    for module, metadata in document["modules"].items():
        if not isinstance(metadata, dict) or not re.fullmatch(r"[0-9a-f]{40}", metadata.get("source_tree_sha1", "")):
            raise ValueError("Missing exact Qt source tree identity")
        for kind, count_key in [("license", "license_files"), ("attribution", "attribution_files")]:
            actual = sum(record["module"] == module and record["kind"] == kind for record in files)
            if actual != metadata.get(count_key) or kind == "license" and actual < 1:
                raise ValueError("Incomplete Qt notice inventory")
    return files


def attribution_references(path, payload):
    # Qt's own v6.8.3 KWin attribution contains literal newlines in a copyright
    # string. Preserve its original bytes; tolerate only that JSON string detail
    # when resolving the explicitly named license/copyright documents.
    document = json.loads(payload, strict=False)
    records = document if isinstance(document, list) else [document]
    references = set()
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("Invalid Qt attribution document")
        for field in ("LicenseFile", "LicenseFiles", "CopyrightFile"):
            values = record.get(field, [])
            values = [values] if isinstance(values, str) else values
            if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
                raise ValueError("Invalid Qt attribution reference")
            for value in values:
                if "\\" in value or value.startswith("/") or ":" in value:
                    raise ValueError("Invalid Qt attribution reference")
                # The attribution's Path field describes the attributed code.
                # LicenseFile/CopyrightFile remain relative to the JSON folder.
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(path), value))
                safe_path(resolved)
                references.add(resolved)
    return references


def fetch(record, cache):
    cached = cache / record["sha256"]
    if cached.exists():
        if (not cached.is_file() or cached.is_symlink() or cached.stat().st_size != record["bytes"]
                or digest(cached) != record["sha256"]):
            raise RuntimeError("Qt notice cache verification failed")
        return cached
    request = urllib.request.Request(record["url"], headers={"User-Agent": "Maracuja-release-builder/0.9.5"})
    temporary = None
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            if urllib.parse.urlparse(response.geturl()).hostname != "raw.githubusercontent.com":
                raise RuntimeError("Unexpected Qt notice download origin")
            with tempfile.NamedTemporaryFile(dir=cache, prefix="notice-", suffix=".partial", delete=False) as output:
                temporary = Path(output.name)
                remaining = record["bytes"] + 1
                while remaining:
                    chunk = response.read(min(64 * 1024, remaining))
                    if not chunk:
                        break
                    output.write(chunk)
                    remaining -= len(chunk)
        if temporary.stat().st_size != record["bytes"] or digest(temporary) != record["sha256"]:
            raise RuntimeError("Qt notice download verification failed")
        temporary.replace(cached)
        return cached
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def collect(stage, cache, document, downloader=fetch):
    records = validate_lock(document)
    stage, cache = Path(stage).resolve(), Path(cache).resolve()
    if not stage.is_dir() or cache.is_relative_to(stage):
        raise ValueError("Use an existing stage and a private cache outside it")
    cache = cache / ("qt-notices-" + QT_VERSION)
    cache.mkdir(parents=True, exist_ok=True)
    verified = {}
    # Verify every upstream file and attribution reference before copying any
    # documents. A failed download can never produce a successful release gate.
    for record in records:
        path = Path(downloader(record, cache))
        if path.stat().st_size != record["bytes"] or digest(path) != record["sha256"]:
            raise RuntimeError("Qt notice payload verification failed")
        verified[(record["module"], record["path"])] = path
    for record in records:
        if record["kind"] == "attribution":
            source = verified[(record["module"], record["path"])]
            for reference in attribution_references(record["path"], source.read_bytes()):
                if (record["module"], reference) not in verified:
                    raise ValueError("Missing referenced Qt license or copyright document")
    target = stage / "licenses/Qt" / QT_VERSION
    for record in records:
        destination = target / record["module"] / record["path"]
        for path in [destination, *destination.parents]:
            if path == stage:
                break
            if path.is_symlink():
                raise ValueError("A Qt notice destination must not be a symlink")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(verified[(record["module"], record["path"])], destination)
    for name in ("sources.json", "README.txt"):
        if (target / name).is_symlink():
            raise ValueError("A Qt notice index must not be a symlink")
    (target / "sources.json").write_text(json.dumps(document, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    (target / "README.txt").write_text(
        "Qt " + QT_VERSION + " license and attribution documents\n\n"
        "Maracuja uses the Qt shared libraries. Qt and its bundled components\n"
        "retain their respective licenses and copyright notices, independently\n"
        "of the application's MIT license. The LGPL-3.0-only and GPL-3.0-only\n"
        "texts are preserved in each module's LICENSES directory.\n\n"
        "These are exact, unmodified documents from the official Qt module\n"
        "source tag " + QT_TAG + ". sources.json records every origin, source\n"
        "tree identity and SHA256. Attribution records include their referenced\n"
        "license/copyright files. Some records describe optional platforms or\n"
        "examples from those modules; those files are not application features.\n\n"
        "Qt source: https://github.com/qt/qt5/tree/" + QT_TAG + "\n"
        "Keep this directory when redistributing the package.\n",
        encoding="utf-8")
    return {"qt_version": QT_VERSION, "modules": sorted(MODULES), "notice_files": len(records),
            "notice_bytes": sum(record["bytes"] for record in records), "sha256_verified": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        document = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
        result = collect(arguments.stage, arguments.cache, document)
    except (OSError, ValueError, RuntimeError) as error:
        # Configuration/network exceptions can carry local filesystem paths.
        print(json.dumps({"passed": False, "error": "qt_notice_collection_failed", "kind": type(error).__name__}))
        return 1
    print(json.dumps(result, ensure_ascii=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
