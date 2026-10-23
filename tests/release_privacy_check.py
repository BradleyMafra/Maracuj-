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
import zipfile

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("maracuja_release_privacy", PROJECT / "packaging/check_release_privacy.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


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
            completed = subprocess.run([sys.executable, "-I", "-B", PROJECT / "packaging/check_release_privacy.py",
                                        "--stage", stage, "--forbidden-text-json", private], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 1)
            self.assertFalse(json.loads(completed.stdout)["passed"])
            self.assertNotIn("private@example.invalid", completed.stdout + completed.stderr)
            self.assertNotIn(str(root), completed.stdout + completed.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
