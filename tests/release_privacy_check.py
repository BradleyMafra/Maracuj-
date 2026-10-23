#!/usr/bin/env python3
"""Privacy gate checks using generated fixtures; no application data is read."""

import importlib.util
import io
import json
import marshal
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("maracuja_release_privacy", PROJECT / "packaging/check_release_privacy.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


def interpreter_command():
    executable = Path(sys.executable)
    # LibreOffice's Windows launcher reports its python-core directory as
    # sys.executable. Relaunch its adjacent wrapper, which initializes that
    # same private interpreter, rather than attempting to execute a directory.
    if executable.is_dir() and (executable.parent / "python.exe").is_file():
        return str(executable.parent / "python.exe")
    return str(executable)


def pyc(filename):
    code = compile("def inner():\n    return 42\n", filename, "exec")
    return importlib.util.MAGIC_NUMBER + b"\0" * 12 + marshal.dumps(code)


class ReleasePrivacyChecks(unittest.TestCase):
    def test_known_text_streams_across_boundaries_and_checks_utf16(self):
        known = gate.KnownText(["/home/private-builder", "private@example.invalid"])
        for payload in [b"\0" * (gate.CHUNK_SIZE - 5) + b"/home/private-builder/app",
                        "C:\\home\\private-builder\\app".encode("utf-16-le"),
                        "PRIVATE@EXAMPLE.INVALID".encode("utf-16-be")]:
            self.assertTrue(known.in_stream(io.BytesIO(payload)))
        self.assertFalse(known.in_stream(io.BytesIO(b"C:\\Users\\example public upstream fixture")))

    def test_binary_utf16_zip_contents_and_metadata_are_inspected_without_contents_in_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            secret = "private@example.invalid"
            (root / "binary.exe").write_bytes(b"MZ\0" + secret.encode("utf-16-le"))
            with zipfile.ZipFile(root / "python.zip", "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("public.txt", "ordinary content")
                archive.writestr("module.py", "# " + secret)
                archive.comment = secret.encode()
            report = gate.audit_stage(root, gate.KnownText([secret]))
            self.assertFalse(report["passed"])
            self.assertEqual(report["counts"]["files"], 2)
            self.assertEqual(report["counts"]["zip_entries"], 2)
            self.assertNotIn(secret, json.dumps(report))
            categories = {issue["category"] for issue in report["issues"]}
            self.assertIn("known_private_text", categories)
            self.assertIn("known_private_text_in_archive_metadata", categories)

    def test_nested_bytecode_filenames_are_checked_in_files_and_archives(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "module.pyc").write_bytes(pyc("/upstream-builder/module.py"))
            inner = io.BytesIO()
            with zipfile.ZipFile(inner, "w") as archive:
                archive.writestr("nested.pyc", pyc(r"C:\Users\upstream\nested.py"))
            with zipfile.ZipFile(root / "python.zip", "w") as archive:
                archive.writestr("nested.zip", inner.getvalue())
                archive.writestr("safe.pyc", pyc("runtime/module.py"))
                archive.writestr("frozen.pyc", pyc("<frozen module>"))
            report = gate.audit_stage(root, gate.KnownText([]))
            self.assertFalse(report["passed"])
            self.assertEqual(report["counts"]["bytecode_files"], 4)
            self.assertEqual([issue["category"] for issue in report["issues"]].count("absolute_bytecode_filename"), 2)

    @unittest.skipIf(os.name == "nt", "Unprivileged POSIX symlink fixtures")
    def test_internal_links_pass_and_external_broken_absolute_or_looping_links_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "stage"
            root.mkdir()
            (root / "target").write_text("safe")
            (root / "internal").symlink_to("target")
            self.assertTrue(gate.audit_stage(root, gate.KnownText([]))["passed"])
            (root / "absolute").symlink_to(root / "target")
            (root / "outside").symlink_to("..", target_is_directory=True)
            (root / "broken").symlink_to("missing")
            (root / "loop").symlink_to("loop")
            report = gate.audit_stage(root, gate.KnownText([]))
            self.assertFalse(report["passed"])
            self.assertEqual(len(report["issues"]), 4)
            self.assertEqual(report["counts"]["files"], 1)

    def test_bad_bytecode_and_unsafe_zip_members_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "wrong-version.pyc").write_bytes(b"BAD!" + pyc("module.py")[4:])
            with zipfile.ZipFile(root / "python.zip", "w") as archive:
                archive.writestr("../outside", b"safe bytes")
                link = zipfile.ZipInfo("link")
                link.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(link, "outside")
            report = gate.audit_stage(root, gate.KnownText([]))
            self.assertFalse(report["passed"])
            self.assertEqual({issue["category"] for issue in report["issues"]},
                             {"unreadable_or_incompatible_bytecode", "unsafe_archive_member_path", "symlink_inside_archive"})
            self.assertFalse((root.parent / "outside").exists())

    def test_private_names_are_redacted_and_public_examples_remain_allowed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "fixture.py").write_text(r"path = 'C:\Users\example\AppData'\n# upstream@example.invalid")
            known = gate.KnownText(["private@example.invalid"])
            self.assertTrue(gate.audit_stage(root, known)["passed"])
            (root / "private@example.invalid.txt").write_text("safe bytes")
            report = gate.audit_stage(root, known)
            self.assertFalse(report["passed"])
            self.assertNotIn("private@example.invalid", json.dumps(report))
            self.assertEqual(report["issues"][0]["path"], "<redacted path>")

    def test_private_json_is_optional_validated_and_never_allowed_in_the_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            stage.mkdir()
            private = root / "private.json"
            private.write_text(json.dumps({"forbidden_text": ["private@example.invalid"]}))
            self.assertTrue(gate.load_known_text(private).contains(b"PRIVATE@EXAMPLE.INVALID"))
            for document in ({}, [], [1], ["x"]):
                private.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    gate.load_known_text(private)
            private = stage / "private.json"
            private.write_text(json.dumps(["private@example.invalid"]))
            completed = subprocess.run([interpreter_command(), "-I", "-B", PROJECT / "packaging/check_release_privacy.py",
                                        "--stage", stage, "--forbidden-text-json", private], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 1)
            self.assertFalse(json.loads(completed.stdout)["passed"])
            self.assertNotIn("private@example.invalid", completed.stdout + completed.stderr)
            self.assertNotIn(str(root), completed.stdout + completed.stderr)


    def test_runtime_scopes_cover_all_files_without_relaxing_bytecode_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            office = stage / "runtime/libreoffice"
            office.mkdir(parents=True)
            (office / "office.pyc").write_bytes(pyc("runtime/libreoffice/office.py"))
            (stage / "app.pyc").write_bytes(pyc("app.py"))
            (stage / "data.txt").write_text("public")
            known = gate.KnownText([])
            entire = gate.audit_stage(stage, known)
            app = gate.audit_stage(stage, known, exclude=["runtime/libreoffice"])
            office_report = gate.audit_stage(stage, known, subtree="runtime/libreoffice")
            self.assertTrue(entire["passed"] and app["passed"] and office_report["passed"])
            self.assertEqual(entire["counts"]["files"], app["counts"]["files"] + office_report["counts"]["files"])
            self.assertEqual(entire["counts"]["bytecode_files"], 2)
            self.assertEqual(app["counts"]["bytecode_files"], 1)
            self.assertEqual(office_report["counts"]["bytecode_files"], 1)
            (office / "office.pyc").write_bytes(b"BAD!" + pyc("office.py")[4:])
            self.assertTrue(gate.audit_stage(stage, known, exclude=["runtime/libreoffice"])["passed"])
            failed = gate.audit_stage(stage, known, subtree="runtime/libreoffice")
            self.assertFalse(failed["passed"])
            self.assertEqual(failed["issues"], [{"path": "runtime/libreoffice/office.pyc",
                                               "category": "unreadable_or_incompatible_bytecode"}])

    def test_both_scopes_detect_private_text_and_redact_private_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            office = stage / "runtime/libreoffice"
            office.mkdir(parents=True)
            private = "private@example.invalid"
            (office / "private.txt").write_text(private)
            (stage / (private + ".txt")).write_text("public")
            known = gate.KnownText([private])
            for arguments in ({"subtree": "runtime/libreoffice"}, {"exclude": ["runtime/libreoffice"]}):
                report = gate.audit_stage(stage, known, **arguments)
                self.assertFalse(report["passed"])
                self.assertNotIn(private, json.dumps(report))
            # Even an empty explicitly selected directory is release metadata.
            (stage / private).mkdir()
            for arguments in ({"subtree": private}, {"exclude": [private]}):
                report = gate.audit_stage(stage, known, **arguments)
                self.assertFalse(report["passed"])
                self.assertNotIn(private, json.dumps(report))

    def test_invalid_or_overlapping_scopes_fail_before_scanning(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            (stage / "runtime/libreoffice").mkdir(parents=True)
            (stage / "runtime/python").mkdir()
            (stage / "file").write_text("public")
            invalid = ("", ".", "..", "runtime/./python", "runtime//python", "runtime/../../outside",
                       str(stage), "C:/outside", "\\\\server\\share", "file", "missing")
            for value in invalid:
                for argument in ("subtree", "exclude"):
                    kwargs = {argument: [value] if argument == "exclude" else value}
                    with self.subTest(value=value, argument=argument), patch.object(gate.os, "walk") as walk:
                        with self.assertRaises((OSError, ValueError)):
                            gate.audit_stage(stage, gate.KnownText([]), **kwargs)
                        walk.assert_not_called()
            for value in ("runtime/libreoffice", "runtime/python"):
                with self.assertRaises(ValueError):
                    gate.audit_stage(stage, gate.KnownText([]), subtree="runtime/libreoffice", exclude=[value])

    def test_unreadable_tree_is_a_failed_report_without_exception_contents(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            with patch.object(gate.os, "scandir", side_effect=PermissionError("private@example.invalid")):
                report = gate.audit_stage(stage, gate.KnownText(["private@example.invalid"]))
            self.assertFalse(report["passed"])
            self.assertEqual(report["issues"], [{"path": "<directory>", "category": "unreadable_directory"}])
            self.assertNotIn("private@example.invalid", json.dumps(report))

    def test_cli_scopes_keep_reports_relative_and_leave_inputs_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            office = stage / "runtime/libreoffice"
            office.mkdir(parents=True)
            module = office / "office.pyc"
            payload = pyc("runtime/libreoffice/office.py")
            module.write_bytes(payload)
            timestamp = module.stat().st_mtime_ns
            for scope in (["--subtree", "runtime/libreoffice"], ["--exclude", "runtime/libreoffice"]):
                completed = subprocess.run([interpreter_command(), "-I", "-B", PROJECT / "packaging/check_release_privacy.py",
                                            "--stage", stage, *scope], capture_output=True, text=True)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertTrue(json.loads(completed.stdout)["passed"])
                self.assertNotIn(str(stage), completed.stdout)
                self.assertEqual(module.read_bytes(), payload)
                self.assertEqual(module.stat().st_mtime_ns, timestamp)

    @unittest.skipUnless(os.name == "nt", "Windows junction fixture")
    def test_windows_junction_is_rejected_and_not_traversed_including_explicit_scopes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, outside = root / "stage", root / "outside"
            stage.mkdir()
            outside.mkdir()
            # Traversal would find a second issue; the only expected finding is
            # the reparse point itself, without opening this private payload.
            private = outside / "private.txt"
            private.write_text("private@example.invalid")
            link = stage / "linked"
            environment = dict(os.environ, MARACUJA_TEST_LINK=str(link), MARACUJA_TEST_TARGET=str(outside))
            completed = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction -Path $env:MARACUJA_TEST_LINK "
                "-Target $env:MARACUJA_TEST_TARGET | Out-Null"], env=environment, capture_output=True)
            self.assertEqual(completed.returncode, 0, "Windows junction fixture creation failed")
            try:
                report = gate.audit_stage(stage, gate.KnownText(["private@example.invalid"]))
                self.assertFalse(report["passed"])
                self.assertEqual(report["counts"]["files"], 0)
                self.assertEqual(report["issues"], [{"path": "linked", "category": "unsupported_reparse_point"}])
                for arguments in ({"subtree": "linked"}, {"exclude": ["linked"]}):
                    with self.assertRaisesRegex(ValueError, "link|reparse"):
                        gate.audit_stage(stage, gate.KnownText([]), **arguments)
                self.assertEqual(private.read_text(), "private@example.invalid")
            finally:
                link.rmdir()


if __name__ == "__main__":
    unittest.main(verbosity=2)
