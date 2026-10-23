#!/usr/bin/env python3
"""Prepare the exact Qt/MinGW SDK in a private directory, with SHA256 gating."""
import argparse
import hashlib
import importlib.util
import json
import logging
from pathlib import Path

import py7zr
from aqt.updater import Updater

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("maracuja_packaging_fetch", ROOT / "packaging/linux/fetch_runtime.py")
fetcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetcher)

parser = argparse.ArgumentParser()
parser.add_argument("--tools", type=Path, required=True)
parser.add_argument("--cache", type=Path, required=True)
args = parser.parse_args()
tools, cache = args.tools.resolve(), args.cache.resolve() / "qt-mingw"
cache.mkdir(parents=True, exist_ok=True)
lock_path = Path(__file__).with_name("qt-mingw.lock.json")
lock = json.loads(lock_path.read_text())
archives = []
for entry in lock["archives"]:
    print("Verifying Qt dependency: " + entry["package"], flush=True)
    archives.append((entry, fetcher.fetch(entry, cache)))

qt = tools / "Qt"
marker = qt / ".maracuja-verified-sdk"
fingerprint = hashlib.sha256(lock_path.read_bytes() + b"direct-extraction-v1").hexdigest()
if not marker.is_file() or marker.read_text().strip() != fingerprint:
    prefix = qt / lock["qt_version"] / "mingw_64"
    # Extract the SHA256-verified files themselves. The aqt CLI downloads them
    # a second time and its qmake probe assumes UTF-8, although Windows qmake
    # emits the local code page (e.g. CP1252 for a path containing Maracuja/á).
    for entry, archive in archives:
        filename = archive.name.removeprefix(entry["version"])
        if entry["package"] == lock["compiler_package"]:
            destination = qt
        elif filename.startswith(("MinGW-", "d3dcompiler_", "opengl32sw-")):
            destination = prefix / "bin"
        else:
            destination = prefix
        print("Extracting " + archive.name, flush=True)
        destination.mkdir(parents=True, exist_ok=True)
        with py7zr.SevenZipFile(archive) as contents:
            contents.extractall(path=destination)
    # Qt 6's CMake files resolve their prefix relative to the SDK. qt.conf
    # likewise relocates qmake/windeployqt without probing or patching a binary.
    updater = Updater(prefix, logging.getLogger("maracuja.qt"))
    updater.set_license(str(qt), lock["qt_version"], "mingw_64")
    updater.make_qtconf(str(qt), lock["qt_version"], "mingw_64")
    updater.patch_pkgconfig("c:/Users/qt/work/install", "windows")
    updater.patch_prl("c:/Users/qt/work/install/lib")
    if not (qt / "6.8.3/mingw_64/bin/windeployqt.exe").is_file():
        raise RuntimeError("Qt MinGW SDK installation incomplete")
    if not (qt / "Tools/mingw1310_64/bin/objdump.exe").is_file():
        raise RuntimeError("MinGW compiler installation incomplete")
    marker.write_text(fingerprint + "\n")
print(json.dumps({"qt_sdk_sha256_verified": True, "compiler": "MinGW 13.1.0", "globally_installed": False}))
