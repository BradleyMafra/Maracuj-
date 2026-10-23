#!/usr/bin/env python3
"""Build a Linux x86-64 AppDir, portable tar and optionally a verified AppImage.

Target baseline: Ubuntu 24.04/glibc 2.39 or newer, KDE Plasma 6 for the Shift
gesture. This builder uses only explicit distribution dependencies and locked
public runtimes; no Codex cache paths or local user application data are copied.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

import importlib.util
_spec = importlib.util.spec_from_file_location("maracuja_packaging_fetch", Path(__file__).with_name("fetch_runtime.py"))
_fetch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_fetch)
LOCK, digest, fetch = _fetch.LOCK, _fetch.digest, _fetch.fetch
_privacy_spec = importlib.util.spec_from_file_location("maracuja_archive_privacy", Path(__file__).resolve().parents[1] / "archive_privacy.py")
_privacy = importlib.util.module_from_spec(_privacy_spec)
_privacy_spec.loader.exec_module(_privacy)

PROJECT = Path(__file__).resolve().parents[2]


def call(*arguments, **kwargs):
    subprocess.run(list(map(str, arguments)), check=True, **kwargs)


def elf(path):
    try:
        with path.open("rb") as source:
            return source.read(4) == b"\x7fELF"
    except OSError:
        return False


def distribution_license_source(name, directory=Path("/usr/share/common-licenses")):
    # Some copyright summaries use SPDX's .0 spelling while Debian names
    # these same major-version license texts without it. Do not guess for
    # other names/versions: an unavailable referenced text must stop packaging.
    aliases = {"GPL-1.0": "GPL-1", "GPL-2.0": "GPL-2", "GPL-3.0": "GPL-3",
               "LGPL-3.0": "LGPL-3"}
    source = directory / name
    if not source.is_file() and name in aliases:
        source = directory / aliases[name]
    if not source.is_file():
        raise RuntimeError("Referenced distribution license is unavailable: " + name)
    return source


def copy_shared_libraries(stage):
    destination = stage / "lib"
    # This directory is generated solely by this builder. A resumed attempt must
    # not compare already-patched ELF bytes with pristine distro libraries.
    if destination.exists():
        if destination.is_symlink():
            raise RuntimeError("Refusing to reuse a symbolic library directory")
        shutil.rmtree(destination)
    destination.mkdir()
    # The Java AWT embedding bridge is unrelated to headless Writer conversion.
    # Do not require/bundle a JRE just to satisfy this unused optional module.
    (stage / "runtime/libreoffice/program/libofficebean.so").unlink(missing_ok=True)
    # glibc and its loader must come from the baseline-compatible host. Mixing
    # loaders/glibc is unsafe and graphics drivers remain the host's responsibility.
    excluded = re.compile(r"^(ld-linux|lib(?:c|m|pthread|dl|rt|resolv|util|anl)\.so|libnss_(?:compat|dns|files|hesiod))")
    candidates = [path for path in stage.rglob("*") if path.is_file() and elf(path)]
    copied = set()
    source_paths = set()
    for binary in candidates:
        result = subprocess.run(["ldd", str(binary)], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, check=False)
        if "not found" in result.stdout:
            raise RuntimeError("Missing ELF dependency while packaging " + binary.name)
        for name, source in re.findall(r"^\s*(\S+)\s+=>\s+(/\S+)\s+\(", result.stdout, re.MULTILINE):
            if excluded.match(name):
                continue
            path = Path(source).resolve()
            if path.is_relative_to(stage):
                continue
            if (destination / name).exists() and digest(destination / name) != digest(path):
                raise RuntimeError("Conflicting ELF SONAME: " + name)
            if name in copied:
                continue
            shutil.copy2(path, destination / name)
            copied.add(name)
            source_paths.add(str(path))
    for binary in destination.iterdir():
        if elf(binary):
            call("patchelf", "--set-rpath", "$ORIGIN", binary)
    for binary in [stage / "maracuja", stage / "maracuja-gesture-helper"]:
        if binary.exists():
            call("patchelf", "--set-rpath", "$ORIGIN/lib", binary)
    for binary in (stage / "tools").iterdir():
        if binary.is_file() and elf(binary):
            call("patchelf", "--set-rpath", "$ORIGIN/../lib", binary)
    call("patchelf", "--set-rpath", "$ORIGIN/../../lib", stage / "ocr/lib/libtesseract.so.5")
    return source_paths


def distribution_notices(stage, source_paths):
    owners = set()
    paths = sorted(source_paths)
    for index in range(0, len(paths), 100):
        result = subprocess.run(["dpkg-query", "-S", *paths[index:index + 100]],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=False)
        for line in result.stdout.splitlines():
            if ": " in line:
                owners.update(line.split(": ", 1)[0].split(", "))
    package_report = ""
    if owners:
        package_report = subprocess.run(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n", *sorted(owners)],
                                        check=True, stdout=subprocess.PIPE, text=True).stdout
    (stage / "licenses/ubuntu-packages.txt").write_text(package_report)
    sources = []
    notices = stage / "licenses/Ubuntu"
    notices.mkdir(exist_ok=True)
    common_notices = stage / "licenses/common"
    common_notices.mkdir(exist_ok=True)
    for line in package_report.splitlines():
        fields = line.split("\t")
        if len(fields) == 4:
            package, version, source, source_version = fields
            sources.append({"binary_package": package, "version": version, "source": source,
                            "source_version": source_version,
                            "source_retrieval": "apt-get source " + source + "=" + source_version,
                            "repository_requirement": "Use the matching deb-src repository from the build distribution; do not substitute another version."})
            copyright_file = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
            if copyright_file.is_file():
                shutil.copy2(copyright_file, notices / (package.replace(":", "-") + ".copyright"))
                # Debian copyright summaries often reference full GPL/LGPL and
                # Apache texts installed separately by the distribution.
                # Mirror those texts so a portable package is self-contained.
                for name in re.findall(r"/usr/share/common-licenses/([A-Za-z0-9.+-]+)",
                                       copyright_file.read_text(encoding="utf-8")):
                    name = name.rstrip(".")
                    common_source = distribution_license_source(name)
                    # Keep the referenced filename, even when its source used
                    # an explicit alias, so the packaged notice still resolves.
                    shutil.copy2(common_source, common_notices / name)
    (stage / "licenses/distribution-sources.json").write_text(json.dumps(sources, indent=2) + "\n")


def copy_qt_runtime(qt, stage):
    # A development host may also contain Plasma/KIO/WebEngine plugin trees.
    # Include only the modules needed by Main.qml and Qt's own Basic controls.
    plugin_names = {"platforms", "imageformats", "iconengines", "tls", "networkinformation",
                    "wayland-decoration-client", "wayland-graphics-integration-client",
                    "wayland-shell-integration", "xcbglintegrations"}
    destination = stage / "plugins"
    destination.mkdir()
    for name in sorted(plugin_names):
        if (qt / "plugins" / name).is_dir():
            shutil.copytree(qt / "plugins" / name, destination / name, symlinks=True)
    imports = stage / "qml"
    imports.mkdir()
    for family, modules in {
        "QtQml": {"Models", "WorkerScript"},
        "QtQuick": {"Controls", "Dialogs", "Layouts", "Templates", "Window", "Effects", "Shapes"},
    }.items():
        source = qt / "qml" / family
        target = imports / family
        target.mkdir()
        for item in source.iterdir():
            if item.is_file():
                shutil.copy2(item, target / item.name)
            elif item.name in modules:
                shutil.copytree(item, target / item.name, symlinks=True,
                                ignore=shutil.ignore_patterns("org.kde.*", "KDE*", "Plasma*"))


def copy_qt_sboms(qt, stage):
    """Retain available upstream SBOMs for the Qt modules actually bundled.

    These describe complete upstream modules, including build-only components;
    shipped-files.json remains the inventory of our actual release payload.
    """
    result = subprocess.run(["qmake6", "-query", "QT_VERSION"], check=True,
                            stdout=subprocess.PIPE, text=True)
    version = result.stdout.strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise RuntimeError("Cannot identify the bundled Qt version for its SBOM")
    copied = set()
    for module, library in [("qtbase", "libQt6Core.so.6"), ("qtdeclarative", "libQt6Qml.so.6")]:
        source = qt / "sbom" / (module + "-" + version + ".spdx")
        if (stage / "lib" / library).is_file() and source.is_file():
            destination = stage / "licenses/Qt-SBOM"
            destination.mkdir(exist_ok=True)
            shutil.copy2(source, destination / source.name)
            copied.add(str(source))
    return copied


def create_appimage(stage, destination, cache):
    tool = fetch(LOCK["appimagetool"], cache)
    tool.chmod(0o755)
    # The verified appimagetool is itself a type-2 AppImage. Reuse its verified
    # runtime prefix rather than letting it download a mutable continuous asset.
    result = subprocess.run([str(tool), "--appimage-offset"], check=True,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    offset = int(result.stdout.strip())
    if not 0 < offset <= min(tool.stat().st_size, 8 * 1024 * 1024):
        raise RuntimeError("Invalid runtime offset in verified appimagetool")
    with tempfile.TemporaryDirectory(prefix="maracuja-appimagetool-") as temporary:
        temporary = Path(temporary)
        runtime = temporary / "runtime-x86_64"
        with tool.open("rb") as source:
            runtime.write_bytes(source.read(offset))
        call(tool, "--appimage-extract", cwd=temporary, stdout=subprocess.DEVNULL)
        environment = os.environ.copy()
        environment.pop("LD_LIBRARY_PATH", None)
        environment["ARCH"] = "x86_64"
        call(temporary / "squashfs-root/AppRun", "--no-appstream", "--runtime-file", runtime,
             "--mksquashfs-opt=-processors", "--mksquashfs-opt=1", "--mksquashfs-opt=-all-root", stage, destination,
             env=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="0.9.5")
    parser.add_argument("--build", type=Path, default=PROJECT / "build-linux")
    parser.add_argument("--stage", type=Path, default=PROJECT / "dist/linux/Maracuja.AppDir")
    parser.add_argument("--cache", type=Path, default=PROJECT / ".package-cache")
    parser.add_argument("--forbidden-text-json", type=Path,
                        help="Private known home paths/emails for the final privacy gate; keep outside the stage")
    parser.add_argument("--skip-compile", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--resume-stage", action="store_true",
                        help="Reuse an existing stage with matching locked runtimes; rerun all checks")
    parser.add_argument("--appimage", action="store_true")
    arguments = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:[.-][A-Za-z0-9.]+)?", arguments.version):
        raise RuntimeError("Invalid release version")
    build, stage, cache = arguments.build.resolve(), arguments.stage.resolve(), arguments.cache.resolve()
    if stage.exists() and not arguments.resume_stage:
        raise RuntimeError("Use a fresh staging directory")
    if arguments.resume_stage:
        required = ["runtime/python/bin/python3", "runtime/libreoffice/program/soffice",
                    "licenses/runtime.lock.json", "runtime-paths.json", "VERSION", "plugins", "qml"]
        if not stage.is_dir() or any(not (stage / name).exists() for name in required):
            raise RuntimeError("The existing stage is incomplete; cannot resume")
        if (stage / "VERSION").read_text().strip() != arguments.version:
            raise RuntimeError("The existing stage has a different application version")
        if json.loads((stage / "licenses/runtime.lock.json").read_text()) != LOCK:
            raise RuntimeError("The existing stage has a different dependency lock")
        settings = json.loads((stage / "runtime-paths.json").read_text())
        if (settings.get("python") != "runtime/python/bin/python3"
                or settings.get("soffice") != "runtime/libreoffice/program/soffice"
                or settings.get("portable") is not True):
            raise RuntimeError("The existing stage is not this builder's portable runtime")
    if not arguments.skip_compile:
        call("cmake", "-S", PROJECT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON")
        call("cmake", "--build", build, "--parallel", "1")
    if not arguments.resume_stage:
        call("python3", "-I", PROJECT / "packaging/linux/fetch_runtime.py", "--platform", "linux",
             "--stage", stage, "--cache", cache)
    for name in ["document_convert.py", "file_tools.py", "ocr_engine.py", "platform_runtime.py"]:
        shutil.copy2(PROJECT / "helpers" / name, stage / name)
    shutil.copy2(build / "maracuja", stage / "maracuja")
    shutil.copy2(build / "maracuja-gesture-helper", stage / "maracuja-gesture-helper")
    tools = stage / "tools"
    tools.mkdir(exist_ok=arguments.resume_stage)
    source_paths = set()
    for name in ["ffmpeg", "ffprobe", "pdfinfo", "pdftoppm", "pdftotext", "pdfunite", "bwrap"]:
        # Never copy a development runtime's PATH wrapper into a release.
        source = Path("/usr/bin") / name
        if not source.is_file() or not elf(source):
            raise RuntimeError("Required distribution tool missing: " + name)
        shutil.copy2(source, tools / name)
        source_paths.add(str(Path(source).resolve()))
    (stage / "ocr/lib").mkdir(exist_ok=arguments.resume_stage)
    tess = Path("/usr/lib/x86_64-linux-gnu/libtesseract.so.5").resolve()
    shutil.copy2(tess, stage / "ocr/lib/libtesseract.so.5")
    source_paths.add(str(tess))
    qt = Path("/usr/lib/x86_64-linux-gnu/qt6")
    if not arguments.resume_stage:
        copy_qt_runtime(qt, stage)
    source_paths.update(str(qt / "plugins" / directory.name / "*") for directory in (stage / "plugins").iterdir())
    source_paths.update(str(qt / "qml" / directory.name / "*") for directory in (stage / "qml").iterdir())
    (stage / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=plugins\nQmlImports=qml\n")
    effect = stage / "share/kwin/effects/mandarina-shift"
    shutil.copytree(PROJECT / "integration/kwin/mandarina-shift", effect, dirs_exist_ok=arguments.resume_stage)
    shutil.copy2(PROJECT / "integration/kwin/window-hints.js", effect / "window-hints.js")
    fonts = stage / "fonts"
    fonts.mkdir(exist_ok=arguments.resume_stage)
    for directory in [Path("/usr/share/fonts/truetype/dejavu"), Path("/usr/share/fonts/truetype/liberation2")]:
        if directory.is_dir():
            for path in directory.glob("*.ttf"):
                shutil.copy2(path, fonts / path.name)
                source_paths.add(str(path))
    (fonts / "fonts.conf").write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig><dir prefix="relative">.</dir><cachedir prefix="xdg">maracuja/fontconfig</cachedir></fontconfig>')
    for name in ["AppRun", "install-user.sh"]:
        shutil.copy2(Path(__file__).with_name(name), stage / name)
        (stage / name).chmod(0o755)
    shutil.copy2(PROJECT / "assets/org.winux.Mandarina.svg", stage / "org.winux.Maracuja.svg")
    if (stage / ".DirIcon").is_symlink() and os.readlink(stage / ".DirIcon") != "org.winux.Maracuja.svg":
        (stage / ".DirIcon").unlink()
    if not (stage / ".DirIcon").is_symlink():
        (stage / ".DirIcon").symlink_to("org.winux.Maracuja.svg")
    (stage / "org.winux.Mandarina.desktop").write_text("[Desktop Entry]\nType=Application\nName=Maracujá\nExec=AppRun %U\nIcon=org.winux.Maracuja\nCategories=Utility;Graphics;\nTerminal=false\n")
    (stage / "VERSION").write_text(arguments.version + "\n")
    for name in ["LICENSE", "README.md", "README_WINDOWS.md", "THIRD_PARTY_NOTICES.md", "CREDITS.md"]:
        if (PROJECT / name).is_file():
            shutil.copy2(PROJECT / name, stage / name)
    source_paths.update(copy_shared_libraries(stage))
    source_paths.update(copy_qt_sboms(qt, stage))
    os.environ["MARACUJA_RUNTIME_CONFIG"] = str(stage / "runtime-paths.json")
    os.environ["LD_LIBRARY_PATH"] = str(stage / "lib")
    os.environ["QT_PLUGIN_PATH"] = str(stage / "plugins")
    os.environ["QML2_IMPORT_PATH"] = str(stage / "qml")
    os.environ["QML_IMPORT_PATH"] = str(stage / "qml")
    # Record only owners of shipped binaries/fonts, not the host's complete app inventory.
    distribution_notices(stage, source_paths)
    if not arguments.skip_tests:
        test_environment = os.environ.copy()
        test_environment.pop("LD_LIBRARY_PATH", None)
        call("python3", "-I", PROJECT / "tests/archive_privacy_check.py", env=test_environment)
        call("python3", "-I", PROJECT / "tests/release_privacy_check.py", env=test_environment)
        call(stage / "runtime/python/bin/python3", "-I", PROJECT / "tests/platform_runtime_check.py",
             env=test_environment)
        call(stage / "runtime/python/bin/python3", "-I", PROJECT / "tests/file_tools_check.py",
             stage / "file_tools.py", env=test_environment)
        call("ctest", "--test-dir", build, "--output-on-failure", "-j1")
        for script in ["tests/integration_check.py", "tests/expanded_formats_check.py", "tests/ocr_check.py", "packaging/linux/smoke.py"]:
            call(stage / "runtime/python/bin/python3", "-I", PROJECT / script, stage / "AppRun", env=test_environment)
    # Imports during validation can regenerate caches with local source paths.
    # Sanitize after those checks and before hashing or archiving the payload.
    _privacy.sanitize_stage(stage)
    call("python3", "-I", PROJECT / "packaging/windows/license_manifest.py", "--stage", stage, "--version", arguments.version)
    privacy_arguments = ["--stage", stage]
    if arguments.forbidden_text_json:
        privacy_arguments += ["--forbidden-text-json", arguments.forbidden_text_json.resolve()]
    # The inspector must not regenerate absolute-path bytecode while checking
    # the sanitized runtime. Use its own bytecode version with cache writes off.
    privacy_environment = os.environ.copy()
    privacy_environment.pop("LD_LIBRARY_PATH", None)
    call(stage / "runtime/python/bin/python3", "-I", "-B",
         PROJECT / "packaging/check_release_privacy.py", *privacy_arguments, env=privacy_environment)
    artifacts = PROJECT / "dist/artifacts"
    artifacts.mkdir(parents=True, exist_ok=True)
    archive = artifacts / ("Maracuja-" + arguments.version + "-linux-x86_64.tar.gz")
    with tarfile.open(archive, "w:gz", compresslevel=5) as output:
        output.add(stage, arcname="Maracuja-" + arguments.version, filter=_privacy.anonymous_tarinfo)
    generated = [archive]
    if arguments.appimage:
        appimage = artifacts / ("Maracuja-" + arguments.version + "-linux-x86_64.AppImage")
        create_appimage(stage, appimage, cache)
        generated.append(appimage)
    for path in generated:
        path.with_name(path.name + ".sha256").write_text(digest(path) + "  " + path.name + "\n")
    print(json.dumps({"version": arguments.version, "artifact_directory": str(artifacts),
                      "checks_executed": not arguments.skip_tests, "manual_kde_gesture_check_required": True}))


if __name__ == "__main__":
    main()
