#!/usr/bin/env python3
"""Emit shipped dependency/source inventory and reproducible per-file hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--stage", type=Path, required=True)
parser.add_argument("--version", required=True)
arguments = parser.parse_args()
root = arguments.stage.resolve()
project = Path(__file__).resolve().parents[2]
licenses = root / "licenses"
licenses.mkdir(exist_ok=True)
records = []
for path in sorted(root.rglob("*")):
    if path.is_file() and not path.is_symlink():
        hasher = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                hasher.update(chunk)
        records.append({"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
                        "sha256": hasher.hexdigest()})
(licenses / "shipped-files.json").write_text(json.dumps({"app": "Maracujá", "version": arguments.version,
    "platform_artifact_requires_ci_validation": True, "files": records}, indent=2) + "\n")
sources = {
    "application": "https://github.com/BradleyMafra/Maracuj-",
    "qt": "https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.tar.xz",
    "python": "https://www.python.org/ftp/python/3.12.15/Python-3.12.15.tar.xz",
    "python_build_recipes": "https://github.com/astral-sh/python-build-standalone/tree/20261021",
    "libreoffice": "https://download.documentfoundation.org/libreoffice/src/26.8.1/",
    "ocr_models": "https://github.com/tesseract-ocr/tessdata_fast/tree/65727574dfcd264acbb0c3e07860e4e9e9b22185",
}
if os_name := ("linux" if (root / "lib").is_dir() else "windows"):
    if os_name == "linux":
        result = subprocess.run(["qmake6", "-query", "QT_VERSION"], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, check=False)
        version = result.stdout.strip()
        if result.returncode == 0 and version.count(".") == 2 and all(x.isdecimal() for x in version.split(".")):
            series = ".".join(version.split(".")[:2])
            sources["qt"] = "https://download.qt.io/official_releases/qt/" + series + "/" + version + "/single/qt-everywhere-src-" + version + ".tar.xz"
        sources["distribution_sources"] = "distribution-sources.json"
lock = project / "packaging/windows/msys2-packages.lock.json"
if lock.is_file() and (licenses / "msys2-packages.lock.json").is_file():
    sources["mingw_packages"] = json.loads(lock.read_text())["packages"]
(licenses / "SOURCES.json").write_text(json.dumps(sources, indent=2) + "\n")
# Python wheel notices are shipped in each distribution's dist-info directory;
# Qt windeployqt licenses and LibreOffice LICENSE/NOTICE remain in their trees.
(licenses / "README.txt").write_text(
    "Maracujá includes separately licensed third-party components.\n"
    "Keep this licenses directory, Python dist-info notices, Qt licenses and\n"
    "LibreOffice LICENSE/NOTICE files when redistributing. Exact binary versions,\n"
    "SHA256 values and corresponding upstream sources are listed beside this file.\n"
    "FFmpeg (MSYS2) is GPL-3.0-or-later; Poppler is GPL-2.0-or-later;\n"
    "Tesseract/models are Apache-2.0. Qt has LGPL/GPL/commercial terms,\n"
    "LibreOffice MPL-2.0/LGPL terms, Python PSF terms. Consult original notices.\n",
    encoding="utf-8")
