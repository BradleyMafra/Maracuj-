#!/usr/bin/env python3
"""Run only in a fresh private D-Bus and virtual Wayland compositor.

No existing desktop is used, queried or controlled. The only injected input is
sent to a mandarina-test-* socket created below a private temporary runtime.
App/fixture binaries must already exist; only the small C injector is compiled.
"""
from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib

PROJECT = Path(__file__).resolve().parents[1]
PROCESSES: list[subprocess.Popen] = []


def run(command, *, env=None, timeout=8):
    return subprocess.run(command, env=env, text=True, capture_output=True,
                          check=True, timeout=timeout)


def stop_owned(process):
    if process.poll() is not None:
        return
    # Every process here was started below with its own new process group.
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=2)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=2)


def spawn(command, log, *, env=None, stdin=None, stdout=None):
    process = subprocess.Popen(command, env=env, stdin=stdin,
                               stdout=stdout if stdout is not None else log,
                               stderr=log, text=True, start_new_session=True)
    PROCESSES.append(process)
    return process


def png(path):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))
    data = b"\x00" + bytes((131, 94, 188, 255)) * 32
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 24, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(data * 24)) + chunk(b"IEND", b""))


OCR_MARKER = "MARACUJA OCR 5729"


def ocr_png(path):
    from PIL import Image, ImageDraw, ImageFont
    fonts = (Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
             Path("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"))
    font_path = next((font for font in fonts if font.is_file()), None)
    if font_path is None:
        raise RuntimeError("The generated OCR fixture requires DejaVu Sans or Liberation Sans")
    image = Image.new("RGB", (1800, 600), "white")
    drawing = ImageDraw.Draw(image)
    drawing.text((90, 220), OCR_MARKER, fill="black", font=ImageFont.truetype(str(font_path), 96))
    image.save(path, format="PNG", dpi=(300, 300))
    image.close()


def ocr_drop_position(build, target, *, source_pdf=False, wheel_size=100):
    capabilities = json.loads(run([str(build / "maracuja"), "--list-formats"]).stdout)
    if source_pdf:
        available = set(capabilities.get("pdf", []))
        # A single PDF never includes the controller's PDF-merge sector.
        ordered_formats = ("docx", "png", "jpg", "txt", "md", "html", "epub", "pptx", "xlsx")
    else:
        available = set(capabilities.get("images", [])) | set(capabilities.get("images_ocr", []))
        if capabilities.get("images_to_pdf"):
            available.add("pdf")
        ordered_formats = ("png", "jpg", "webp", "pdf", "tiff", "bmp", "avif", "heic", "txt", "docx")
    order = [item for item in ordered_formats if item in available]
    if target not in order:
        raise RuntimeError(f"The requested OCR format is unavailable: {target}")
    angle = -math.pi / 2 + order.index(target) * 2 * math.pi / len(order)
    radius = 140 * wheel_size / 100
    return round(600 + radius * math.cos(angle)), round(450 + radius * math.sin(angle))


def verify_ocr_output(paths, target):
    for path in paths:
        if target == "txt":
            text = path.read_text(encoding="utf-8")
        else:
            from docx import Document
            import zipfile
            document = Document(path)
            text = "\n".join(paragraph.text for paragraph in document.paragraphs)
            with zipfile.ZipFile(path) as archive:
                contains_images = any(name.startswith("word/media/") and not name.endswith("/")
                                      for name in archive.namelist())
            if len(document.inline_shapes) or contains_images:
                raise RuntimeError("OCR Word output contains images instead of only editable text")
        if OCR_MARKER not in " ".join(text.upper().split()):
            raise RuntimeError("The OCR output did not preserve the generated marker as editable text")


def compile_injector(root):
    generated = root / "generated"
    generated.mkdir()
    xml = "/usr/share/plasma-wayland-protocols/fake-input.xml"
    header = generated / "fake-input-client-protocol.h"
    code = generated / "fake-input-protocol.c"
    # Sequential lightweight commands; this runner never builds the C++ app.
    run(["wayland-scanner", "client-header", xml, str(header)])
    run(["wayland-scanner", "private-code", xml, str(code)])
    flags = shlex.split(run(["pkg-config", "--cflags", "--libs", "wayland-client"]).stdout)
    binary = root / "wayland-inject"
    run(["cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-I", str(generated),
         str(PROJECT / "tests/wayland_inject.c"), str(code), "-o", str(binary), *flags], timeout=20)
    return binary


