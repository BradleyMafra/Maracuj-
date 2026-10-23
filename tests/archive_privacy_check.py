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


if __name__ == "__main__":
    unittest.main(verbosity=2)
