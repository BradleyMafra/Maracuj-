#!/usr/bin/env python3
"""Build-only launcher pruning checks using generated files, never app data."""

import base64
import csv
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("maracuja_windows_launchers",
    PROJECT / "packaging/windows/prune_python_launchers.py")
pruner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pruner)


def record_row(name, payload):
    output = io.StringIO(newline="")
    digest = base64.urlsafe_b64encode(hashlib.sha256(payload).digest()).rstrip(b"=").decode("ascii")
    csv.writer(output, lineterminator="\r\n").writerow([name, "sha256=" + digest, str(len(payload))])
    return output.getvalue().encode("utf-8")


def fixture(stage):
    scripts = stage / "runtime/python/Scripts"
    packages = stage / "runtime/python/Lib/site-packages"
    scripts.mkdir(parents=True)
    packages.mkdir(parents=True)
    (stage / "runtime/python/python.exe").write_bytes(b"MZ private interpreter fixture")
    (scripts / ".empty").write_bytes(b"")
    (scripts / "unrelated.exe").write_bytes(b"MZ unrelated command must remain")
    expected_records = {}
    for owner in set(pruner.LAUNCHERS.values()):
        info = packages / (owner + "-1.0.dist-info")
        (info / "licenses").mkdir(parents=True)
        (info / "licenses/LICENSE").write_bytes(b"upstream notice\n")
        library = packages / owner
        library.mkdir()
        (library / "api.py").write_bytes(b"def preserved(): return 42\n")
        # Preserve quoting, CRLF and even a legal multiline CSV row verbatim.
        kept = (owner + '/api.py,,\r\n"' + owner + '/data,\r\nfixture.txt",,0\r\n').encode()
        removed = b""
        for name, package in pruner.LAUNCHERS.items():
            if package != owner:
                continue
            payload = (b"MZ\0#!C:/private-builder/python.exe\nPK\0" if name.endswith(".exe")
                       else b"#!C:/private-builder/python.exe\nprint('optional CLI')\n")
            (scripts / name).write_bytes(payload)
            removed += record_row("../../Scripts/" + name, payload)
        # Keep unrelated cache rows, even when optional CLI sources are omitted.
        kept += b"../../Scripts/__pycache__/unrelated.cpython-312.pyc,,\r\n"
        record = info / "RECORD"
        record.write_bytes(removed + kept)
        expected_records[record] = kept
    return expected_records


def snapshot(root):
    return {path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob("*") if path.is_file()}


class WindowsPythonLauncherChecks(unittest.TestCase):
    def test_only_seven_commands_are_removed_and_library_notice_record_bytes_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            records = fixture(root)
            original = snapshot(root)
            report = pruner.prune_stage(root)
            removed = {"runtime/python/Scripts/" + name for name in pruner.LAUNCHERS}
            self.assertEqual(set(report["removed"]), removed)
            self.assertEqual(len(report["records_updated"]), 6)
            for name, payload in original.items():
                path = root / name
                if name in removed:
                    self.assertFalse(path.exists())
                elif path in records:
                    self.assertEqual(path.read_bytes(), records[path])
                else:
                    self.assertEqual(path.read_bytes(), payload)

    def test_repeated_pruning_is_idempotent_and_missing_record_targets_are_cleaned(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture(root)
            (root / "runtime/python/Scripts/pdf2txt.py").unlink()
            report = pruner.prune_stage(root)
            self.assertEqual(len(report["removed"]), 6)
            before = snapshot(root)
            self.assertEqual(pruner.prune_stage(root), {"removed": [], "records_updated": []})
            self.assertEqual(snapshot(root), before)

    def test_a_changed_launcher_stops_before_removing_any_other_file_or_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture(root)
            (root / "runtime/python/Scripts/vba_extract.py").write_bytes(b"changed fixture")
            before = snapshot(root)
            with self.assertRaisesRegex(ValueError, "RECORD"):
                pruner.prune_stage(root)
            self.assertEqual(snapshot(root), before)

    def test_an_unowned_or_wrongly_owned_name_is_not_deleted(self):
        for wrong_owner in (False, True):
            with self.subTest(wrong_owner=wrong_owner), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                fixture(root)
                packages = root / "runtime/python/Lib/site-packages"
                record = packages / "cffi-1.0.dist-info/RECORD"
                text = record.read_bytes()
                script_row, kept = text.split(b"\r\n", 1)
                record.write_bytes(kept)
                if wrong_owner:
                    other = packages / "pdfplumber-1.0.dist-info/RECORD"
                    other.write_bytes(script_row + b"\r\n" + other.read_bytes())
                before = snapshot(root)
                with self.assertRaises(ValueError):
                    pruner.prune_stage(root)
                self.assertEqual(snapshot(root), before)

    def test_record_traversal_cannot_select_an_outside_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            root = outer / "stage"
            fixture(root)
            outside = outer / "outside.exe"
            outside.write_bytes(b"outside must remain")
            record = root / "runtime/python/Lib/site-packages/cffi-1.0.dist-info/RECORD"
            record.write_bytes(record.read_bytes().replace(b"../../Scripts/cffi-gen-src.exe", b"../../../outside.exe"))
            before = snapshot(root)
            with self.assertRaisesRegex(ValueError, "unowned"):
                pruner.prune_stage(root)
            self.assertEqual(outside.read_bytes(), b"outside must remain")
            self.assertEqual(snapshot(root), before)
            for relative in ("../outside.exe", "/outside.exe", "C:/outside.exe"):
                with self.assertRaises(ValueError):
                    pruner.checked_path(root.resolve(), relative)

    @unittest.skipUnless(os.name == "nt", "Windows junction fixture")
    def test_junction_scope_and_parent_are_rejected_without_touching_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            real = outer / "real"
            fixture(real)
            before = snapshot(real)
            link = outer / "linked"
            environment = dict(os.environ, MARACUJA_TEST_LINK=str(link), MARACUJA_TEST_TARGET=str(real))
            completed = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction -Path $env:MARACUJA_TEST_LINK "
                "-Target $env:MARACUJA_TEST_TARGET | Out-Null"], env=environment, capture_output=True)
            self.assertEqual(completed.returncode, 0, "Windows junction fixture creation failed")
            try:
                with self.assertRaisesRegex(ValueError, "reparse|link"):
                    pruner.prune_stage(link)
                with self.assertRaisesRegex(ValueError, "reparse|link"):
                    pruner.checked_path(outer.resolve(), "linked/runtime/python/Scripts", directory=True)
                self.assertEqual(snapshot(real), before)
            finally:
                link.rmdir()

    def test_cli_errors_do_not_print_build_account_paths(self):
        executable = Path(sys.executable)
        if executable.is_dir() and (executable.parent / "python.exe").is_file():
            executable = executable.parent / "python.exe"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completed = subprocess.run([executable, "-I", "-B", PROJECT / "packaging/windows/prune_python_launchers.py",
                                        "--stage", root / "private-builder-missing"], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 1)
            self.assertNotIn(str(root), completed.stdout + completed.stderr)
            self.assertIn("windows_python_launcher_pruning_failed", completed.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
