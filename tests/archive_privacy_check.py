#!/usr/bin/env python3
"""Release-account privacy checks using only generated standard-library fixtures."""

import importlib.machinery
import importlib.util
import io
import marshal
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location(
    "maracuja_archive_privacy", Path(__file__).resolve().parents[1] / "packaging/archive_privacy.py")
privacy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(privacy)


def bytecode(filename, flags=0):
    code = compile("def outer(value):\n    def inner():\n        return value + 1\n    return inner\n", filename, "exec")
    header = importlib.util.MAGIC_NUMBER + flags.to_bytes(4, "little") + b"12345678"
    return header + marshal.dumps(code)


def filenames(code):
    return [code.co_filename] + [name for value in code.co_consts if isinstance(value, types.CodeType)
                                 for name in filenames(value)]


class ArchivePrivacyChecks(unittest.TestCase):
    def test_tar_omits_account_metadata_and_keeps_content_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "application"
            source.write_text("fixture", encoding="utf-8")
            source.chmod(0o755)
            output = io.BytesIO()

            def filter_info(info):
                info.uid, info.gid = 12345, 23456
                info.uname, info.gname = "private-builder", "private-group"
                info.mode = 0o755
                info.pax_headers.update({"uid": "12345", "gname": "private-group"})
                return privacy.anonymous_tarinfo(info)

            with tarfile.open(fileobj=output, mode="w:gz") as archive:
                archive.add(source, arcname="Maracuja/application", filter=filter_info)
            output.seek(0)
            with tarfile.open(fileobj=output, mode="r:gz") as archive:
                info = archive.getmember("Maracuja/application")
                self.assertEqual((info.uid, info.gid, info.uname, info.gname), (0, 0, "", ""))
                self.assertEqual(info.mode, 0o755)
                self.assertEqual(archive.extractfile(info).read(), b"fixture")
            self.assertNotIn(b"private-builder", output.getvalue())

    def test_nested_code_is_relative_and_executes_with_unchanged_header(self):
        stage = Path("/temporary/private-builder/Maracuja")
        name = "runtime/python/lib/python3.12/site-packages/example/__pycache__/demo.cpython-312.pyc"
        expected = "runtime/python/lib/python3.12/site-packages/example/demo.py"
        for flags in (0, 1, 3):
            with self.subTest(flags=flags):
                original = bytecode(str(stage / expected), flags)
                sanitized, changed = privacy.sanitize_bytecode(original, stage, name)
                self.assertTrue(changed)
                self.assertEqual(sanitized[:16], original[:16])
                code = marshal.loads(sanitized[16:])
                self.assertEqual(filenames(code), [expected] * 3)
                namespace = {}
                exec(code, namespace)
                self.assertEqual(namespace["outer"](41)(), 42)
                again, changed_again = privacy.sanitize_bytecode(sanitized, stage, name)
                self.assertFalse(changed_again)
                self.assertEqual(again, sanitized)

    def test_windows_and_upstream_paths_use_cache_location(self):
        stage = Path("/temporary/stage")
        name = "runtime/python/Lib/site-packages/example.pyc"
        expected = "runtime/python/Lib/site-packages/example.py"
        for original in (r"C:\Users\private-builder\checkout\example.py", "/install/lib/example.py"):
            with self.subTest(original=original):
                sanitized, changed = privacy.sanitize_bytecode(bytecode(original), stage, name)
                self.assertTrue(changed)
                self.assertEqual(filenames(marshal.loads(sanitized[16:])), [expected] * 3)

    def test_relative_and_synthetic_filenames_remain_byte_identical(self):
        for filename in ("runtime/python/example.py", "<string>", "<frozen example>"):
            with self.subTest(filename=filename):
                original = bytecode(filename)
                sanitized, changed = privacy.sanitize_bytecode(original, Path("/stage"), "example.pyc")
                self.assertFalse(changed)
                self.assertEqual(sanitized, original)

    def test_sourceless_module_survives_with_permissions_and_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            module = stage / "example.pyc"
            module.write_bytes(bytecode("/private-builder/example.py"))
            module.chmod(0o640)
            os.utime(module, ns=(1_000_000_000, 2_000_000_000))
            before = module.stat()
            result = privacy.sanitize_stage(stage)
            self.assertEqual(result["bytecode_files_sanitized"], 1)
            after = module.stat()
            self.assertEqual(stat.S_IMODE(after.st_mode), stat.S_IMODE(before.st_mode))
            self.assertEqual(after.st_mtime_ns, before.st_mtime_ns)
            loaded = types.ModuleType("privacy_fixture")
            importlib.machinery.SourcelessFileLoader("privacy_fixture", str(module)).exec_module(loaded)
            self.assertEqual(loaded.outer(41)(), 42)
            self.assertFalse((stage / "example.py").exists())

    def test_zip_bytecode_is_sanitized_without_losing_data_or_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            path = stage / "python312.zip"
            entry = zipfile.ZipInfo("example.pyc", date_time=(2020, 1, 2, 3, 4, 6))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            with zipfile.ZipFile(path, "w") as archive:
                archive.comment = b"fixture comment"
                archive.writestr(entry, bytecode("/private-builder/example.py"))
                archive.writestr("data.txt", b"unchanged data")
            result = privacy.sanitize_stage(stage)
            self.assertEqual(result["zip_bytecode_entries_sanitized"], 1)
            with zipfile.ZipFile(path) as archive:
                self.assertEqual(archive.read("data.txt"), b"unchanged data")
                self.assertEqual(archive.comment, b"fixture comment")
                self.assertEqual(archive.getinfo("example.pyc").date_time, entry.date_time)
                self.assertEqual(archive.getinfo("example.pyc").external_attr, entry.external_attr)
                code = marshal.loads(archive.read("example.pyc")[16:])
                self.assertEqual(filenames(code), ["python312.zip/example.py"] * 3)
            self.assertEqual(privacy.sanitize_stage(stage)["zip_bytecode_entries_sanitized"], 0)

    def test_incompatible_bytecode_fails_without_deleting_or_rewriting(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            path = stage / "example.pyc"
            payload = b"BAD!" + bytecode("/private-builder/example.py")[4:]
            path.write_bytes(payload)
            with self.assertRaisesRegex(RuntimeError, "matching Python version"):
                privacy.sanitize_stage(stage)
            self.assertEqual(path.read_bytes(), payload)

    @unittest.skipIf(os.name == "nt", "POSIX launcher relocation")
    def test_private_pip_launcher_runs_after_relocation_with_original_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "private builder" / "Maracuja"
            binaries = stage / "runtime/python/bin"
            binaries.mkdir(parents=True)
            (binaries / "python3").symlink_to(sys.executable)
            launcher = binaries / "example"
            # pip selects a shell trampoline when the interpreter has spaces.
            launcher.write_text("#!/bin/sh\n'''exec' '" + str(binaries / "python3")
                                + "' -I \"$0\" \"$@\"\n' '''\n"
                                + "import json, sys\nprint(json.dumps(sys.argv[1:]))\n", encoding="utf-8")
            launcher.chmod(0o755)
            os.utime(launcher, ns=(1_000_000_000, 2_000_000_000))
            before = launcher.stat()
            result = privacy.sanitize_stage(stage)
            self.assertEqual(result["python_launchers_sanitized"], 1)
            self.assertNotIn(str(stage).encode(), launcher.read_bytes())
            self.assertEqual(launcher.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(stat.S_IMODE(launcher.stat().st_mode), 0o755)
            destination = root / "relocated app"
            stage.rename(destination)
            moved = destination / "runtime/python/bin/example"
            arguments = ["first argument", "--flag", "ação", "$(unchanged)"]
            completed = subprocess.run([moved, *arguments], check=True, capture_output=True, text=True, cwd=root)
            import json
            self.assertEqual(json.loads(completed.stdout), arguments)
            self.assertEqual(privacy.sanitize_stage(destination)["python_launchers_sanitized"], 0)

    def test_simple_shebang_relocates_but_binary_and_other_scripts_are_untouched(self):
        stage = Path("/private-builder/Maracuja")
        original = b"#!/private-builder/Maracuja/runtime/python/bin/python3.12 -s\nprint('same code')\n"
        sanitized, changed = privacy.sanitize_python_launcher(original, stage, "runtime/python/bin/example")
        self.assertTrue(changed)
        self.assertIn(b'/python3.12" -s "$0" "$@"', sanitized)
        self.assertTrue(sanitized.endswith(b"print('same code')\n"))
        for payload, name in [(b"MZ\x00" + original, "runtime/python/bin/example.exe"),
                              (original, "helpers/example.py"),
                              (b"#!/usr/bin/python3\nprint('public')\n", "runtime/python/bin/example")]:
            self.assertEqual(privacy.sanitize_python_launcher(payload, stage, name), (payload, False))


    def test_subtree_uses_full_stage_relative_names_and_preserves_other_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            office = stage / "runtime/libreoffice"
            office.mkdir(parents=True)
            module = office / "loader.pyc"
            original = bytecode(str(module.with_suffix(".py")))
            module.write_bytes(original)
            foreign = stage / "conversion.pyc"
            foreign_payload = b"BAD!" + original[4:]
            foreign.write_bytes(foreign_payload)
            result = privacy.sanitize_stage(stage, subtree="runtime/libreoffice")
            self.assertEqual(result["bytecode_files_checked"], 1)
            self.assertEqual(result["bytecode_files_sanitized"], 1)
            self.assertEqual(module.read_bytes()[:16], original[:16])
            self.assertEqual(filenames(marshal.loads(module.read_bytes()[16:])),
                             ["runtime/libreoffice/loader.py"] * 3)
            self.assertEqual(foreign.read_bytes(), foreign_payload)
            self.assertFalse(module.with_suffix(".py").exists())

    def test_explicit_exclusion_does_not_relax_magic_validation_in_selected_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            office = stage / "runtime/libreoffice"
            office.mkdir(parents=True)
            payload = b"BAD!" + bytecode("/private-builder/office.py")[4:]
            foreign = office / "loader.pyc"
            foreign.write_bytes(payload)
            module = stage / "conversion.pyc"
            module.write_bytes(bytecode("/private-builder/conversion.py"))
            result = privacy.sanitize_stage(stage, exclude=["runtime/libreoffice"])
            self.assertEqual(result["bytecode_files_checked"], 1)
            self.assertEqual(result["bytecode_files_sanitized"], 1)
            self.assertEqual(foreign.read_bytes(), payload)
            with self.assertRaisesRegex(RuntimeError, "matching Python version"):
                privacy.sanitize_stage(stage, subtree="runtime/libreoffice")
            self.assertEqual(foreign.read_bytes(), payload)

    def test_invalid_scopes_fail_before_any_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            (stage / "runtime/office").mkdir(parents=True)
            (stage / "runtime/conversion").mkdir()
            module = stage / "first.pyc"
            original = bytecode("/private-builder/first.py")
            module.write_bytes(original)
            invalid = ("", ".", "..", "runtime/./office", "runtime//office", "runtime/../../outside", str(root),
                       "C:/outside", "\\\\server\\share", "first.pyc", "missing")
            for value in invalid:
                for argument in ("subtree", "exclude"):
                    with self.subTest(value=value, argument=argument):
                        kwargs = {argument: [value] if argument == "exclude" else value}
                        with self.assertRaises((OSError, RuntimeError)):
                            privacy.sanitize_stage(stage, **kwargs)
                        self.assertEqual(module.read_bytes(), original)
            for excluded in ("runtime/office", "runtime/conversion"):
                with self.assertRaises(RuntimeError):
                    privacy.sanitize_stage(stage, subtree="runtime/office", exclude=[excluded])
            self.assertEqual(module.read_bytes(), original)

    def test_check_mode_rejects_absolute_paths_without_writing_files_or_zip(self):
        for zipped in (False, True):
            with self.subTest(zipped=zipped), tempfile.TemporaryDirectory() as temporary:
                stage = Path(temporary)
                path = stage / ("python.zip" if zipped else "module.pyc")
                payload = bytecode("/private-builder/module.py")
                if zipped:
                    with zipfile.ZipFile(path, "w") as archive:
                        archive.writestr("module.pyc", payload)
                else:
                    path.write_bytes(payload)
                before, timestamp = path.read_bytes(), path.stat().st_mtime_ns
                with self.assertRaisesRegex(RuntimeError, "Absolute traceback path"):
                    privacy.sanitize_stage(stage, check=True)
                self.assertEqual(path.read_bytes(), before)
                self.assertEqual(path.stat().st_mtime_ns, timestamp)
                privacy.sanitize_stage(stage)
                sanitized, timestamp = path.read_bytes(), path.stat().st_mtime_ns
                result = privacy.sanitize_stage(stage, check=True)
                self.assertEqual(result["bytecode_files_sanitized"], 0)
                self.assertEqual(result["zip_bytecode_entries_sanitized"], 0)
                self.assertEqual(result["bytecode_files_checked"] + result["zip_bytecode_entries_checked"], 1)
                self.assertEqual(path.read_bytes(), sanitized)
                self.assertEqual(path.stat().st_mtime_ns, timestamp)

    def test_directory_links_are_not_traversed_or_accepted_as_scopes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, outside = root / "stage", root / "outside"
            stage.mkdir()
            outside.mkdir()
            module = outside / "private.pyc"
            original = bytecode("/private-builder/private.py")
            module.write_bytes(original)
            link = stage / "linked"
            try:
                link.symlink_to(outside, target_is_directory=True)
            except OSError:
                self.skipTest("Directory symlinks are unavailable in this test environment")
            for kwargs in ({"subtree": "linked"}, {"exclude": ["linked"]}):
                with self.assertRaisesRegex(RuntimeError, "link"):
                    privacy.sanitize_stage(stage, **kwargs)
            self.assertEqual(privacy.sanitize_stage(stage)["bytecode_files_checked"], 0)
            self.assertEqual(module.read_bytes(), original)

    def test_unreadable_selected_tree_cannot_report_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            module = stage / "module.pyc"
            original = bytecode("/private-builder/module.py")
            module.write_bytes(original)
            for check in (False, True):
                with self.subTest(check=check), patch.object(privacy.os, "scandir", side_effect=PermissionError("denied")):
                    with self.assertRaises(PermissionError):
                        privacy.sanitize_stage(stage, check=check)
                self.assertEqual(module.read_bytes(), original)


    @unittest.skipIf(os.name == "nt", "POSIX launcher paths")
    def test_launcher_check_mode_does_not_rewrite_a_private_shebang(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            binaries = stage / "runtime/python/bin"
            binaries.mkdir(parents=True)
            launcher = binaries / "example"
            payload = ("#!" + (binaries / "python3").as_posix() + " -I\nprint(42)\n").encode()
            launcher.write_bytes(payload)
            timestamp = launcher.stat().st_mtime_ns
            with self.assertRaisesRegex(RuntimeError, "Absolute launcher path"):
                privacy.sanitize_stage(stage, check=True)
            self.assertEqual(launcher.read_bytes(), payload)
            self.assertEqual(launcher.stat().st_mtime_ns, timestamp)
            self.assertEqual(privacy.sanitize_stage(stage)["python_launchers_sanitized"], 1)
            self.assertEqual(privacy.sanitize_stage(stage, check=True)["python_launchers_sanitized"], 0)

    @unittest.skipUnless(os.name == "nt", "Windows junction fixture")
    def test_windows_junction_fails_without_reading_or_modifying_its_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, outside = root / "stage", root / "outside"
            stage.mkdir()
            outside.mkdir()
            module = outside / "private.pyc"
            original = bytecode("/private-builder/private.py")
            module.write_bytes(original)
            link = stage / "linked"
            environment = dict(os.environ, MARACUJA_TEST_LINK=str(link), MARACUJA_TEST_TARGET=str(outside))
            completed = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction -Path $env:MARACUJA_TEST_LINK "
                "-Target $env:MARACUJA_TEST_TARGET | Out-Null"], env=environment, capture_output=True)
            self.assertEqual(completed.returncode, 0, "Windows junction fixture creation failed")
            try:
                for arguments in ({}, {"check": True}, {"subtree": "linked"}, {"exclude": ["linked"]}):
                    with self.subTest(arguments=arguments), self.assertRaisesRegex(RuntimeError, "reparse|link"):
                        privacy.sanitize_stage(stage, **arguments)
                self.assertEqual(module.read_bytes(), original)
            finally:
                # rmdir removes only the junction, never its target.
                link.rmdir()


if __name__ == "__main__":
    unittest.main(verbosity=2)
