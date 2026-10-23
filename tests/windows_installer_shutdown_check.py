#!/usr/bin/env python3
"""Exercise the installer's native file-release probe without installing it.

The generated Inno harness has no files, registry entries, icons, or uninstaller.
It exits from InitializeSetup after inspecting synthetic files and its own image.
"""

import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parents[1]


def check(iscc):
    if os.name != "nt":
        raise RuntimeError("The installer shutdown check requires native Windows")
    source = (PROJECT / "packaging/windows/Maracuja.iss").read_text(encoding="utf-8-sig")
    code = source.partition("[Code]")[2]
    if not code.strip():
        raise RuntimeError("Missing production installer code")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.restype = wintypes.BOOL
    with tempfile.TemporaryDirectory(prefix="maracuja-installer-check-") as temporary:
        root = Path(temporary)
        unlocked, locked = root / "unlocked.exe", root / "locked.exe"
        unlocked.write_bytes(b"Synthetic unlocked file; never executed.")
        locked.write_bytes(b"Synthetic locked file; never executed.")
        script = root / "probe.iss"
        script.write_text("""[Setup]
AppName=Maracuja installer probe
AppVersion=1
PrivilegesRequired=lowest
CreateAppDir=no
Uninstallable=no
DefaultDirName={tmp}\\maracuja-unused-probe
DisableStartupPrompt=yes
OutputBaseFilename=shutdown-probe
OutputDir=.
[Code]
""" + code + """
function InitializeSetup(): Boolean;
var
  Root, Report: String;
begin
  Root := ExtractFileDir(ExpandConstant('{srcexe}'));
  Report := 'missing=' + IntToStr(Ord(ExecutableIsReleased(Root + '\\missing.exe'))) + #13#10;
  Report := Report + 'unlocked=' + IntToStr(Ord(ExecutableIsReleased(Root + '\\unlocked.exe'))) + #13#10;
  Report := Report + 'locked=' + IntToStr(Ord(ExecutableIsReleased(Root + '\\locked.exe'))) + #13#10;
  Report := Report + 'mapped_image=' + IntToStr(Ord(ExecutableIsReleased(ExpandConstant('{srcexe}')))) + #13#10;
  SaveStringToFile(Root + '\\result.txt', Report, False);
  Result := False;
end;
""", encoding="utf-8-sig")
        subprocess.run([str(iscc), "/Q", str(script)], cwd=root, check=True, timeout=60)
        before = {path: path.read_bytes() for path in (unlocked, locked)}
        handle = kernel.CreateFileW(str(locked), 0x80000000, 0, None, 3, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            completed = subprocess.run([str(root / "shutdown-probe.exe"), "/VERYSILENT",
                                        "/SUPPRESSMSGBOXES", "/NORESTART"],
                                       check=False, timeout=30)
        finally:
            kernel.CloseHandle(handle)
        # InitializeSetup=False is an expected cancellation, never installation.
        if completed.returncode != 1:
            raise RuntimeError("Unexpected installer probe exit status")
        result = dict(line.split("=", 1) for line in (root / "result.txt").read_text().splitlines())
        expected = {"missing": "1", "unlocked": "1", "locked": "0", "mapped_image": "0"}
        if result != expected:
            raise RuntimeError("Installer release probe returned unexpected results: " + str(result))
        if any(path.read_bytes() != data for path, data in before.items()):
            raise RuntimeError("Installer release probe modified a fixture")
        print("Installer shutdown probe passed: absent, unlocked, locked and mapped executable; no installation.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iscc", type=Path, required=True)
    arguments = parser.parse_args()
    check(arguments.iscc.resolve(strict=True))


if __name__ == "__main__":
    main()
