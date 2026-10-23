#!/usr/bin/env python3
"""Exercise Alt + file drag only in a guarded, private KDE Wayland desktop.

The application and both gesture helpers must already be built. Only the small
existing C injector is compiled. No input, D-Bus request or window operation is
sent to the user's desktop; every process and file belongs to this fixture.
"""
from __future__ import annotations

import argparse
import hashlib
import html
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

shortcut_spec = importlib.util.spec_from_file_location(
    "maracuja_private_tools_support", Path(__file__).with_name("wayland_shortcut_check.py"))
if shortcut_spec is None or shortcut_spec.loader is None:
    raise RuntimeError("The private Wayland shortcut fixture is unavailable")
shortcut = importlib.util.module_from_spec(shortcut_spec)
shortcut_spec.loader.exec_module(shortcut)
fixture = shortcut.fixture

MAIN_CAPTION = "Maracujá · Conversor de arquivos"
TOOL_CAPTIONS = {"compress": "Comprimir · Maracujá", "metadata": "Metadados · Maracujá",
                 "photo": "Editar foto · Maracujá"}
PROBE_CAPTIONS = {"MaracujaShiftProbe", "MaracujaToolsProbe"}
HELPER_CAPTIONS = PROBE_CAPTIONS | {"MaracujaShiftBusy", "MaracujaToolsBusy"}


def owned_windows(root: Path, log: Path) -> list[dict]:
    fixture.kwin_script(root, "maracuja-private-tools-windows", """
var rows=[];
var output=workspace.screenAt(workspace.cursorPos);
var area=output ? workspace.clientArea(KWin.PlacementArea, output, workspace.currentDesktop) : null;
workspace.windowList().forEach(function(w) {
    if (String(w.resourceClass) !== 'org.winux.Mandarina') return;
    rows.push({caption:w.caption,minimized:w.minimized,above:w.keepAbove,
        noBorder:w.noBorder,skipTaskbar:w.skipTaskbar,skipSwitcher:w.skipSwitcher,
        x:w.frameGeometry.x,y:w.frameGeometry.y,width:w.frameGeometry.width,
        height:w.frameGeometry.height,
        usableArea:area ? {x:area.x,y:area.y,width:area.width,height:area.height} : null,
        activeCaption:workspace.activeWindow ? workspace.activeWindow.caption : '(none)'});
});
console.warn('MARACUJA_PRIVATE_TOOLS_WINDOWS '+JSON.stringify(rows));
""")
    return shortcut.latest_log_json(log, "MARACUJA_PRIVATE_TOOLS_WINDOWS ") or []


