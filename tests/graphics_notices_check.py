#!/usr/bin/env python3
"""Verify that reviewed graphics notices cannot silently change or disappear."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "packaging/windows/collect_graphics_notices.py"
spec = importlib.util.spec_from_file_location("graphics_notices", MODULE)
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


def identity(data):
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class GraphicsNoticeChecks(unittest.TestCase):
    def fixture(self, root):
        stage, cache = root / "stage", root / "private-cache"
        stage.mkdir()
        cache.mkdir()
        document = {"schema_version": 1, "components": {name: {} for name in ["Mesa", "LLVM", "WindowsSDK"]},
                    "binaries": {}, "sources": []}
        for name in notices.REQUIRED_BINARIES:
            payload = ("fixture " + name).encode()
            (stage / name).write_bytes(payload)
            document["binaries"][name] = identity(payload)
        for component in document["components"]:
            payload = ("original notice " + component + "\r\n").encode()
            (cache / component).write_bytes(payload)
            document["sources"].append({"id": component, "component": component, "format": "file",
                "url": "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-3.6.2/llvm/LICENSE.TXT",
                **identity(payload), "documents": [{"path": component + "/NOTICE", **identity(payload)}]})
        def download(source, _cache):
            return cache / source["id"]
        return stage, cache, document, download

    def test_exact_original_documents_and_origins_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage, cache, document, download = self.fixture(Path(temporary))
            result = notices.collect(stage, cache, document, download)
            self.assertTrue(result["sha256_verified"])
            for source in document["sources"]:
                self.assertEqual((stage / "licenses/graphics" / source["documents"][0]["path"]).read_bytes(),
                                 (cache / source["id"]).read_bytes())
            self.assertEqual(json.loads((stage / "licenses/graphics/sources.json").read_text()), document)

    def test_changed_binary_or_corrupt_notice_stops_before_any_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage, cache, document, download = self.fixture(Path(temporary))
            (stage / "opengl32sw.dll").write_bytes(b"unexpected version")
            with self.assertRaises(RuntimeError):
                notices.collect(stage, cache, document, download)
            self.assertFalse((stage / "licenses").exists())
            (stage / "opengl32sw.dll").write_bytes(b"fixture opengl32sw.dll")
            (cache / "LLVM").write_bytes(b"changed notice")
            with self.assertRaises(RuntimeError):
                notices.collect(stage, cache, document, download)
            self.assertFalse((stage / "licenses").exists())

    def test_missing_components_unofficial_sources_and_paths_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage, cache, document, download = self.fixture(Path(temporary))
            variants = []
            item = copy.deepcopy(document); item["sources"].pop(); variants.append(item)
            item = copy.deepcopy(document); item["sources"][0]["url"] = "https://example.com/notice"; variants.append(item)
            for path in ["../escape", "/absolute", "C:/private", "bad\\path"]:
                item = copy.deepcopy(document); item["sources"][0]["documents"][0]["path"] = path; variants.append(item)
            for item in variants:
                with self.assertRaises(ValueError):
                    notices.collect(stage, cache, item, download)
            with self.assertRaises(ValueError):
                notices.collect(stage, stage / "cache", document, download)
            self.assertFalse((stage / "licenses").exists())

    def test_only_named_regular_archive_members_are_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage, cache, document, download = self.fixture(Path(temporary))
            original = document["sources"][0]
            payload = (cache / original["id"]).read_bytes()
            source = cache / original["id"]
            for symbolic in [False, True]:
                with tarfile.open(source, "w") as archive:
                    member = tarfile.TarInfo("mesa/docs/license.html")
                    if symbolic:
                        member.type = tarfile.SYMTYPE
                        member.linkname = "/private/file"
                    else:
                        member.size = len(payload)
                    archive.addfile(member, None if symbolic else io.BytesIO(payload))
                original.update({"format": "tar", **identity(source.read_bytes())})
                original["documents"][0]["member"] = "mesa/docs/license.html"
                if symbolic:
                    with self.assertRaises(ValueError):
                        notices.collect(stage, cache, document, download)
                else:
                    notices.collect(stage, cache, document, download)
                    self.assertEqual((stage / "licenses/graphics/Mesa/NOTICE").read_bytes(), payload)

    def test_corrupt_existing_cache_fails_without_network(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, cache, document, _ = self.fixture(Path(temporary))
            source = document["sources"][0]
            (cache / source["sha256"]).write_bytes(b"corrupt")
            with self.assertRaises(RuntimeError):
                notices.fetch(source, cache)

    def test_vendored_original_notice_is_hash_verified_and_cannot_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, cache, document, download = self.fixture(root)
            source = document["sources"][2]
            base = root / "packaging/windows"
            local = base / "notices/windows-sdk-8.1/license.rtf"
            local.parent.mkdir(parents=True)
            local.write_bytes((cache / source["id"]).read_bytes())
            source.update({"format": "vendored", "vendored_path": "notices/windows-sdk-8.1/license.rtf",
                           "source_archive": {"member": "AttachedContainer/u1", **identity(b"reviewed archive")}})
            original_module_file = notices.__file__
            notices.__file__ = str(base / "collect_graphics_notices.py")
            try:
                notices.collect(stage, cache, document, download)
                self.assertEqual((stage / "licenses/graphics/WindowsSDK/NOTICE").read_bytes(), local.read_bytes())
                local.write_bytes(b"converted line endings or changed terms")
                with self.assertRaises(RuntimeError):
                    notices.collect(stage, cache, document, download)
                source["vendored_path"] = "notices/../../outside"
                with self.assertRaises(ValueError):
                    notices.collect(stage, cache, document, download)
            finally:
                notices.__file__ = original_module_file


if __name__ == "__main__":
    unittest.main(verbosity=2)
