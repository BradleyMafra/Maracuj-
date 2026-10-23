"""Private test dependency paths; import by file location when Python uses -I.

This module never launches a process or downloads/installs a dependency. Windows
tests use their application bundle, rather than an unrelated executable on PATH.
"""

from __future__ import annotations

import json
import os
from pathlib import Path


def default_application(project: Path) -> Path:
    name = "maracuja.exe" if os.name == "nt" else "maracuja"
    candidates = (project / "build" / name, project / "build/Release" / name)
    return next((path for path in candidates if path.is_file()), candidates[0])


class TestRuntime:
    def __init__(self, application: Path):
        application = application.resolve()
        override = os.environ.get("MARACUJA_RUNTIME_CONFIG")
        self.configuration = Path(override).resolve() if override else application.parent / "runtime-paths.json"
        self.root = self.configuration.parent
        self.values = {}
        try:
            if self.configuration.is_file() and self.configuration.stat().st_size <= 64 * 1024:
                parsed = json.loads(self.configuration.read_text(encoding="utf-8"))
                if isinstance(parsed, dict):
                    self.values.update(parsed)
        except (OSError, UnicodeError, ValueError):
            raise AssertionError("test runtime configuration is invalid") from None

    def configured(self, name: str) -> Path | None:
        value = self.values.get(name)
        tools = self.values.get("tools")
        if not isinstance(value, str) and isinstance(tools, dict):
            value = tools.get(name)
        if not isinstance(value, str) or not value:
            return None
        path = Path(value)
        return path.resolve() if path.is_absolute() else (self.root / path).resolve()

    def tool(self, name: str) -> Path:
        explicit = self.configured(name)
        if explicit is not None:
            if not explicit.is_file():
                raise AssertionError("configured test tool is missing: " + name)
            return explicit
        suffix = ".exe" if os.name == "nt" else ""
        configured_tools = self.values.get("tools")
        tools = Path(configured_tools) if isinstance(configured_tools, str) and configured_tools else Path("tools")
        if not tools.is_absolute():
            tools = self.root / tools
        candidates = [tools / (name + suffix), tools / "bin" / (name + suffix)]
        if os.name != "nt":
            candidates.append(Path("/usr/bin") / name)
        result = next((path for path in candidates if path.is_file()), None)
        if result is None:
            raise AssertionError("required test tool is missing from runtime: " + name)
        return result.resolve()

    def fixture_font(self) -> Path:
        # CI may bundle DejaVu Sans for identical fixtures on both platforms.
        # Arial is a legible Unicode fallback on a clean Windows installation;
        # OCR assertions, including accents and page markers, remain unchanged.
        explicit = os.environ.get("MARACUJA_TEST_FONT")
        configured = self.configured("fixture_font")
        candidates = []
        if explicit:
            path = Path(explicit)
            candidates.append(path if path.is_absolute() else self.root / path)
        if configured is not None:
            candidates.append(configured)
        candidates.extend([self.root / "fonts/DejaVuSans.ttf",
                           self.root / "tests/fonts/DejaVuSans.ttf",
                           Path(__file__).resolve().parent / "fonts/DejaVuSans.ttf"])
        if os.name == "nt":
            windows = Path(os.environ.get("WINDIR") or os.environ.get("SystemRoot") or "C:/Windows")
            candidates.extend([windows / "Fonts/DejaVuSans.ttf", windows / "Fonts/arial.ttf"])
        else:
            candidates.extend([Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
                               Path("/usr/share/fonts/dejavu/DejaVuSans.ttf")])
        result = next((path for path in candidates if path.is_file()), None)
        if result is None:
            raise AssertionError("a readable Unicode OCR fixture font is unavailable")
        return result.resolve()

    @staticmethod
    def process_options() -> dict:
        return {"creationflags": 0x08000000} if os.name == "nt" else {}
