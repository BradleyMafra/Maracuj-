#!/usr/bin/env python3
"""Validate a real configurable chord in a fresh private KDE Wayland desktop.

All D-Bus requests and synthetic keys target the temporary bus and guarded
mandarina-test-* compositor created here. No running desktop or user input is
accessed. Build the application first; this script builds only its C fixture.
"""
from __future__ import annotations

import argparse
import configparser
import html
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

fixture_spec = importlib.util.spec_from_file_location(
    "maracuja_private_wayland_fixture", Path(__file__).with_name("wayland_integration_check.py"))
if fixture_spec is None or fixture_spec.loader is None:
    raise RuntimeError("The private Wayland fixture module is unavailable")
fixture = importlib.util.module_from_spec(fixture_spec)
fixture_spec.loader.exec_module(fixture)


COMBINED_CTRL_ALT_M = 0x04000000 | 0x08000000 | ord("M")
ACTION_PREFIX = "_k_session:maracuja-wheel-"


def private_guard(root: Path) -> tuple[Path, str]:
    runtime = root / "runtime"
    display = (root / "display-name").read_text().strip()
    runtime_info = runtime.stat()
    if (os.environ.get("MANDARINA_ISOLATED_TEST") != "1"
            or not display.startswith("mandarina-test-") or "/" in display
            or os.environ.get("WAYLAND_DISPLAY") != display
            or os.environ.get("XDG_RUNTIME_DIR") != str(runtime)
            or runtime_info.st_uid != os.getuid()
            or runtime_info.st_mode & 0o777 != 0o700
            or not os.environ.get("DBUS_SESSION_BUS_ADDRESS", "").startswith(f"unix:path={runtime}/bus")):
        raise RuntimeError("The isolated bus/runtime guard failed; no input was sent.")
    for kind in ("CONFIG", "DATA", "CACHE", "STATE"):
        if os.environ.get(f"XDG_{kind}_HOME") != str(root / kind.lower()):
            raise RuntimeError("The private XDG guard failed; no desktop was started.")
    return runtime, display


def binding() -> list[str]:
    return fixture.run(["qdbus6", "org.kde.kglobalaccel", "/kglobalaccel",
                        "org.kde.KGlobalAccel.action", str(COMBINED_CTRL_ALT_M)], timeout=2).stdout.splitlines()


def configured_action(root: Path) -> str:
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.read(root / "config/kwinrc", encoding="utf-8")
    return parser.get("Effect-mandarina-shift", "ShortcutAction", fallback="")


def latest_log_json(log_path: Path, marker: str):
    rows = [line.split(marker, 1)[1] for line in log_path.read_text(errors="replace").splitlines()
            if marker in line]
    return json.loads(rows[-1]) if rows else None


def windows(root: Path, kwin_log: Path) -> list[dict]:
    fixture.kwin_script(root, "maracuja-private-shortcut-windows", """
var rows = [];
workspace.windowList().forEach(function(w) {
    if (String(w.resourceClass) !== 'org.winux.Mandarina'
            || w.caption !== 'Maracujá · Conversor de arquivos') return;
    rows.push({width:w.frameGeometry.width, height:w.frameGeometry.height,
        clientWidth:w.clientGeometry ? w.clientGeometry.width : null,
        clientHeight:w.clientGeometry ? w.clientGeometry.height : null,
        minimized:w.minimized, active:workspace.activeWindow === w});
});
console.warn('MARACUJA_PRIVATE_SHORTCUT_WINDOWS '+JSON.stringify(rows));
""")
    return latest_log_json(kwin_log, "MARACUJA_PRIVATE_SHORTCUT_WINDOWS ") or []


def inject(binary: Path, log, commands: str):
    process = fixture.spawn([str(binary)], log, stdin=subprocess.PIPE)
    process.communicate(commands + "\nquit\n", timeout=5)
    if process.returncode:
        raise RuntimeError(f"Private shortcut input fixture failed: {process.returncode}")


