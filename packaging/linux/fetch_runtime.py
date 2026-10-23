#!/usr/bin/env python3
"""Stage verified, private runtimes; never install anything in the user's OS.

Run only in a disposable build directory/CI. Downloads are exact SHA256 locked.
Windows tools are extracted from official MinGW64 packages, not installed with
pacman. Existing staging directories are rejected to avoid mixing old versions.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.parse
import urllib.request

PROJECT = Path(__file__).resolve().parents[2]
LOCK = json.loads((Path(__file__).with_name("runtime.lock.json")).read_text())


def call(*arguments, **kwargs):
    subprocess.run(list(map(str, arguments)), check=True, **kwargs)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def fetch(record, cache):
    name = urllib.parse.unquote(urllib.parse.urlparse(record["url"]).path.rsplit("/", 1)[-1])
    destination = cache / name
    if destination.is_file() and digest(destination) == record["sha256"]:
        return destination
    partial = destination.with_name(destination.name + ".partial")
    headers = {"User-Agent": "Maracuja-release-builder/0.7"}
    if "api.github.com/repos/" in record["url"] and "/releases/assets/" in record["url"]:
        headers["Accept"] = "application/octet-stream"
        headers["X-GitHub-Api-Version"] = "2022-11-28"
    request = urllib.request.Request(record["url"], headers=headers)
    with urllib.request.urlopen(request, timeout=90) as response, partial.open("wb") as output:
        shutil.copyfileobj(response, output, 1024 * 1024)
    if digest(partial) != record["sha256"]:
        partial.unlink(missing_ok=True)
        raise RuntimeError("SHA256 mismatch for locked dependency " + name)
    partial.replace(destination)
    return destination


def untar(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as stream:
        # Python 3.12's data filter rejects paths/devices outside this directory.
        stream.extractall(destination, filter="data")


def prepare_python(platform, stage, cache):
    runtime = stage / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    untar(fetch(LOCK["python_" + platform], cache), runtime)
    python = runtime / "python" / ("python.exe" if platform == "windows" else "bin/python3")
    if not python.is_file():
        raise RuntimeError("Private Python executable missing after extraction")
    call(python, "-I", "-m", "pip", "--isolated", "install", "--disable-pip-version-check", "--only-binary=:all:",
         "--require-hashes", "-r", PROJECT / "packaging/linux/python-requirements.lock")
    call(python, "-I", "-c", "import docx,pptx,openpyxl,pdfplumber,PIL,pypdf")
    return python


def prepare_office(platform, stage, cache, work):
    archive = fetch(LOCK["libreoffice_" + platform], cache)
    unpacked = work / "office"
    unpacked.mkdir()
    if platform == "windows":
        # Administrative image extraction, not product installation/registration.
        call("msiexec.exe", "/a", archive, "/qn", "TARGETDIR=" + str(unpacked.resolve()))
        candidates = list(unpacked.rglob("program/soffice.exe"))
    else:
        untar(archive, unpacked)
        expanded = work / "deb-root"
        expanded.mkdir()
        for package in sorted(unpacked.rglob("*.deb")):
            call("dpkg-deb", "-x", package, expanded)
        candidates = list(expanded.glob("opt/libreoffice*/program/soffice"))
    if len(candidates) != 1:
        raise RuntimeError("LibreOffice installation root is ambiguous/missing")
    destination = stage / "runtime/libreoffice"
    shutil.copytree(candidates[0].parent.parent, destination, symlinks=True)
    return destination / "program" / ("soffice.exe" if platform == "windows" else "soffice")


def prepare_models(stage):
    source = PROJECT / "vendor/tessdata-fast-4.1.0"
    manifest = json.loads((source / "manifest.json").read_text())
    destination = stage / "ocr/tessdata"
    destination.mkdir(parents=True)
    for entry in manifest["files"]:
        path = source / entry["file"]
        if digest(path) != entry["sha256"]:
            raise RuntimeError("OCR model/license hash mismatch: " + entry["file"])
        shutil.copy2(path, destination / path.name)
    shutil.copy2(source / "manifest.json", destination / "manifest.json")


def enable_windows_utf8_manifest(executable):
    """Preserve the PE manifest and make Poppler's UTF-8 paths work on Windows.

    The locked MSYS2 tools mix UTF-8 paths with Windows ANSI APIs. Without a
    UTF-8 process code page, simply installing them below an accented folder
    makes PDF operations exit silently. Windows 10 1903+ supports this manifest
    setting without changing the user's system locale.
    """
    import ctypes
    from ctypes import wintypes
    import xml.etree.ElementTree as ET

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    pointer = ctypes.c_void_p
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceExW.argtypes = [wintypes.HMODULE, pointer, pointer, wintypes.WORD]
    kernel.FindResourceExW.restype = wintypes.HANDLE
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = [wintypes.HANDLE]
    kernel.LockResource.restype = pointer
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    language_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE, pointer, pointer,
                                          wintypes.WORD, ctypes.c_ssize_t)
    kernel.EnumResourceLanguagesW.argtypes = [wintypes.HMODULE, pointer, pointer,
                                              language_callback, ctypes.c_ssize_t]
    kernel.EnumResourceLanguagesW.restype = wintypes.BOOL
    kernel.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    kernel.BeginUpdateResourceW.restype = wintypes.HANDLE
    kernel.UpdateResourceW.argtypes = [wintypes.HANDLE, pointer, pointer, wintypes.WORD,
                                       pointer, wintypes.DWORD]
    kernel.UpdateResourceW.restype = wintypes.BOOL
    kernel.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    kernel.EndUpdateResourceW.restype = wintypes.BOOL

    # LOAD_LIBRARY_AS_DATAFILE reads resources without executing the program.
    module = kernel.LoadLibraryExW(str(executable), None, 2)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    manifests = {}
    languages = []

    @language_callback
    def collect_language(_module, _type, _name, language, _parameter):
        languages.append(language)
        return True

    try:
        if not kernel.EnumResourceLanguagesW(module, 24, 1, collect_language, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        for language in languages:
            resource = kernel.FindResourceExW(module, 24, 1, language)
            if not resource:
                raise ctypes.WinError(ctypes.get_last_error())
            loaded = kernel.LoadResource(module, resource)
            address = kernel.LockResource(loaded) if loaded else None
            if not address:
                raise ctypes.WinError(ctypes.get_last_error())
            manifests[language] = ctypes.string_at(address, kernel.SizeofResource(module, resource))
    finally:
        kernel.FreeLibrary(module)

    updated = {}
    namespace = "{urn:schemas-microsoft-com:asm.v3}"
    codepage_tag = "{http://schemas.microsoft.com/SMI/2019/WindowsSettings}activeCodePage"
    for language, data in manifests.items():
        document = ET.fromstring(data)
        application = document.find(namespace + "application")
        if application is None:
            application = ET.SubElement(document, namespace + "application")
        settings = application.find(namespace + "windowsSettings")
        if settings is None:
            settings = ET.SubElement(application, namespace + "windowsSettings")
        codepage = settings.find(codepage_tag)
        if codepage is None:
            codepage = ET.SubElement(settings, codepage_tag)
        codepage.text = "UTF-8"
        updated[language] = ET.tostring(document, encoding="utf-8", xml_declaration=True)

    handle = kernel.BeginUpdateResourceW(str(executable), False)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        for language, data in updated.items():
            buffer = ctypes.create_string_buffer(data)
            if not kernel.UpdateResourceW(handle, 24, 1, language, buffer, len(data)):
                raise ctypes.WinError(ctypes.get_last_error())
    except BaseException:
        kernel.EndUpdateResourceW(handle, True)
        raise
    else:
        if not kernel.EndUpdateResourceW(handle, False):
            raise ctypes.WinError(ctypes.get_last_error())


def prepare_mingw(stage, cache, work):
    lock = json.loads((PROJECT / "packaging/windows/msys2-packages.lock.json").read_text())
    prefix = work / "mingw"
    prefix.mkdir()
    import zstandard
    for package in lock["packages"]:
        archive = fetch(package, cache)
        with archive.open("rb") as source, zstandard.ZstdDecompressor().stream_reader(source) as decoded:
            with tarfile.open(fileobj=decoded, mode="r|") as contents:
                contents.extractall(prefix, filter="data")
    binary_directory = prefix / "mingw64/bin"
    tools = stage / "tools"
    tools.mkdir()
    roots = [binary_directory / name for name in
             ["ffmpeg.exe", "ffprobe.exe", "pdfinfo.exe", "pdftoppm.exe", "pdftotext.exe", "pdfunite.exe"]]
    tess = sorted(binary_directory.glob("libtesseract-*.dll"))
    if len(tess) != 1:
        raise RuntimeError("Tesseract DLL missing/ambiguous")
    roots += tess
    # MSVC dumpbin reads PE imports without executing third-party programs. Copy
    # only the DLL closure actually required by our selected tools, not the
    # hundreds of optional executables/plugins contained in all packages.
    dumpbin = shutil.which("dumpbin.exe")
    objdump = shutil.which("objdump.exe")
    if not dumpbin and not objdump:
        raise RuntimeError("Provide Visual Studio dumpbin or the private MinGW objdump")
    import re
    copied = set()
    pending = roots[:]
    index = {path.name.casefold(): path for path in binary_directory.glob("*.dll")}
    while pending:
        source = pending.pop()
        if source.name.casefold() in copied:
            continue
        if not source.is_file():
            raise RuntimeError("Required executable/DLL absent: " + source.name)
        if source.name.casefold().startswith("msys-"):
            raise RuntimeError("An MSYS runtime must not enter the native Windows package")
        copied.add(source.name.casefold())
        shutil.copy2(source, tools / source.name)
        inspect = [dumpbin, "/DEPENDENTS", str(source)] if dumpbin else [objdump, "-p", str(source)]
        result = subprocess.run(inspect, check=True,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        expression = r"^\s+([\w.+-]+\.dll)\s*$" if dumpbin else r"^\s*DLL Name:\s*([^\s]+)"
        for name in re.findall(expression, result.stdout, re.MULTILINE | re.IGNORECASE):
            if name.casefold() in index:
                pending.append(index[name.casefold()])
            elif not (Path(os.environ["SystemRoot"]) / "System32" / name).is_file() and not name.lower().startswith(("api-ms-", "ext-ms-")):
                raise RuntimeError("Unresolved native DLL import: " + name)
    for name in ["pdfinfo.exe", "pdftoppm.exe", "pdftotext.exe", "pdfunite.exe"]:
        enable_windows_utf8_manifest(tools / name)
    ocr = stage / "ocr/lib"
    ocr.mkdir()
    shutil.copy2(tess[0], ocr / tess[0].name)
    # Poppler's character maps and upstream notices are data, not executable code.
    data = prefix / "mingw64/share/poppler"
    if data.is_dir():
        shutil.copytree(data, tools / "share/poppler")
    licenses = prefix / "mingw64/share/licenses"
    if licenses.is_dir():
        shutil.copytree(licenses, stage / "licenses/MSYS2")
    shutil.copy2(PROJECT / "packaging/windows/msys2-packages.lock.json", stage / "licenses/msys2-packages.lock.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=["windows", "linux"], required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    arguments = parser.parse_args()
    stage, cache = arguments.stage.resolve(), arguments.cache.resolve()
    if (stage / "runtime").exists() or (stage / "ocr").exists():
        raise RuntimeError("Runtime staging must be fresh; refusing to mix versions")
    stage.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    (stage / "licenses").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="maracuja-dependencies-") as temporary:
        work = Path(temporary)
        prepare_python(arguments.platform, stage, cache)
        prepare_office(arguments.platform, stage, cache, work)
        prepare_models(stage)
        if arguments.platform == "windows":
            prepare_mingw(stage, cache, work)
    for name in ["document_convert.py", "file_tools.py", "ocr_engine.py", "platform_runtime.py"]:
        shutil.copy2(PROJECT / "helpers" / name, stage / name)
    if arguments.platform == "windows":
        shutil.copy2(PROJECT / "helpers/windows_office_profile.py", stage / "windows_office_profile.py")
    settings = {
        "python": "runtime/python/python.exe" if arguments.platform == "windows" else "runtime/python/bin/python3",
        "soffice": "runtime/libreoffice/program/soffice.com" if arguments.platform == "windows" else "runtime/libreoffice/program/soffice",
        "tessdata": "ocr/tessdata", "office_isolated": arguments.platform == "linux",
        "office_isolation": "bubblewrap" if arguments.platform == "linux" else "active-content-rejection",
        "tools": {name: "tools/" + name + (".exe" if arguments.platform == "windows" else "")
                  for name in ["ffmpeg", "ffprobe", "pdfinfo", "pdftoppm", "pdftotext", "pdfunite"]},
        "tesseract_library": "ocr/lib/libtesseract-5.5.dll" if arguments.platform == "windows" else "ocr/lib/libtesseract.so.5",
        "portable": True,
    }
    if arguments.platform == "linux":
        settings["tools"]["bwrap"] = "tools/bwrap"
    (stage / "runtime-paths.json").write_text(json.dumps(settings, indent=2) + "\n")
    for path in [Path(__file__).with_name("runtime.lock.json"),
                 Path(__file__).with_name("python-packages.lock.json"),
                 Path(__file__).with_name("python-requirements.lock")]:
        shutil.copy2(path, stage / "licenses" / path.name)
    print(json.dumps({"stage": str(stage), "platform": arguments.platform, "runtimes_verified": True}))


if __name__ == "__main__":
    main()
