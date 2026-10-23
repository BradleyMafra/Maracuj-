#!/usr/bin/env python3
"""Create a portable ZIP with streaming compression; no external 7-Zip needed."""
from pathlib import Path
import sys
import zipfile

source, destination = map(lambda value: Path(value).resolve(), sys.argv[1:3])
if not source.is_dir() or destination.exists() or destination.is_relative_to(source):
    raise RuntimeError("Use a fresh ZIP path outside the staged application")
with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=5) as archive:
    for path in sorted(source.rglob("*")):
        if path.is_file():
            archive.write(path, path.relative_to(source).as_posix())