def inside(root: Path, build: Path, tool: str, input_kind: str, wheel_size: int = 100) -> int:
    if tool == "photo" and input_kind != "image":
        raise ValueError("The photo editor fixture requires one supported image")
    wheel_extent = round(460 * wheel_size / 100)
    runtime, display = shortcut.private_guard(root)
    injector = fixture.compile_injector(root)
    source = root / ("foto de teste á.png" if input_kind == "image" else "arquivo de teste á.bin")
    if input_kind == "image":
        fixture.png(source)
    else:
        source.write_bytes(b"Maracuja private file tools fixture\n" * 128)
    original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    kwin_log = root / "kwin.log"
    app_log = root / "app.log"
    fixture_result = root / "fixture-result.jsonl"
    log_files = (kwin_log, app_log, root / "fixture.log", root / "injector.log")
    with kwin_log.open("w+") as compositor_log, app_log.open("w+") as application_log, \
            (root / "fixture.log").open("w+") as fixture_log, \
            fixture_result.open("w+") as fixture_output, \
            (root / "injector.log").open("w+") as input_log:
        try:
            compositor_env = dict(os.environ)
            compositor_env.update({"QT_QPA_PLATFORM": "offscreen", "LIBGL_ALWAYS_SOFTWARE": "1"})
            compositor = fixture.spawn(
                ["kwin_wayland", "--virtual", "--no-lockscreen", "--no-kactivities",
                 "--no-global-shortcuts", "--width", "1200", "--height", "900", "--socket", display],
                compositor_log, env=compositor_env)
            fixture.wait_for(lambda: (runtime / display).exists() or compositor.poll() is not None,
                             "Private Wayland socket did not appear")
            if compositor.poll() is not None:
                raise RuntimeError("The private compositor exited during startup")
            fixture.wait_for(lambda: fixture.run(["qdbus6", "org.kde.KWin", "/KWin"], timeout=1).returncode == 0,
                             "Private KWin D-Bus interface did not appear")
            app = fixture.spawn([str(build / "maracuja"), "--background"], application_log)

            def helpers_ready():
                rows = owned_windows(root, kwin_log)
                return {row["caption"] for row in rows if row["caption"] in PROBE_CAPTIONS} == PROBE_CAPTIONS

            fixture.wait_for(helpers_ready, "Both isolated gesture helpers did not become ready", timeout=15)
            if any(row["caption"] == MAIN_CAPTION for row in owned_windows(root, kwin_log)):
                raise RuntimeError("Background startup unexpectedly revealed the main window")
            if fixture.qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "true":
                raise RuntimeError("The private gesture effect was not loaded")

            source_process = fixture.spawn([str(build / "mandarina-wayland-drag-source"), str(source)],
                                           fixture_log, stdout=fixture_output)
            time.sleep(0.7)
            fixture.kwin_script(root, "maracuja-private-tools-place-source", """
workspace.windowList().forEach(function(w) {
    if (w.caption !== 'MandarinaDragFixture'
            || String(w.resourceClass) !== 'org.winux.MandarinaDragFixture') return;
    var g=w.frameGeometry;
    w.frameGeometry={x:60,y:80,width:g.width,height:g.height};
});
""")
            injection = fixture.spawn([str(injector)], input_log, stdin=subprocess.PIPE)
            injection.stdin.write("move 160 180\nwait 150\nbutton down\nmove 200 220\nwait 250\n"
                                  "move 600 450\nwait 100\n")
            injection.stdin.flush()
            time.sleep(0.8)
            fixture.kwin_script(root, "maracuja-private-tools-focus", """
console.warn('MARACUJA_PRIVATE_TOOLS_FOCUS '+JSON.stringify(
    workspace.activeWindow ? workspace.activeWindow.caption : '(none)'));
""")
            focus_before = shortcut.latest_log_json(kwin_log, "MARACUJA_PRIVATE_TOOLS_FOCUS ")
            injection.stdin.write("alt down\nwait 200\nmove 601 451\nwait 300\n")
            injection.stdin.flush()
            time.sleep(0.7)
            rows = owned_windows(root, kwin_log)
            probes = [row for row in rows if row["caption"] == "MaracujaToolsProbe"]
            if (len(probes) != 1 or probes[0]["minimized"] or not probes[0]["above"]
                    or not probes[0]["noBorder"] or not probes[0]["skipTaskbar"]
                    or not probes[0]["skipSwitcher"] or probes[0]["activeCaption"] != focus_before
                    or probes[0]["width"] != wheel_extent or probes[0]["height"] != wheel_extent):
                raise RuntimeError("The isolated tools wheel visibility, focus or window flags are incorrect")
            conversion = [row for row in rows if row["caption"] == "MaracujaShiftProbe"]
            if len(conversion) != 1 or not conversion[0]["minimized"]:
                raise RuntimeError("Alt opened the conversion wheel as well as the tools wheel")
            before_drop = set(root.iterdir())
            # The native wheel adds photo editing only for a single supported
            # image. Aim at the selected sector's center for its actual count.
            count = 3 if input_kind == "image" else 2
            index = {"compress": 0, "metadata": 1, "photo": 2}[tool]
            angle = -math.pi / 2 + index * math.tau / count
            radius = 140 * wheel_size / 100
            drop_x = round(probes[0]["x"] + probes[0]["width"] / 2 + radius * math.cos(angle))
            drop_y = round(probes[0]["y"] + probes[0]["height"] / 2 + radius * math.sin(angle))
            injection.communicate(f"move {drop_x} {drop_y}\nwait 150\nbutton up\nalt up\nwait 300\nquit\n", timeout=8)
            if injection.returncode:
                raise RuntimeError(f"The isolated input fixture failed: {injection.returncode}")
            source_process.wait(timeout=8)
            if source_process.returncode:
                raise RuntimeError(f"The isolated drag source failed: {source_process.returncode}")
            records = [json.loads(line) for line in fixture_result.read_text().splitlines() if line.startswith("{")]
            if not records or records[-1].get("action") != "copy" or not records[-1].get("original_exists"):
                raise RuntimeError("The source did not confirm CopyAction with its original intact")
            marker = f'PRIVATE tools drop "tool:{tool}" 1'
            fixture.wait_for(lambda: marker in app_log.read_text(errors="replace"),
                             "The tools drop was not routed to the selected tool", timeout=5)
            tool_caption = TOOL_CAPTIONS[tool]
            fixture.wait_for(lambda: any(row["caption"] == tool_caption and not row["minimized"]
                                         for row in owned_windows(root, kwin_log)),
                             "The selected tool did not open its floating options window", timeout=5)
            fixture.wait_for(lambda: any(row["caption"] == tool_caption
                                         and row["activeCaption"] == tool_caption
                                         for row in owned_windows(root, kwin_log)),
                             "The floating options did not accept editing focus after the drop", timeout=5)
            rows = owned_windows(root, kwin_log)
            if any(row["caption"] == MAIN_CAPTION for row in rows):
                raise RuntimeError("The tools drop revealed the main application window")
            options = [row for row in rows if row["caption"] == tool_caption]
            min_width, max_width, min_height, max_height = ((500, 660, 580, 760) if tool == "photo"
                                                           else (400, 500, 340, 560))
            if len(options) != 1 or not (min_width <= options[0]["width"] <= max_width
                                        and min_height <= options[0]["height"] <= max_height):
                raise RuntimeError("The tools options did not use one compact floating window")
            option = options[0]
            if not (option["above"] and option["noBorder"] and option["skipTaskbar"]
                    and option["skipSwitcher"] and option["activeCaption"] == tool_caption):
                raise RuntimeError("The floating options focus or window flags are incorrect")
            area = option["usableArea"]
            if not area or not (option["x"] >= area["x"] and option["y"] >= area["y"]
                                and option["x"] + option["width"] <= area["x"] + area["width"]
                                and option["y"] + option["height"] <= area["y"] + area["height"]):
                raise RuntimeError("The floating options exceed this output's usable area")
            # Metadata inspection is read-only and may run immediately. Neither
            # route may write an output before the user confirms the operation.
            time.sleep(0.3)
            if set(root.iterdir()) != before_drop:
                raise RuntimeError("A tool created an output before user confirmation")
            if hashlib.sha256(source.read_bytes()).hexdigest() != original_hash:
                raise RuntimeError("The tools drop changed the original fixture")
            # Closing the compact options belongs to this synthetic app only:
            # it must leave the background process and both gestures running.
            def close_options():
                # An initial metadata inspection can still be finishing. A
                # close during work is intentionally refused; retry only this
                # fixture's floating window until the read-only job is idle.
                fixture.kwin_script(root, "maracuja-private-tools-close-options", """
workspace.windowList().forEach(function(w) {
    if (String(w.resourceClass) === 'org.winux.Mandarina'
            && (w.caption === 'Comprimir · Maracujá' || w.caption === 'Metadados · Maracujá'
                || w.caption === 'Editar foto · Maracujá'))
        w.closeWindow();
});
""")
                return not any(row["caption"] == tool_caption
                               for row in owned_windows(root, kwin_log))
            fixture.wait_for(close_options,
                             "Closing tools did not hide the floating options", timeout=5)
            if app.poll() is not None:
                raise RuntimeError("Closing tools exited the background application")
            if any(row["caption"] == MAIN_CAPTION for row in owned_windows(root, kwin_log)):
                raise RuntimeError("Closing tools revealed the main application window")
            if not helpers_ready():
                raise RuntimeError("Closing tools stopped a gesture helper")
            if fixture.qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "true":
                raise RuntimeError("Closing tools unloaded the gesture effect")
            fixture.run(["qdbus6", "org.winux.Mandarina", "/Mandarina", "org.winux.Mandarina.Quit"], timeout=4)
            app.wait(timeout=5)
            if app.returncode:
                raise RuntimeError("The isolated app failed during cooperative exit")
            if fixture.qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "false":
                raise RuntimeError("The app left its gesture effect loaded")
            if fixture.qdbus("/Scripting", "org.kde.kwin.Scripting.isScriptLoaded", "mandarina-window-hints") != "false":
                raise RuntimeError("The app left its window-hint script loaded")
            if any(row["caption"] in HELPER_CAPTIONS for row in owned_windows(root, kwin_log)):
                raise RuntimeError("A gesture helper remained after cooperative app exit")
            print(json.dumps({"test": "private-wayland-alt-tools-drop", "tool": tool,
                              "wheel_size": wheel_size, "wheel_extent": wheel_extent,
                              "input_kind": input_kind, "action": "copy", "background_hidden": True,
                              "two_independent_helpers": True, "tools_focus_preserved": True,
                              "tools_window_flags": True, "conversion_wheel_stays_hidden": True,
                              "floating_tool_window_opened": True, "main_window_remains_hidden": True,
                              "floating_options_accept_focus": True, "floating_options_flags": True,
                              "floating_options_in_usable_area": True,
                              "closing_tools_preserves_background_gestures": True,
                              "no_output_before_confirmation": True,
                              "original_sha256_unchanged": True, "helpers_and_effect_cleaned_on_exit": True}))
            return 0
        except Exception:
            # This compositor contains only generated fixture windows. Include
            # their identities if startup fails, rather than assuming that an
            # empty filtered result means that no helper surface was mapped.
            try:
                fixture.kwin_script(root, "maracuja-private-tools-failure-identities", """
console.warn('MARACUJA_PRIVATE_TOOLS_FAILURE_IDENTITIES '+JSON.stringify(
    workspace.windowList().map(function(w) {
        return {caption:w.caption,resourceClass:String(w.resourceClass),
            resourceName:String(w.resourceName),desktopFileName:String(w.desktopFileName)};
    })));
""")
            except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
                pass
            for path in (*log_files, fixture_result):
                if path.exists():
                    print(f"--- private tools {path.name} diagnostics ---", file=sys.stderr)
                    lines = path.read_text(errors="replace").splitlines()
                    selected = lines[:35] + ["... private helper protocol ..."] + lines[-45:] if path == app_log and len(lines) > 80 else lines[-45:]
                    print("\n".join(selected), file=sys.stderr)
            raise
        finally:
            for process in reversed(fixture.PROCESSES):
                fixture.stop_owned(process)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=fixture.PROJECT / "build")
    parser.add_argument("--tool", choices=("compress", "metadata", "photo"), default="compress")
    parser.add_argument("--input-kind", choices=("image", "binary"), default="image")
    parser.add_argument("--wheel-size", type=int, choices=range(70, 131), default=100, metavar="70..130",
                        help="Configured wheel size in percent for this isolated test")
    parser.add_argument("--inside", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    build = arguments.build_dir.resolve()
    if arguments.inside:
        def timed_out(_signum, _frame):
            raise TimeoutError("Private tools test exceeded its 45-second deadline")
        signal.signal(signal.SIGALRM, timed_out)
        signal.alarm(45)
        try:
            return inside(arguments.inside.resolve(), build, arguments.tool, arguments.input_kind,
                          arguments.wheel_size)
        finally:
            signal.alarm(0)
            for process in reversed(fixture.PROCESSES):
                fixture.stop_owned(process)

    for dependency in ("kwin_wayland", "dbus-run-session", "qdbus6", "cc", "pkg-config", "wayland-scanner"):
        if not shutil.which(dependency):
            raise RuntimeError(f"Required test tool is missing: {dependency}")
    for name in ("maracuja", "maracuja-gesture-helper", "mandarina-wayland-drag-source"):
        if not (build / name).is_file():
            raise RuntimeError(f"Build the test binary first: {build / name}")
    with tempfile.TemporaryDirectory(prefix="mandarina-tools-test-") as temporary:
        root = Path(temporary)
        for name in ("runtime", "config", "data", "cache", "state"):
            (root / name).mkdir(mode=0o700)
        effect = root / "data/kwin/effects/mandarina-shift"
        shutil.copytree(fixture.PROJECT / "integration/kwin/mandarina-shift", effect)
        shutil.copy2(fixture.PROJECT / "integration/kwin/window-hints.js", effect / "window-hints.js")
        preferences = root / "config/Winux/Maracuja.ini"
        preferences.parent.mkdir(mode=0o700)
        preferences.write_text("[activation]\nshortcut=Shift\ndragRequired=true\n"
                               "[toolsActivation]\nshortcut=Alt\ndragRequired=true\n"
                               f"[appearance]\nwheelSize={arguments.wheel_size}\nwheelSoundEnabled=false\n", encoding="utf-8")
        display = "mandarina-test-" + uuid.uuid4().hex[:10]
        (root / "display-name").write_text(display, encoding="utf-8")
        bus_config = root / "session-bus.conf"
        bus_path = html.escape(str(root / "runtime/bus"))
        bus_config.write_text(f"""<!DOCTYPE busconfig PUBLIC '-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN'
 'http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd'>
<busconfig><type>session</type><listen>unix:path={bus_path}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>
""", encoding="utf-8")
        env = dict(os.environ)
        for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "DISPLAY", "XAUTHORITY"):
            env.pop(key, None)
        env.update({"XDG_RUNTIME_DIR": str(root / "runtime"), "XDG_CONFIG_HOME": str(root / "config"),
                    "XDG_DATA_HOME": str(root / "data"), "XDG_CACHE_HOME": str(root / "cache"),
                    "XDG_STATE_HOME": str(root / "state"), "WAYLAND_DISPLAY": display,
                    "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "KDE",
                    "QT_QPA_PLATFORM": "wayland", "QT_QUICK_BACKEND": "software",
                    "QT_SCALE_FACTOR": "1", "QT_FONT_DPI": "96",
                    "QT_FORCE_STDERR_LOGGING": "1", "QT_LOGGING_TO_CONSOLE": "1",
                    "MANDARINA_ISOLATED_TEST": "1"})
        command = ["dbus-run-session", "--config-file", str(bus_config), "--", sys.executable,
                   str(Path(__file__).resolve()), "--inside", str(root), "--build-dir", str(build),
                   "--tool", arguments.tool, "--input-kind", arguments.input_kind,
                   "--wheel-size", str(arguments.wheel_size)]
        return subprocess.run(command, env=env, timeout=55).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"private Wayland tools test failed: {error}", file=sys.stderr)
        sys.exit(1)