def qdbus(*arguments):
    return run(["qdbus6", "org.kde.KWin", *map(str, arguments)], timeout=4).stdout.strip()


def kwin_script(root, name, body):
    invocation = uuid.uuid4().hex
    plugin_name = name + "-" + invocation
    source = root / (plugin_name + ".js")
    acknowledgement = "MARACUJA_PRIVATE_SCRIPT_DONE " + invocation
    # KWin loads JavaScript through QtConcurrent before evaluating it. Observe
    # this invocation's completed body before unloading it or reading query
    # rows. Unique names and paths also avoid reusing a script whose unloading
    # is still pending in KWin's deleteLater() queue.
    source.write_text(body + "\nconsole.warn(" + json.dumps(acknowledgement) + ");\n", encoding="utf-8")
    loaded = False
    try:
        identifier = int(qdbus("/Scripting", "org.kde.kwin.Scripting.loadScript", source, plugin_name))
        if identifier < 0:
            raise RuntimeError(f"Private KWin script did not load: {plugin_name}")
        loaded = True
        try:
            qdbus(f"/Scripting/Script{identifier}", "org.kde.kwin.Script.run")
        except subprocess.CalledProcessError:
            qdbus(f"/{identifier}", "org.kde.kwin.Script.run")
        wait_for(lambda: acknowledgement in (root / "kwin.log").read_text(errors="replace"),
                 f"Private KWin script did not acknowledge completion: {name}", timeout=2)
    finally:
        try:
            if loaded:
                qdbus("/Scripting", "org.kde.kwin.Scripting.unloadScript", plugin_name)
        finally:
            source.unlink(missing_ok=True)


def wait_for(predicate, message, timeout=7):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            ready = predicate()
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            ready = False
        if ready:
            return
        time.sleep(0.1)
    raise RuntimeError(message)