def inside(root: Path, build: Path) -> int:
    runtime, display = private_guard(root)
    injector = fixture.compile_injector(root)
    kwin_log = root / "kwin.log"
    app_log = root / "app.log"
    injector_log = root / "injector.log"
    with kwin_log.open("w+") as compositor_log, app_log.open("w+") as application_log, injector_log.open("w+") as input_log:
        try:
            compositor_env = dict(os.environ)
            compositor_env.update({"QT_QPA_PLATFORM": "offscreen", "LIBGL_ALWAYS_SOFTWARE": "1"})
            # Global shortcuts must remain enabled to exercise KGlobalAccel's
            # real registration, signal delivery and session-scoped cleanup.
            compositor = fixture.spawn(["kwin_wayland", "--virtual", "--no-lockscreen", "--no-kactivities",
                                        "--width", "1200", "--height", "900", "--socket", display],
                                       compositor_log, env=compositor_env)
            fixture.wait_for(lambda: (runtime / display).exists() or compositor.poll() is not None,
                             "Private Wayland socket did not appear")
            if compositor.poll() is not None:
                raise RuntimeError("The private compositor exited during startup")
            fixture.wait_for(lambda: fixture.run(["qdbus6", "org.kde.KWin", "/KWin"], timeout=1).returncode == 0,
                             "Private KWin D-Bus interface did not appear")
            app = fixture.spawn([str(build / "maracuja"), "--background"], application_log)

            def owned_binding():
                action = configured_action(root)
                actual = binding()
                return action.startswith(ACTION_PREFIX) and len(actual) >= 2 and actual[1] == action

            fixture.wait_for(owned_binding, "Ctrl+Alt+M was not registered by this session", timeout=15)
            action = configured_action(root)
            if fixture.qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "true":
                raise RuntimeError("The private shortcut effect was not loaded")
            if windows(root, kwin_log):
                raise RuntimeError("The background app mapped its window before activation")

            inject(injector, input_log, "f8 down\nwait 80\nf8 up\nwait 200")
            if windows(root, kwin_log):
                raise RuntimeError("An unrelated function key activated the wheel")
            inject(injector, input_log, "ctrl down\nm down\nwait 80\nm up\nctrl up\nwait 200")
            if windows(root, kwin_log):
                raise RuntimeError("A chord missing Alt activated the wheel")
            inject(injector, input_log, "ctrl down\nalt down\nm down\nwait 120\nm up\nalt up\nctrl up\nwait 300")
            fixture.wait_for(lambda: len(windows(root, kwin_log)) == 1,
                             "The real Ctrl+Alt+M signal did not open the compact wheel", timeout=4)
            opened = {}
            def compact_geometry():
                nonlocal opened
                shown = windows(root, kwin_log)
                if len(shown) != 1:
                    return False
                opened = shown[0]
                # Main.qml's compact client is 550x610, with 520x580 minima.
                # Decorations are compositor-owned; check client geometry when
                # available and allow only a small decorated-frame allowance.
                width = opened.get("clientWidth") or opened["width"]
                height = opened.get("clientHeight") or opened["height"]
                return (not opened["minimized"] and 520 <= width <= 600
                        and 580 <= height <= 660)
            try:
                fixture.wait_for(compact_geometry, "The shortcut window did not settle to compact geometry", timeout=4)
            except RuntimeError as error:
                raise RuntimeError(f"{error}: {json.dumps(opened, sort_keys=True)}") from error

            # Close only the window owned by this test's isolated Maracujá.
            fixture.kwin_script(root, "maracuja-private-shortcut-close", """
workspace.windowList().forEach(function(w) {
    if (String(w.resourceClass) === 'org.winux.Mandarina'
            && w.caption === 'Maracujá · Conversor de arquivos') w.closeWindow();
});
""")
            fixture.wait_for(lambda: not windows(root, kwin_log), "Closing the wheel did not hide it")
            if app.poll() is not None or not owned_binding():
                raise RuntimeError("Closing the wheel stopped the background shortcut")
            inject(injector, input_log, "ctrl down\nalt down\nm down\nwait 100\nm up\nalt up\nctrl up\nwait 250")
            fixture.wait_for(lambda: len(windows(root, kwin_log)) == 1,
                             "The configured chord did not reopen the existing application")
            fixture.run(["qdbus6", "org.winux.Mandarina", "/Mandarina", "org.winux.Mandarina.Quit"], timeout=4)
            app.wait(timeout=4)
            if app.returncode != 0:
                raise RuntimeError("The app failed during cooperative exit")
            if fixture.qdbus("/Effects", "org.kde.kwin.Effects.isEffectLoaded", "mandarina-shift") != "false":
                raise RuntimeError("The app left its transient effect loaded")
            actual = binding()
            if len(actual) >= 2 and actual[1] == action:
                raise RuntimeError("The app left its session shortcut registered after exit")
            available = fixture.run(["qdbus6", "org.kde.kglobalaccel", "/kglobalaccel",
                                     "org.kde.KGlobalAccel.isGlobalShortcutAvailable", str(COMBINED_CTRL_ALT_M), ""], timeout=2).stdout.strip()
            if available != "true":
                raise RuntimeError("Ctrl+Alt+M remained reserved after the app exited")
            print(json.dumps({"test": "private-wayland-configured-shortcut", "shortcut": "Ctrl+Alt+M",
                              "drag_required": False, "background_hidden": True,
                              "real_kglobalaccel_session_binding": True, "unrelated_keys_ignored": True,
                              "compact_wheel_opened_without_drag": True, "reopened_existing_application": True,
                              "effect_and_shortcut_cleaned_on_exit": True}))
            return 0
        except Exception:
            # Logs belong only to this temporary compositor and our test app.
            for name, path in (("kwin", kwin_log), ("app", app_log), ("injector", injector_log)):
                if path.exists():
                    print(f"--- private {name} shortcut diagnostics ---", file=sys.stderr)
                    print("\n".join(path.read_text(errors="replace").splitlines()[-55:]), file=sys.stderr)
            raise
        finally:
            for process in reversed(fixture.PROCESSES):
                fixture.stop_owned(process)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=fixture.PROJECT / "build")
    parser.add_argument("--inside", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    build = arguments.build_dir.resolve()
    if arguments.inside:
        def timed_out(_signal, _frame):
            raise TimeoutError("Private shortcut test exceeded its 45-second deadline")
        signal.signal(signal.SIGALRM, timed_out)
        signal.alarm(45)
        try:
            return inside(arguments.inside.resolve(), build)
        finally:
            signal.alarm(0)
            for process in reversed(fixture.PROCESSES):
                fixture.stop_owned(process)

    for dependency in ("kwin_wayland", "dbus-run-session", "qdbus6", "cc", "pkg-config", "wayland-scanner"):
        if not shutil.which(dependency):
            raise RuntimeError(f"Required test tool is missing: {dependency}")
    for name in ("maracuja", "maracuja-gesture-helper"):
        if not (build / name).is_file():
            raise RuntimeError(f"Build the test binary first: {build / name}")
    with tempfile.TemporaryDirectory(prefix="mandarina-shortcut-test-") as temporary:
        root = Path(temporary)
        for name in ("runtime", "config", "data", "cache", "state"):
            (root / name).mkdir(mode=0o700)
        effect = root / "data/kwin/effects/mandarina-shift"
        shutil.copytree(fixture.PROJECT / "integration/kwin/mandarina-shift", effect)
        shutil.copy2(fixture.PROJECT / "integration/kwin/window-hints.js", effect / "window-hints.js")
        preferences = root / "config/Winux/Maracuja.ini"
        preferences.parent.mkdir(mode=0o700)
        preferences.write_text("[activation]\nshortcut=Ctrl+Alt+M\ndragRequired=false\n", encoding="utf-8")
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
                   str(Path(__file__).resolve()), "--inside", str(root), "--build-dir", str(build)]
        return subprocess.run(command, env=env, timeout=55).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"private Wayland shortcut test failed: {error}", file=sys.stderr)
        sys.exit(1)
