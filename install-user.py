#!/usr/bin/python3
"""Install Maracujá for the current user, retaining its existing desktop IDs."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--no-build", action="store_true")
parser.add_argument("--enable-autostart", action="store_true",
                    help="Iniciar o Maracujá em segundo plano ao entrar no KDE.")
parser.add_argument("--runtime-source", type=Path,
                    help="Pasta de dependências com python/ e native/libreoffice-headless/.")
options = parser.parse_args()
project = Path(__file__).resolve().parent
home = Path.home()


def desktop_string(value):
    """Escape a Desktop Entry string before its optional Exec parsing."""
    return (value.replace("\\", "\\\\").replace("\n", "\\n")
            .replace("\r", "\\r").replace("\t", "\\t"))


def launcher_argument():
    # Exec uses Desktop Entry quoting, not shell quoting. The string escape
    # layer is decoded before the quoted argument and field codes are parsed.
    value = str(home / ".local/bin/maracuja").replace("%", "%%")
    value = "".join("\\" + char if char in '\\"`$' else char for char in value)
    return desktop_string('"' + value + '"')


def desktop_template(name):
    placeholder = "@MARACUJA_COMMAND@"
    result = []
    for line in (project / "integration" / name).read_text().splitlines():
        if line.startswith("Exec=" + placeholder):
            line = "Exec=" + launcher_argument() + line[len("Exec=" + placeholder):]
        elif line == "TryExec=" + placeholder:
            line = "TryExec=" + desktop_string(str(home / ".local/bin/maracuja"))
        result.append(line)
    return ("\n".join(result) + "\n").encode()


if not options.no_build:
    subprocess.run(["cmake", "-S", str(project), "-B", str(project / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", str(project / "build"), "--parallel", "1"], check=True)
binary = project / "build/maracuja"
assert binary.is_file(), "Compile o aplicativo antes de instalar."
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H%M%S%fZ")
backup = project / "validation" / ("install-" + stamp)
backup.mkdir(parents=True, mode=0o700)


def prepare_document_runtime():
    # Keep a private snapshot: a cache cleanup or Codex update must not break
    # conversions started from Dolphin or at the next login. No system packages
    # or the user's Office profile are changed. Copy streams, never holds the
    # runtime in RAM, and publishes only after both executables validate.
    existing = home / ".local/lib/mandarina/runtime-paths.json"
    if existing.is_file() and options.runtime_source is None:
        values = json.loads(existing.read_text())
        if all(Path(values.get(key, "")).is_file() for key in ("python", "soffice")):
            return {key: values[key] for key in ("python", "soffice")}
    target = home / ".local/lib/mandarina/runtime-local"
    python = target / "python/bin/python3"
    soffice = target / "libreoffice/libreoffice/program/soffice"
    if not (target / ".complete").is_file():
        assert options.runtime_source is not None, (
            "Use o instalador do pacote Linux ou forneça --runtime-source explicitamente.")
        source = options.runtime_source
        assert (source / "python/bin/python3").is_file(), "Runtime Python de documentos ausente."
        assert (source / "native/libreoffice-headless/libreoffice/program/soffice").is_file(), "Runtime LibreOffice ausente."
        target.parent.mkdir(parents=True, exist_ok=True)
        assert not target.exists(), "Runtime incompleto existente: preserve-o e confira antes de substituir."
        with tempfile.TemporaryDirectory(prefix=".maracuja-runtime-", dir=target.parent) as private:
            staging = Path(private) / "runtime"
            staging.mkdir()
            shutil.copytree(source / "python", staging / "python", symlinks=True,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
            shutil.copytree(source / "native/libreoffice-headless", staging / "libreoffice", symlinks=True)
            subprocess.run([str(staging / "python/bin/python3"), "-I", "-c",
                            "import docx,pptx,openpyxl,pdfplumber,PIL"], check=True,
                           capture_output=True, timeout=30)
            subprocess.run([str(staging / "libreoffice/libreoffice/program/soffice"), "--version"],
                           check=True, capture_output=True, timeout=30)
            (staging / ".complete").write_text("Maracujá private document runtime\n")
            os.rename(staging, target)
    return {"python": str(python), "soffice": str(soffice)}


runtime_paths = prepare_document_runtime()
model_folder = project / "vendor/tessdata-fast-4.1.0"
model_hashes = {
    "por.traineddata": "c4932b937207a9514b7514d518b931a99938c02a28a5a5a553f8599ed58b7deb",
    "eng.traineddata": "7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2",
}
for name, expected_hash in model_hashes.items():
    assert hashlib.sha256((model_folder / name).read_bytes()).hexdigest() == expected_hash, "Modelo OCR ausente ou divergente; instalação interrompida."
runtime_paths["tessdata"] = str(home / ".local/lib/mandarina/ocr/tessdata")
files = {
    home / ".local/lib/mandarina/maracuja": (binary.read_bytes(), 0o755),
    home / ".local/lib/mandarina/maracuja-gesture-helper": ((project / "build/maracuja-gesture-helper").read_bytes(), 0o755),
    home / ".local/lib/mandarina/document_convert.py": ((project / "helpers/document_convert.py").read_bytes(), 0o644),
    home / ".local/lib/mandarina/file_tools.py": ((project / "helpers/file_tools.py").read_bytes(), 0o644),
    home / ".local/lib/mandarina/platform_runtime.py": ((project / "helpers/platform_runtime.py").read_bytes(), 0o644),
    home / ".local/lib/mandarina/ocr_engine.py": ((project / "helpers/ocr_engine.py").read_bytes(), 0o644),
    home / ".local/lib/mandarina/runtime-paths.json": ((json.dumps(runtime_paths, indent=2) + "\n").encode(), 0o600),
    home / ".local/bin/mandarina": (("#!/bin/sh\nexec " + "'" + str(home / ".local/lib/mandarina/maracuja").replace("'", "'\\''") + "'" + ' "$@"\n').encode(), 0o755),
    home / ".local/bin/maracuja": (("#!/bin/sh\nexec " + "'" + str(home / ".local/lib/mandarina/maracuja").replace("'", "'\\''") + "'" + ' "$@"\n').encode(), 0o755),
    home / ".local/share/applications/org.winux.Mandarina.desktop": (desktop_template("org.winux.Mandarina.desktop"), 0o644),
    home / ".local/share/kio/servicemenus/mandarina-convert.desktop": (desktop_template("mandarina-convert.desktop"), 0o755),
    home / ".local/share/icons/hicolor/scalable/apps/org.winux.Mandarina.svg": ((project / "assets/org.winux.Mandarina.svg").read_bytes(), 0o644),
    home / ".local/share/icons/hicolor/scalable/apps/org.winux.Maracuja.svg": ((project / "assets/org.winux.Mandarina.svg").read_bytes(), 0o644),
}
for name in (*model_hashes, "LICENSE", "manifest.json"):
    files[home / ".local/lib/mandarina/ocr/tessdata" / name] = ((model_folder / name).read_bytes(), 0o644)
autostart_path = home / ".config/autostart/org.winux.Mandarina.desktop"
autostart_created = options.enable_autostart and not autostart_path.exists()
if options.enable_autostart:
    files[autostart_path] = (desktop_template("org.winux.Mandarina-autostart.desktop"), 0o644)
effect = project / "integration/kwin/mandarina-shift"
files[home / ".local/share/kwin/effects/mandarina-shift/window-hints.js"] = ((project / "integration/kwin/window-hints.js").read_bytes(), 0o644)
for source in effect.rglob("*"):
    if source.is_file():
        destination = home / ".local/share/kwin/effects/mandarina-shift" / source.relative_to(effect)
        files[destination] = (source.read_bytes(), 0o644)
installed = []
for number, (destination, (data, mode)) in enumerate(files.items()):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        shutil.copy2(destination, backup / (str(number) + "-" + destination.name))
    fd, temporary = tempfile.mkstemp(prefix=".maracuja-install-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    installed.append(str(destination))
subprocess.run(["desktop-file-validate", str(home / ".local/share/applications/org.winux.Mandarina.desktop")], check=True)
if options.enable_autostart:
    subprocess.run(["desktop-file-validate", str(autostart_path)], check=True)
for command in [["update-desktop-database", str(home / ".local/share/applications")], ["kbuildsycoca6"]]:
    if shutil.which(command[0]):
        subprocess.run(command, check=True, capture_output=True, timeout=30)
receipt = {"display_name": "Maracujá", "preferred_command": str(home / ".local/bin/maracuja"),
           "version": "0.9.5", "document_runtime": runtime_paths,
           "ocr_languages": ["por", "eng"], "ocr_model_version": "tessdata_fast 4.1.0",
           "installed": installed, "autostart_created": autostart_created,
           "autostart_changed": options.enable_autostart,
           "default_associations_changed": False, "backup": str(backup)}
(backup / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
(project / "validation/installed.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps(receipt, indent=2))
