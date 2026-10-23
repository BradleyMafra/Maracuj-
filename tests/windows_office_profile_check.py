#!/usr/bin/env python3
"""Synthetic tests for the portable, private Windows Office profile cache."""

import hashlib
import importlib.util
import json
import marshal
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import zipfile


PROJECT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


profile = load("profile_check_helper", PROJECT / "helpers/windows_office_profile.py")
builder = load("profile_check_builder", PROJECT / "packaging/windows/prepare_office_profile.py")


class OfficeProfileChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="maracuja-profile-check-",
                                                      dir=profile.filesystem_path(Path(tempfile.gettempdir())))
        self.base = Path(self.temporary.name)
        self.stage = self.base / "Maracujá portátil"
        for name in ("version.ini", "bootstrap.ini", "fundamental.ini"):
            self.file(self.stage / "runtime/libreoffice/program" / name, b"synthetic-runtime")
        self.file(self.stage / "runtime/libreoffice/share/extensions/example/description.xml", b"<extension/>")
        self.seed = self.base / "synthetic-profile"
        self.file(self.seed / "user/extensions/buildid", b"synthetic-build")
        self.file(self.seed / "user/extensions/bundled/lastsynchronized", b"1")
        self.file(self.seed / "user/extensions/bundled/registry/configmgr.ini",
                  b"DATA=$BUNDLED_EXTENSIONS_USER/registry/safe.xcu\n")
        self.file(self.seed / "user/extensions/bundled/registry/safe.xcu", b"<registry/>")
        self.file(self.seed / "user/extensions/shared/lastsynchronized", b"1")
        self.cache = self.stage / profile.CACHE_DIRECTORY

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def file(path, data):
        path = profile.filesystem_path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def package(self):
        return builder.package_registry(self.seed, self.stage, self.cache)

    def job(self, name="job"):
        work = self.base / name
        self.file(work / "profile/user/registrymodifications.xcu", b"PRIVATE-SECURITY-POLICY")
        return work

    def inventory(self, directory):
        return {path.relative_to(directory).as_posix(): path.read_bytes()
                for path in directory.rglob("*") if path.is_file()}

    def change_archive(self, extra_name, payload):
        manifest = json.loads((self.cache / "manifest.json").read_text())
        with zipfile.ZipFile(self.cache / "extensions.zip", "a") as archive:
            archive.writestr(extra_name, payload)
        manifest["files"][extra_name] = {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
        manifest["archive_sha256"] = hashlib.sha256((self.cache / "extensions.zip").read_bytes()).hexdigest()
        (self.cache / "manifest.json").write_text(json.dumps(manifest))

    def test_copies_into_independent_profiles_and_retains_security_policy(self):
        self.package()
        before = self.inventory(self.cache)
        first, second = self.job("first"), self.job("second")
        self.assertTrue(profile.copy_prepared_profile(first, self.stage))
        self.assertTrue(profile.copy_prepared_profile(second, self.stage))
        self.file(first / "profile/user/extensions/buildid", b"first-job-only")
        self.assertEqual((second / "profile/user/extensions/buildid").read_bytes(), b"synthetic-build")
        self.assertEqual((first / "profile/user/registrymodifications.xcu").read_bytes(), b"PRIVATE-SECURITY-POLICY")
        self.assertEqual(self.inventory(self.cache), before)

    def test_excludes_document_history_recovery_macros_and_temporary_extensions(self):
        for name in ("user/registrymodifications.xcu", "user/pack/registrymodifications.pack",
                     "user/backup/document.odt", "user/basic/secret.xba", "crash/dump.ini",
                     "user/extensions/tmp/registry/document-cache", "user/extensions/bundled/.lock"):
            self.file(self.seed / name, b"PRIVATE-DOCUMENT-CONTENTS")
        self.package()
        files = profile.read_template(self.stage)
        self.assertEqual(len(files), 5)
        self.assertTrue(all(b"PRIVATE-DOCUMENT-CONTENTS" not in value for value in files.values()))

    def test_absent_or_corrupt_template_falls_back_without_partial_profile(self):
        work = self.job()
        self.assertFalse(profile.copy_prepared_profile(work, self.stage))
        self.package()
        archive = self.cache / "extensions.zip"
        archive.write_bytes(archive.read_bytes()[:-8])
        self.assertFalse(profile.copy_prepared_profile(work, self.stage))
        self.assertFalse((work / "profile/user/extensions").exists())
        self.assertEqual((work / "profile/user/registrymodifications.xcu").read_bytes(), b"PRIVATE-SECURITY-POLICY")

    def test_runtime_update_invalidates_cache(self):
        self.package()
        self.assertTrue(profile.copy_prepared_profile(self.job("before"), self.stage))
        self.file(self.stage / "runtime/libreoffice/program/version.ini", b"new-version")
        self.assertFalse(profile.copy_prepared_profile(self.job("after"), self.stage))

    def test_extension_configuration_update_invalidates_cache(self):
        self.package()
        self.file(self.stage / "runtime/libreoffice/share/extensions/example/description.xml", b"<updated/>")
        self.assertFalse(profile.copy_prepared_profile(self.job(), self.stage))

    def test_source_backed_bytecode_creation_sanitization_and_removal_keep_identity(self):
        source = self.file(self.stage / "runtime/libreoffice/share/extensions/example/pythonpath/handler.py",
                           b"VALUE = 1\n")
        self.package()
        expected = profile.runtime_identity(self.stage)
        payload = (importlib.util.MAGIC_NUMBER + b"\0" * 12
                   + marshal.dumps(compile(source.read_bytes(), str(source), "exec")))
        cache = self.file(source.parent / "__pycache__/handler.cpython-313.pyc", payload)
        legacy = self.file(source.with_suffix(".pyc"), payload)
        self.assertEqual(profile.runtime_identity(self.stage), expected)
        privacy = load("office_profile_privacy_check", PROJECT / "packaging/archive_privacy.py")
        sanitized, changed = privacy.sanitize_bytecode(payload, self.stage, "handler.pyc")
        self.assertTrue(changed)
        for path in (cache, legacy):
            path.write_bytes(sanitized)
        self.assertEqual(profile.runtime_identity(self.stage), expected)
        self.assertTrue(profile.copy_prepared_profile(self.job(), self.stage))
        cache.unlink()
        legacy.unlink()
        self.assertEqual(profile.runtime_identity(self.stage), expected)

    def test_same_size_python_source_change_invalidates_cache(self):
        source = self.file(self.stage / "runtime/libreoffice/share/extensions/example/handler.py", b"VALUE = 1\n")
        self.package()
        source.write_bytes(b"VALUE = 2\n")
        self.assertFalse(profile.copy_prepared_profile(self.job(), self.stage))

    def test_sourceless_bytecode_remains_part_of_runtime_identity(self):
        cache = self.file(self.stage / "runtime/libreoffice/share/extensions/example/handler.pyc", b"code-version-1")
        self.package()
        cache.write_bytes(b"code-version-2")
        self.assertFalse(profile.copy_prepared_profile(self.job(), self.stage))

    def test_removing_source_with_remaining_bytecode_invalidates_cache(self):
        source = self.file(self.stage / "runtime/libreoffice/share/extensions/example/handler.py", b"VALUE = 1\n")
        self.file(source.parent / "__pycache__/handler.cpython-313.pyc", b"bytecode")
        self.package()
        source.unlink()
        self.assertFalse(profile.copy_prepared_profile(self.job(), self.stage))

    def test_relocated_runtime_works_without_original_paths(self):
        self.package()
        moved = self.base / "Outro local áç com espaços"
        # Both resolved paths are children of this test's owned temporary root.
        self.assertTrue(self.stage.resolve().is_relative_to(self.base.resolve()))
        self.assertTrue(moved.resolve().is_relative_to(self.base.resolve()))
        self.stage.rename(moved)
        self.assertFalse(self.stage.exists())
        self.assertTrue(profile.copy_prepared_profile(self.job(), moved))
        for path in (moved / profile.CACHE_DIRECTORY).iterdir():
            self.assertNotIn(str(self.base).encode(), path.read_bytes())

    def test_rejects_paths_outside_extension_registry_even_with_valid_hash(self):
        for name in ("../outside", "user/registrymodifications.xcu", "user/extensions/bundled/../../outside",
                     "user/extensions/bundled/C:/escape"):
            with self.subTest(name=name):
                if self.cache.exists():
                    shutil.rmtree(self.cache)
                self.package()
                self.change_archive(name, b"unexpected")
                self.assertFalse(profile.copy_prepared_profile(self.job(name="job" + str(len(name))), self.stage))

    def test_rejects_absolute_paths_in_build_and_runtime(self):
        entry = "user/extensions/bundled/registry/leak.xcu"
        for payload in (b"file:///C:/Users/Builder/secret", b"C:\\Users\\Builder", "C:/Users/Builder".encode("utf-16le")):
            self.file(self.seed / entry, payload)
            with self.assertRaises(RuntimeError):
                self.package()
        (self.seed / entry).unlink()
        self.package()
        self.change_archive(entry, b"file:///C:/Users/Builder/private")
        self.assertFalse(profile.copy_prepared_profile(self.job(), self.stage))

    def test_registry_namespace_urls_are_portable(self):
        self.assertTrue(profile.portable_payload(b'<ext xmlns="http://openoffice.org/example"/>'))
        self.assertTrue(profile.portable_payload(b'License: https://mozilla.org/MPL/2.0/'))
        self.assertFalse(profile.portable_payload(b'\\\\server\\private\\document'))

    def test_rejects_wrong_schema_bad_inventory_and_entry_hash(self):
        self.package()
        path = self.cache / "manifest.json"
        original = json.loads(path.read_text())
        for manifest in ([], {**original, "schema": 999}, {**original, "files": {}},
                         {**original, "files": {name: {**data, "sha256": "0" * 64}
                                               for name, data in original["files"].items()}}):
            path.write_text(json.dumps(manifest))
            self.assertFalse(profile.copy_prepared_profile(self.job(), self.stage))

    def test_existing_profile_extensions_are_not_overwritten(self):
        self.package()
        work = self.job()
        self.file(work / "profile/user/extensions/existing", b"keep")
        self.assertFalse(profile.copy_prepared_profile(work, self.stage))
        self.assertEqual((work / "profile/user/extensions/existing").read_bytes(), b"keep")

    def test_failed_copy_removes_partial_registry_before_fallback(self):
        self.package()
        work = self.job()
        original = Path.write_bytes

        def fail_mid_copy(path, data):
            if path.name == "safe.xcu":
                raise OSError("simulated write failure")
            return original(path, data)

        with patch.object(Path, "write_bytes", fail_mid_copy):
            self.assertFalse(profile.copy_prepared_profile(work, self.stage))
        self.assertFalse((work / "profile/user/extensions").exists())
        self.assertEqual((work / "profile/user/registrymodifications.xcu").read_bytes(), b"PRIVATE-SECURITY-POLICY")

    def test_failed_cleanup_aborts_instead_of_using_partial_registry(self):
        self.package()
        work = self.job()
        with patch.object(Path, "write_bytes", side_effect=OSError("simulated write failure")), \
                patch.object(profile.shutil, "rmtree", side_effect=OSError("simulated cleanup failure")):
            with self.assertRaises(OSError):
                profile.copy_prepared_profile(work, self.stage)

    @unittest.skipUnless(os.name == "nt", "Windows extended path check")
    def test_long_accented_destination_and_cleanup(self):
        self.package()
        work = self.job("conversão-" + "x" * 100 + "/arquivo-" + "y" * 100)
        self.assertGreater(len(str(work / "profile/user/extensions/bundled/registry/safe.xcu")), 260)
        self.assertTrue(profile.copy_prepared_profile(work, self.stage))
        self.assertTrue(profile.filesystem_path(work / "profile/user/extensions/bundled/registry/safe.xcu").is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
