#!/usr/bin/env python3
"""Prepare a portable Office extension registry using a synthetic document only."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


PROJECT = Path(__file__).resolve().parents[2]


def load_helper(name):
    spec = importlib.util.spec_from_file_location("maracuja_build_" + name,
                                                PROJECT / "helpers" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


profile = load_helper("windows_office_profile")


def package_registry(source: Path, runtime_root: Path, destination: Path) -> dict:
    """Allowlist generated extension data, omitting all document/user state."""
    source = profile.filesystem_path(source)
    destination = profile.filesystem_path(destination)
    entries = {}
    for path in sorted((source / "user/extensions").rglob("*")):
        if profile.linked(path):
            raise RuntimeError("Linked generated Office registry")
        if not path.is_file():
            continue
        name = path.relative_to(source).as_posix()
        if not profile.allowed_name(name):
            continue
        data = path.read_bytes()
        if not profile.portable_payload(data):
            raise RuntimeError("Generated Office registry contains an absolute path")
        entries[name] = data
    if (not entries or "user/extensions/buildid" not in entries
            or len(entries) > profile.MAX_FILES
            or sum(map(len, entries.values())) > profile.MAX_BYTES):
        raise RuntimeError("Unexpected generated Office registry")
    destination.mkdir(parents=True, exist_ok=False)
    archive_path = destination / "extensions.zip"
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, payload in entries.items():
            # Fixed metadata excludes the builder's timestamps/account and
            # absolute paths. Fresh private copies receive current timestamps.
            info = zipfile.ZipInfo(name, (2000, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 0
            info.external_attr = 0o600 << 16
            archive.writestr(info, payload)
    manifest = {"schema": profile.SCHEMA, "purpose": "bundled-extension-registry-only",
                "runtime_identity": profile.runtime_identity(runtime_root),
                "archive_sha256": hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                          for name, data in entries.items()}}
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def prepare(stage: Path) -> dict:
    if os.name != "nt":
        raise RuntimeError("Office profile preparation requires Windows")
    stage = stage.resolve(strict=True)
    destination = stage / profile.CACHE_DIRECTORY
    if destination.exists():
        raise RuntimeError("Office profile staging must be fresh")
    documents = load_helper("document_convert")
    documents._RUNTIME_ROOT = str(stage)
    executable = stage / "runtime/libreoffice/program/soffice.com"
    # No user-provided path or document is accepted by this build step.
    parent = profile.filesystem_path(stage.parent)
    with tempfile.TemporaryDirectory(prefix=".maracuja-office-build-", dir=parent) as temporary:
        work_name = str(temporary)
        if work_name.startswith("\\\\?\\UNC\\"):
            work_name = "\\\\" + work_name[8:]
        elif work_name.startswith("\\\\?\\"):
            work_name = work_name[4:]
        work = Path(work_name)
        documents.write_profile(work)
        (work / "source.txt").write_text("Maracuja portable Office initialization.\n", encoding="utf-8")
        (work / "out").mkdir()
        roaming, local = work / "home/AppData/Roaming", work / "home/AppData/Local"
        roaming.mkdir(parents=True)
        local.mkdir(parents=True)
        environment = dict(os.environ)
        environment.update(HOME=str(work / "home"), USERPROFILE=str(work / "home"),
                           APPDATA=str(roaming), LOCALAPPDATA=str(local),
                           TEMP=str(work / "tmp"), TMP=str(work / "tmp"))
        system = Path(environment.get("SystemRoot", r"C:\Windows"))
        environment["PATH"] = os.pathsep.join([str(executable.parent), str(stage / "tools"),
                                               str(system / "System32")])
        process = subprocess.Popen(documents.windows_office_command(executable, work, "txt", "pdf"),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, env=environment,
                                   **documents.runtime().subprocess_options())
        try:
            returncode = process.wait(timeout=120)
        except BaseException:
            # Kill only the build-owned private Office tree on timeout/cancel.
            subprocess.run([str(system / "System32/taskkill.exe"), "/PID", str(process.pid), "/T", "/F"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=20, **documents.runtime().subprocess_options())
            process.wait(timeout=20)
            raise
        product = work / "out/source.pdf"
        if returncode or not product.is_file() or product.stat().st_size == 0:
            raise RuntimeError("Synthetic Office profile initialization failed")
        manifest = package_registry(work / "profile", stage, destination)
    # Round-trip the exact distribution data after the synthetic profile has
    # been destroyed. A template that needs its build paths fails this check.
    files = profile.read_template(stage)
    return {"office_profile_prepared": True, "files": len(files),
            "bytes": sum(map(len, files.values())), "runtime_identity": manifest["runtime_identity"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--check", action="store_true",
                        help="Verify the packaged template without creating or modifying any profile")
    arguments = parser.parse_args()
    if arguments.check:
        files = profile.read_template(arguments.stage.resolve(strict=True))
        print(json.dumps({"office_profile_verified": True, "files": len(files),
                          "bytes": sum(map(len, files.values()))}))
    else:
        print(json.dumps(prepare(arguments.stage)))


if __name__ == "__main__":
    main()
