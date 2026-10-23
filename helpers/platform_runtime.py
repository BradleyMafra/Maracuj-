"""Resolve private conversion dependencies without consulting a document's PATH/cwd."""

from __future__ import annotations

import json
import os
from pathlib import Path
import xml.etree.ElementTree as ET


class Runtime:
    def __init__(self, root: str = ""):
        self.root = Path(root).resolve() if root else Path(__file__).resolve().parent
        self.windows = os.name == "nt"
        self.values = {}
        settings = self.root / "runtime-paths.json"
        try:
            if settings.is_file() and settings.stat().st_size <= 64 * 1024:
                parsed = json.loads(settings.read_text(encoding="utf-8"))
                if isinstance(parsed, dict):
                    self.values.update({k: v for k, v in parsed.items() if isinstance(v, str)})
                    if isinstance(parsed.get("tools"), dict):
                        self.values.update({k: v for k, v in parsed["tools"].items() if isinstance(v, str)})
        except (OSError, UnicodeError, ValueError):
            pass

    def configured(self, key: str) -> Path | None:
        value = self.values.get(key)
        if not value:
            return None
        candidate = Path(value)
        if not candidate.is_absolute():
            candidate = self.root / candidate
        return candidate.resolve()

    def tool(self, key: str, explicit: str = "") -> Path | None:
        if explicit:
            candidate = Path(explicit)
            if not candidate.is_absolute():
                candidate = self.root / candidate
            return candidate.resolve() if candidate.is_file() else None
        candidate = self.configured(key)
        if candidate is not None:
            return candidate if candidate.is_file() else None
        if self.windows:
            names = {
                "soffice": ["runtime/libreoffice/program/soffice.com", "runtime/libreoffice/program/soffice.exe"],
                "python": ["runtime/python/python.exe"],
                "tesseract_library": ["ocr/lib/libtesseract-5.5.dll", "ocr/lib/libtesseract-5.dll", "ocr/lib/libtesseract.dll",
                                      "ocr/lib/tesseract53.dll", "ocr/lib/tesseract54.dll"],
            }.get(key, ["tools/" + key + ".exe", "tools/bin/" + key + ".exe"])
            return next((self.root / name for name in names if (self.root / name).is_file()), None)
        if key == "tesseract_library":
            candidates = [self.root / "ocr/lib/libtesseract.so.5",
                          Path("/usr/lib/x86_64-linux-gnu/libtesseract.so.5"),
                          Path("/lib/x86_64-linux-gnu/libtesseract.so.5")]
            return next((path for path in candidates if path.is_file()), None)
        candidates = [self.root / "tools" / key, Path("/usr/bin") / key]
        return next((path for path in candidates if path.is_file()), None)

    def tessdata(self, explicit: str = "") -> Path:
        if explicit:
            candidate = Path(explicit)
            return candidate.resolve() if candidate.is_absolute() else (self.root / candidate).resolve()
        return self.configured("tessdata") or self.root / "ocr/tessdata"

    def subprocess_options(self) -> dict:
        # Inherit the app-owned Job Object. Never launch a console or break away
        # into a process tree that the application's cancellation cannot control.
        return {"creationflags": 0x08000000} if self.windows else {}

    def dll_directories(self, library: Path) -> list[Path]:
        return list(dict.fromkeys([library.parent.resolve(), (self.root / "tools").resolve(),
                                   (self.root / "bin").resolve(),
                                   (self.root / "ocr/lib").resolve()]))


def office_root(executable: str | Path | None) -> Path | None:
    if not executable:
        return None
    path = Path(executable).resolve()
    candidates = [path]
    if len(path.parents) >= 3:
        candidates.append(path.parents[2] / "native/libreoffice-headless/libreoffice/program/soffice")
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK) and candidate.parent.name.lower() == "program":
            return candidate.parent.parent
    return None


def markdown_filter(root: Path | None) -> bool:
    if root is None:
        return False
    registry = root / "share/registry/writer.xcd"
    try:
        if not registry.is_file() or registry.stat().st_size > 16 * 1024 * 1024:
            return False
        attribute = "{http://openoffice.org/2001/registry}name"
        for node in ET.parse(registry).getroot().iter("node"):
            if node.get(attribute) != "Markdown":
                continue
            for prop in node.findall("prop"):
                if prop.get(attribute) == "Flags":
                    flags = " ".join(value.text or "" for value in prop.findall("value")).split()
                    return "IMPORT" in flags and "EXPORT" in flags
    except (OSError, ET.ParseError, ValueError):
        pass
    return False