def inside(root, build, *, tap_shift=False, document=False, ocr_format=None, ocr_pdf=False,
           shortcut="Shift", wheel_size=100):
    runtime = root / "runtime"
    expected_display = (root / "display-name").read_text().strip()
    if (os.environ.get("MANDARINA_ISOLATED_TEST") != "1"
            or not expected_display.startswith("mandarina-test-")
            or os.environ.get("WAYLAND_DISPLAY") != expected_display
            or os.environ.get("XDG_RUNTIME_DIR") != str(runtime)
            or (runtime.stat().st_mode & 0o777) != 0o700
            or not os.environ.get("DBUS_SESSION_BUS_ADDRESS", "").startswith(f"unix:path={runtime}/bus")):
        raise RuntimeError("The isolated bus/runtime guard failed; no input was sent.")
    injector = compile_injector(root)
    def adapt_input(commands):
        if shortcut == "Shift":
            return commands
        return commands.replace("shift down", "ctrl down\nalt down\nm down").replace(
            "shift up", "m up\nalt up\nctrl up")

    source_image = root / ("foto de teste á.txt" if document else "foto de teste á.pdf" if ocr_pdf else "foto de teste á.png")
    if document:
        source_image.write_text("Maracujá: conversão por Shift.\nTexto com acentuação e conteúdo preservado.\n", encoding="utf-8")
    elif ocr_format:
        if ocr_pdf:
            from PIL import Image
            raster_path = root / "ocr-raster.png"
            ocr_png(raster_path)
            with Image.open(raster_path) as raster, raster.convert("RGB") as page:
                page.save(source_image, format="PDF", resolution=300)
            raster_path.unlink()
        else:
            ocr_png(source_image)
    else:
        png(source_image)
    target_format = ocr_format or ("pdf" if document else "png")
    output_pattern = source_image.stem + "-convertido*." + target_format
    wheel_extent = round(460 * wheel_size / 100)
    drop_x, drop_y = (ocr_drop_position(build, ocr_format, source_pdf=ocr_pdf, wheel_size=wheel_size)
                      if ocr_format else (600, round(450 - 140 * wheel_size / 100)))
    drop_command = f"move {drop_x} {drop_y}"
    original_hash = hashlib.sha256(source_image.read_bytes()).hexdigest()
    kwin_log = root / "kwin.log"
    fixture_result = root / "fixture-result.jsonl"
    with kwin_log.open("w+") as compositor_log, (root / "app.log").open("w+") as app_log, \
            (root / "fixture.log").open("w+") as fixture_log, fixture_result.open("w+") as fixture_output, \
            (root / "injector.log").open("w+") as injector_log:
        try:
            kwin_env = dict(os.environ)
            kwin_env["QT_QPA_PLATFORM"] = "offscreen"
            kwin_env["LIBGL_ALWAYS_SOFTWARE"] = "1"
            compositor_command = ["kwin_wayland", "--virtual", "--no-lockscreen", "--no-kactivities"]
            if shortcut == "Shift":
                compositor_command.append("--no-global-shortcuts")
            compositor_command.extend(["--width", "1200", "--height", "900", "--socket", expected_display])
            compositor = spawn(compositor_command,
                               compositor_log, env=kwin_env)
            wait_for(lambda: (runtime / expected_display).exists() or compositor.poll() is not None,
                     "Private Wayland socket did not appear")
            if compositor.poll() is not None:
                raise RuntimeError("The private compositor exited during startup")
            wait_for(lambda: run(["qdbus6", "org.kde.KWin", "/KWin"], timeout=1).returncode == 0,
                     "Private KWin D-Bus interface did not appear")
            app = spawn([str(build / "maracuja"), "--background"], app_log)
            time.sleep(1)
            if qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "true":
                raise RuntimeError("The private Maracujá effect was not loaded")
            def main_window_count():
                kwin_script(root, "mandarina-private-main-count", """
console.warn('MANDARINA_PRIVATE_MAIN_COUNT '+workspace.windowList().filter(function(w) {
    return String(w.resourceClass) === 'org.winux.Mandarina' && w.caption === 'Maracujá · Conversor de arquivos';
}).length);
""")
                lines = [line.split('MANDARINA_PRIVATE_MAIN_COUNT ', 1)[1]
                         for line in kwin_log.read_text().splitlines() if 'MANDARINA_PRIVATE_MAIN_COUNT ' in line]
                return int(lines[-1]) if lines else -1
            if main_window_count() != 0:
                raise RuntimeError("Background startup mapped the main window")
            run([str(build / "maracuja"), "--background"], timeout=4)
            if main_window_count() != 0:
                raise RuntimeError("A duplicate background launch revealed the window")
            fixture = spawn([str(build / "mandarina-wayland-drag-source"), str(source_image)],
                            fixture_log, stdout=fixture_output)
            time.sleep(0.7)
            # Only this fixture is moved. Never examine or manipulate another desktop.
            kwin_script(root, "mandarina-place-fixture", """
workspace.windowList().forEach(function(w) {
    if (w.caption !== 'MandarinaDragFixture') return;
    var klass = String(w.resourceClass);
    if (klass !== 'org.winux.MandarinaDragFixture') return;
    var g = w.frameGeometry;
    w.frameGeometry = {x: 60, y: 80, width: g.width, height: g.height};
});
""")
            kwin_script(root, "mandarina-private-probe-info", """
var rows=[];
workspace.windowList().forEach(function(w) {
    if (w.caption !== 'MaracujaShiftProbe' && w.caption !== 'MandarinaDragFixture') return;
    rows.push({caption:w.caption, resourceClass:String(w.resourceClass),
        minimized:w.minimized, moveable:w.moveable,
        x:w.frameGeometry.x,y:w.frameGeometry.y,
        width:w.frameGeometry.width,height:w.frameGeometry.height});
});
console.warn('MANDARINA_PRIVATE_WINDOWS '+JSON.stringify(rows));
""")
            injection = spawn([str(injector)], injector_log, stdin=subprocess.PIPE)
            # Test both forms independently: holding Shift through the drop is
            # intentional and must never let the native source negotiate Move.
            # --tap-shift releases it after opening the wheel, before choosing.
            tap_release = "shift up\n" if tap_shift else ""
            script = f"""move 160 180
wait 150
button down
move 200 220
wait 250
move 600 450
wait 100
shift down
wait 200
{tap_release}move 601 451
wait 300
{drop_command}
wait 150
button up
shift up
wait 300
quit
"""
            before, after = script.split(drop_command, 1)
            before_shift, with_shift = before.split("shift down", 1)
            injection.stdin.write(adapt_input(before_shift))
            injection.stdin.flush()
            time.sleep(0.8)
            kwin_script(root, "mandarina-private-focus-before", """
console.warn('MANDARINA_PRIVATE_FOCUS '+JSON.stringify(workspace.activeWindow ? workspace.activeWindow.caption : '(none)'));
""")
            focus_lines = [line.split("MANDARINA_PRIVATE_FOCUS ", 1)[1]
                           for line in kwin_log.read_text().splitlines() if "MANDARINA_PRIVATE_FOCUS " in line]
            focused_before = json.loads(focus_lines[-1]) if focus_lines else None
            injection.stdin.write(adapt_input("shift down" + with_shift))
            injection.stdin.flush()
            time.sleep(0.7)
            kwin_script(root, "mandarina-private-open-info", """
var rows=[];
workspace.windowList().forEach(function(w) {
    if (w.caption !== 'MaracujaShiftProbe' && w.caption !== 'MaracujaShiftBusy') return;
    rows.push({minimized:w.minimized,above:w.keepAbove,noBorder:w.noBorder,skipTaskbar:w.skipTaskbar,
        x:w.frameGeometry.x,y:w.frameGeometry.y,width:w.frameGeometry.width,height:w.frameGeometry.height,
        fixtureFocused:!!workspace.activeWindow && workspace.activeWindow.caption === 'MandarinaDragFixture',
        activeCaption:workspace.activeWindow ? workspace.activeWindow.caption : '(none)'});
});
console.warn('MANDARINA_PRIVATE_OPEN '+JSON.stringify(rows));
""")
            open_lines = [line.split("MANDARINA_PRIVATE_OPEN ", 1)[1]
                          for line in kwin_log.read_text().splitlines() if "MANDARINA_PRIVATE_OPEN " in line]
            opened = json.loads(open_lines[-1]) if open_lines else []
            if (len(opened) != 1 or opened[0]["minimized"] or not opened[0]["above"]
                    or not opened[0]["noBorder"] or not opened[0]["skipTaskbar"]
                    or opened[0]["activeCaption"] != focused_before
                    or opened[0]["width"] != wheel_extent or opened[0]["height"] != wheel_extent):
                raise RuntimeError("The private wheel visibility, focus or taskbar flags are incorrect")
            def animation_frames():
                return sum(' -> wl_surface#' in line and '.frame(' in line
                           for line in (root / 'app.log').read_text().splitlines())
            frames = animation_frames()
            if frames < 2 or frames > 60:
                raise RuntimeError("The opening did not produce a finite compositor-paced animation")
            time.sleep(0.18)
            if animation_frames() != frames:
                raise RuntimeError("The wheel kept requesting animation frames after settling")
            injection.communicate(adapt_input(drop_command + after), timeout=8)
            if injection.returncode != 0:
                raise RuntimeError(f"Private injector exited with {injection.returncode}")
            fixture.wait(timeout=8)
            if fixture.returncode != 0:
                raise RuntimeError(f"Drag fixture exited with {fixture.returncode}")
            records = [json.loads(line) for line in fixture_result.read_text().splitlines() if line.startswith("{")]
            if not records or records[-1].get("action") != "copy" or not records[-1].get("original_exists"):
                raise RuntimeError("The source did not confirm CopyAction with its original intact")
            wait_for(lambda: bool(list(root.glob(output_pattern))),
                     "The gesture did not create a converted file", timeout=30 if ocr_format else 15 if document else 5)
            outputs = list(root.glob(output_pattern))
            if hashlib.sha256(source_image.read_bytes()).hexdigest() != original_hash:
                raise RuntimeError("The test original's bytes changed")
            if not all(path.stat().st_size > 0 for path in outputs):
                raise RuntimeError("A converted output is empty")
            if ocr_format:
                verify_ocr_output(outputs, ocr_format)
            if main_window_count() != 0:
                raise RuntimeError("A background gesture unexpectedly revealed the main window")
            # A button+Shift selection without a real drag must time out and
            # leave no invisible input target blocking the desktop.
            def probe_idle():
                kwin_script(root, "mandarina-private-idle-info", """
console.warn('MANDARINA_PRIVATE_IDLE '+workspace.windowList().some(function(w) {
    return w.caption === 'MaracujaShiftProbe' && String(w.resourceClass) === 'org.winux.Mandarina';
}));
""")
                lines = [line for line in kwin_log.read_text().splitlines() if 'MANDARINA_PRIVATE_IDLE ' in line]
                return bool(lines) and lines[-1].endswith('true')
            wait_for(probe_idle, "The helper did not return to idle", timeout=3)
            shown_before = kwin_log.read_text().count('PRIVATE probe shown')
            false_candidate = spawn([str(injector)], injector_log, stdin=subprocess.PIPE)
            false_candidate.communicate(adapt_input("move 1100 840\nbutton down\nmove 1140 850\nshift down\nwait 700\nbutton up\nshift up\nquit\n"), timeout=4)
            if false_candidate.returncode != 0:
                raise RuntimeError("The private invalid-gesture injector failed")
            time.sleep(0.2)
            if kwin_log.read_text().count('PRIVATE probe shown') <= shown_before:
                raise RuntimeError("The invalid-candidate test never opened its probe")
            kwin_script(root, "mandarina-private-timeout-info", """
var rows=[];
workspace.windowList().forEach(function(w) {
    if (w.caption === 'MaracujaShiftProbe' && String(w.resourceClass) === 'org.winux.Mandarina')
        rows.push({minimized:w.minimized});
});
console.warn('MANDARINA_PRIVATE_TIMEOUT '+JSON.stringify(rows));
""")
            timeout_lines = [line.split("MANDARINA_PRIVATE_TIMEOUT ", 1)[1]
                             for line in kwin_log.read_text().splitlines() if "MANDARINA_PRIVATE_TIMEOUT " in line]
            timed = json.loads(timeout_lines[-1]) if timeout_lines else []
            if len(timed) != 1 or not timed[0]["minimized"]:
                raise RuntimeError("An invalid candidate left the probe visible")
            if len(list(root.glob(output_pattern))) != len(outputs):
                raise RuntimeError("An invalid gesture created an output")
            # Dolphin's existing command must reuse the resident application.
            run([str(build / "maracuja"), "--wheel", source_image.as_uri()], timeout=4)
            if app.poll() is not None:
                raise RuntimeError("The existing app exited while receiving Dolphin's command")
            wait_for(lambda: main_window_count() == 1, "Dolphin's command did not show the existing window", timeout=3)
            # Close only this test's main window through its normal close path.
            kwin_script(root, "mandarina-private-close", """
workspace.windowList().forEach(function(w) {
    if (w.caption === 'Maracujá · Conversor de arquivos' && String(w.resourceClass) === 'org.winux.Mandarina')
        w.closeWindow();
});
""")
            wait_for(lambda: main_window_count() == 0, "Closing the resident window did not hide it", timeout=3)
            if app.poll() is not None or qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "true":
                raise RuntimeError("Closing the window stopped the background gesture")
            run([str(build / "maracuja"), "--background"], timeout=4)
            if main_window_count() != 0:
                raise RuntimeError("Repeated background startup reopened the closed window")
            run([str(build / "maracuja")], timeout=4)
            wait_for(lambda: main_window_count() == 1, "The launcher did not reopen the resident app", timeout=3)
            run(["qdbus6", "org.winux.Mandarina", "/Mandarina", "org.winux.Mandarina.Quit"], timeout=4)
            app.wait(timeout=4)
            if app.returncode != 0:
                raise RuntimeError("The application failed during normal exit")
            if qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "false":
                raise RuntimeError("The effect remained loaded after normal app exit")
            if qdbus("/Scripting", "org.kde.kwin.Scripting.isScriptLoaded", "mandarina-window-hints") != "false":
                raise RuntimeError("The window script remained loaded after normal app exit")
            kwin_script(root, "mandarina-private-clean-info", """
console.warn('MANDARINA_PRIVATE_CLEAN '+workspace.windowList().filter(function(w) {
    return String(w.resourceClass) === 'org.winux.Mandarina'
        && (w.caption === 'MaracujaShiftProbe' || w.caption === 'MaracujaShiftBusy');
}).length);
""")
            clean_lines = [line for line in kwin_log.read_text().splitlines() if 'MANDARINA_PRIVATE_CLEAN ' in line]
            if not clean_lines or not clean_lines[-1].endswith('0'):
                raise RuntimeError("The helper window remained after normal app exit")
            print(json.dumps({"test": "private-wayland-shift-drop", "action": "copy",
                              "shortcut": shortcut,
                              "wheel_size": wheel_size, "wheel_extent": wheel_extent,
                              "shift_mode": "tap" if tap_shift else "held",
                              "source_kind": "document" if document else "pdf" if ocr_pdf else "image",
                              "ocr_format": ocr_format,
                              "ocr_pdf": ocr_pdf,
                              "outputs": len(outputs), "original_sha256_unchanged": True,
                              "focus_preserved": True, "invalid_candidate_hidden": True,
                              "dolphin_command_reused_instance": True, "effect_cleaned_on_exit": True,
                              "background_start_hidden": True, "duplicate_background_stays_hidden": True,
                              "close_keeps_gesture": True, "launcher_reopens_window": True,
                              "opening_animation_frames": frames, "animation_stops_when_settled": True}))
            return 0
        except Exception:
            # Only logs produced by this private test compositor and test apps.
            for label, path in (("kwin", kwin_log), ("app", root / "app.log"), ("fixture-result", fixture_result),
                                ("fixture", root / "fixture.log"), ("injector", root / "injector.log")):
                if path.exists():
                    print(f"--- private {label} test diagnostics ---", file=sys.stderr)
                    lines = path.read_text(errors="replace").splitlines()
                    if label == "app":
                        lines = [line for line in lines if 'PRIVATE' in line or 'wl_data_' in line or 'set_input_region' in line or 'Error' in line]
                    print("\n".join(lines[-45:]), file=sys.stderr)
            print("private generated outputs:", len(list(root.glob("*convertido*"))), file=sys.stderr)
            raise
        finally:
            for process in reversed(PROCESSES):
                stop_owned(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=PROJECT / "build")
    parser.add_argument("--tap-shift", action="store_true",
                        help="Release the activation chord before choosing a format; default holds it through the drop")
    parser.add_argument("--shortcut", choices=("Shift", "Ctrl+Alt+M"), default="Shift",
                        help="Activation shortcut for this private drag test; Shift remains the default")
    parser.add_argument("--wheel-size", type=int, choices=range(70, 131), default=100, metavar="70..130",
                        help="Configured wheel size in percent for this isolated test")
    source_mode = parser.add_mutually_exclusive_group()
    source_mode.add_argument("--document", action="store_true", help="Validate a TXT drag converted to PDF")
    source_mode.add_argument("--ocr-format", choices=("txt", "docx"),
                             help="Validate an image drag converted to editable text with local OCR")
    parser.add_argument("--ocr-pdf", action="store_true",
                        help="Use a generated scanned PDF instead of an image for --ocr-format")
    parser.add_argument("--inside", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if arguments.ocr_pdf and not arguments.ocr_format:
        parser.error("--ocr-pdf requires --ocr-format")
    build = arguments.build_dir.resolve()
    if arguments.inside:
        deadline = 50 if arguments.document or arguments.ocr_format else 30
        def timed_out(_signum, _frame):
            raise TimeoutError(f"Private Wayland test exceeded its {deadline}-second deadline")
        signal.signal(signal.SIGALRM, timed_out)
        signal.alarm(deadline)
        try:
            return inside(arguments.inside.resolve(), build, tap_shift=arguments.tap_shift,
                          document=arguments.document, ocr_format=arguments.ocr_format,
                          ocr_pdf=arguments.ocr_pdf, shortcut=arguments.shortcut,
                          wheel_size=arguments.wheel_size)
        finally:
            signal.alarm(0)
            for process in reversed(PROCESSES):
                stop_owned(process)
    for dependency in ("kwin_wayland", "dbus-run-session", "qdbus6", "cc", "pkg-config", "wayland-scanner"):
        if not shutil.which(dependency):
            raise RuntimeError(f"Required test tool is missing: {dependency}")
    for binary in ("maracuja", "maracuja-gesture-helper", "mandarina-wayland-drag-source"):
        if not (build / binary).is_file():
            raise RuntimeError(f"Build the test binary first: {build / binary}")
    with tempfile.TemporaryDirectory(prefix="mandarina-wayland-test-") as temporary:
        root = Path(temporary)
        for name in ("runtime", "config", "data", "cache", "state"):
            (root / name).mkdir(mode=0o700)
        preferences = root / "config/Winux/Maracuja.ini"
        preferences.parent.mkdir(mode=0o700)
        preferences.write_text(f"[activation]\nshortcut={arguments.shortcut}\ndragRequired=true\n"
                               f"[appearance]\nwheelSize={arguments.wheel_size}\nwheelSoundEnabled=false\n", encoding="utf-8")
        effect = root / "data/kwin/effects/mandarina-shift"
        shutil.copytree(PROJECT / "integration/kwin/mandarina-shift", effect)
        shutil.copy2(PROJECT / "integration/kwin/window-hints.js", effect / "window-hints.js")
        hints = effect / "window-hints.js"
        hints.write_text(hints.read_text().replace("if (own(w)) {\n            if (previous)", "console.warn('PRIVATE activate '+(w ? w.caption : '(none)')+' previous '+(previous ? previous.caption : '(none)')); if (own(w)) {\n            if (previous)").replace("var previous = workspace.activeWindow;", "console.warn('PRIVATE hints loaded'); var previous = workspace.activeWindow;"
        ).replace("if (!own(w)) return;", "if (!own(w)) return; console.warn('PRIVATE hints applied');"
        ).replace("w.skipSwitcher = true;", "w.skipSwitcher = true; console.warn('PRIVATE helper flags '+w.noBorder+' '+w.keepAbove+' '+w.skipTaskbar);"))
        # Diagnostics apply solely to the copy loaded by this private compositor.
        detector = effect / "contents/code/main.js"
        detector.write_text(detector.read_text().replace(
            "var probe = findUniqueProbe();", "console.warn('PRIVATE candidate'); var probe = findUniqueProbe();"
        ).replace("if (probe === null) return;", "if (probe === null) { console.warn('PRIVATE no matching probe'); return; }"
        ).replace("if (probe.minimized) probe.minimized = false;", "if (probe.minimized) probe.minimized = false; console.warn('PRIVATE probe shown '+JSON.stringify(destination));"))
        display_name = "mandarina-test-" + uuid.uuid4().hex[:10]
        (root / "display-name").write_text(display_name)
        bus_config = root / "session-bus.conf"
        bus_path = html.escape(str(root / "runtime/bus"))
        bus_config.write_text(f"""<!DOCTYPE busconfig PUBLIC '-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN'
 'http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd'>
<busconfig><type>session</type><listen>unix:path={bus_path}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>
""")
        env = dict(os.environ)
        for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "DISPLAY", "XAUTHORITY"):
            env.pop(key, None)
        env.update({"XDG_RUNTIME_DIR": str(root / "runtime"), "XDG_CONFIG_HOME": str(root / "config"),
                    "XDG_DATA_HOME": str(root / "data"), "XDG_CACHE_HOME": str(root / "cache"),
                    "XDG_STATE_HOME": str(root / "state"), "WAYLAND_DISPLAY": display_name,
                    "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "KDE",
                    "QT_QPA_PLATFORM": "wayland", "QT_QUICK_BACKEND": "software",
                    "QT_FORCE_STDERR_LOGGING": "1", "QT_LOGGING_TO_CONSOLE": "1",
                    "MANDARINA_ISOLATED_TEST": "1"})
        # Keep PATH/library paths and HOME unchanged; all XDG writes are isolated.
        command = ["dbus-run-session", "--config-file", str(bus_config), "--", sys.executable,
                   str(Path(__file__).resolve()), "--inside", str(root), "--build-dir", str(build),
                   "--shortcut", arguments.shortcut, "--wheel-size", str(arguments.wheel_size)]
        if arguments.tap_shift:
            command.append("--tap-shift")
        if arguments.document:
            command.append("--document")
        if arguments.ocr_format:
            command.extend(["--ocr-format", arguments.ocr_format])
        if arguments.ocr_pdf:
            command.append("--ocr-pdf")
        completed = subprocess.run(command, env=env, timeout=60 if arguments.document or arguments.ocr_format else 45)
        return completed.returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"private Wayland test failed: {error}", file=sys.stderr)
        sys.exit(1)
