#!/usr/bin/env python3
"""Preserve exact notices for the optional graphics DLLs deployed by Qt.

These redistributables are separate from the Qt modules and the application's
MIT license. Review their source/version and update the lock when either binary
changes. An unavailable or corrupt notice stops packaging, including SkipTests.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import tempfile
import urllib.parse
import urllib.request

LOCK_PATH = Path(__file__).with_name("graphics-notices.lock.json")
MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_NOTICE_BYTES = 2 * 1024 * 1024
REQUIRED_BINARIES = {"opengl32sw.dll", "d3dcompiler_47.dll"}


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def safe_path(value):
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value
            or any(part in {"", ".", ".."} for part in value.split("/"))
            or PurePosixPath(value).is_absolute()):
        raise ValueError("Invalid graphics notice path")
    return value


def official_url(value):
    if not isinstance(value, str):
        return False
    parsed = urllib.parse.urlparse(value)
    return (parsed.scheme == "https" and not parsed.username and not parsed.password
            and (parsed.netloc == "archive.mesa3d.org"
                 or parsed.netloc == "raw.githubusercontent.com" and parsed.path.startswith(
                     ("/llvm/llvm-project/", "/microsoft/", "/qt/"))
                 or parsed.netloc in {"download.microsoft.com", "learn.microsoft.com", "go.microsoft.com"}))


def hash_size(record, maximum):
    return (re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", "")) is not None
            and isinstance(record.get("bytes"), int) and 0 < record["bytes"] <= maximum)


def validate_lock(document):
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or set(document.get("binaries", {})) != REQUIRED_BINARIES
            or set(document.get("components", {})) != {"Mesa", "LLVM", "WindowsSDK"}):
        raise ValueError("Incomplete graphics notice lock")
    for value in document["binaries"].values():
        if not isinstance(value, dict) or not hash_size(value, 64 * 1024 * 1024):
            raise ValueError("Invalid graphics binary identity")
    sources = document.get("sources")
    if not isinstance(sources, list) or not sources:
        raise ValueError("Missing graphics notice sources")
    names, source_ids, covered = set(), set(), set()
    for source in sources:
        if (not isinstance(source, dict) or source.get("component") not in document["components"]
                or source.get("format") not in {"file", "tar", "vendored"} or not official_url(source.get("url"))
                or not hash_size(source, MAX_SOURCE_BYTES) or not source.get("id")
                or source["id"] in source_ids):
            raise ValueError("Invalid graphics notice source")
        source_ids.add(source["id"])
        covered.add(source["component"])
        if source["format"] == "vendored":
            local = safe_path(source.get("vendored_path"))
            archive = source.get("source_archive")
            if (not local.startswith("notices/") or source["component"] != "WindowsSDK"
                    or not isinstance(archive, dict) or not hash_size(archive, MAX_SOURCE_BYTES)
                    or not isinstance(archive.get("member"), str) or not archive["member"]):
                raise ValueError("Missing reviewed vendored notice provenance")
        documents = source.get("documents")
        if not isinstance(documents, list) or not documents:
            raise ValueError("Missing graphics notice documents")
        for record in documents:
            if not isinstance(record, dict) or not hash_size(record, MAX_NOTICE_BYTES):
                raise ValueError("Invalid graphics notice document")
            name = safe_path(record.get("path"))
            if name in names:
                raise ValueError("Duplicate graphics notice destination")
            names.add(name)
            if source["format"] == "tar":
                safe_path(record.get("member"))
            elif len(documents) != 1 or record["sha256"] != source["sha256"] or record["bytes"] != source["bytes"]:
                raise ValueError("File notice identity differs from its source")
    if covered != set(document["components"]):
        raise ValueError("Missing graphics component notices")
    return sources


def fetch(record, cache):
    destination = cache / record["sha256"]
    if destination.exists():
        if (destination.is_symlink() or not destination.is_file()
                or destination.stat().st_size != record["bytes"] or digest(destination) != record["sha256"]):
            raise RuntimeError("Graphics notice cache verification failed")
        return destination
    temporary = None
    try:
        request = urllib.request.Request(record["url"], headers={"User-Agent": "Maracuja-release-builder/0.9.5"})
        with urllib.request.urlopen(request, timeout=90) as response:
            if not official_url(response.geturl()):
                raise RuntimeError("Unexpected graphics notice download origin")
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
            raise RuntimeError("Graphics notice download verification failed")
        temporary.replace(destination)
        return destination
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def collect(stage, cache, document, downloader=fetch):
    sources = validate_lock(document)
    stage, cache = Path(stage).resolve(), Path(cache).resolve()
    if not stage.is_dir() or cache.is_relative_to(stage):
        raise ValueError("Use an existing stage and a private cache outside it")
    for name, identity in document["binaries"].items():
        matches = [path for path in stage.iterdir() if path.name.lower() == name]
        if (len(matches) != 1 or matches[0].is_symlink() or not matches[0].is_file()
                or matches[0].stat().st_size != identity["bytes"] or digest(matches[0]) != identity["sha256"]):
            raise RuntimeError("The graphics redistributable version differs from the reviewed lock")
    cache = cache / "graphics-notices"
    cache.mkdir(parents=True, exist_ok=True)
    verified = {}
    # Validate every document before writing any stage notice. Never extract an
    # archive: only read explicitly named, size-limited regular file members.
    for source in sources:
        if source["format"] == "vendored":
            base = Path(__file__).resolve().parent
            payload = base / source["vendored_path"]
            if payload.is_symlink() or not payload.resolve().is_relative_to(base):
                raise ValueError("Vendored graphics notice escaped its source directory")
        else:
            payload = Path(downloader(source, cache))
        if payload.stat().st_size != source["bytes"] or digest(payload) != source["sha256"]:
            raise RuntimeError("Graphics notice source verification failed")
        archive = tarfile.open(payload) if source["format"] == "tar" else None
        try:
            for record in source["documents"]:
                if archive is not None:
                    member = archive.getmember(record["member"])
                    if not member.isfile() or member.size != record["bytes"]:
                        raise ValueError("Invalid graphics notice archive member")
                    with archive.extractfile(member) as stream:
                        data = stream.read(MAX_NOTICE_BYTES + 1)
                else:
                    data = payload.read_bytes()
                if len(data) != record["bytes"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
                    raise RuntimeError("Graphics notice document verification failed")
                verified[record["path"]] = data
        finally:
            if archive is not None:
                archive.close()
    target = stage / "licenses/graphics"
    for name in [*verified, "sources.json", "README.txt"]:
        destination = target / name
        for path in [destination, *destination.parents]:
            if path == stage:
                break
            if path.is_symlink():
                raise ValueError("A graphics notice destination must not be a symlink")
    for name, data in verified.items():
        destination = target / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    (target / "sources.json").write_text(json.dumps(document, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    (target / "README.txt").write_text(
        "Graphics redistributables deployed by the official Qt Windows SDK\n\n"
        "opengl32sw.dll (Mesa/LLVM) and D3Dcompiler_47.dll (Microsoft Windows SDK)\n"
        "are separately licensed. Keep these exact upstream notices when\n"
        "redistributing. The application's MIT license does not replace them.\n"
        "sources.json identifies the reviewed DLL versions, source origins,\n"
        "document paths and mandatory SHA256 checks. Mesa's central license\n"
        "document describes additional component-specific notices.\n",
        encoding="utf-8")
    return {"notice_files": len(verified), "components": sorted(document["components"]), "sha256_verified": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        result = collect(arguments.stage, arguments.cache, json.loads(LOCK_PATH.read_text(encoding="utf-8")))
    except (OSError, ValueError, RuntimeError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"passed": False, "error": "graphics_notice_collection_failed", "kind": type(error).__name__}))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
