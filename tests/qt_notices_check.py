#!/usr/bin/env python3
"""Check hash-gated Qt notice collection with small generated fixtures."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("qt_notice_collector", PROJECT / "packaging/windows/collect_qt_notices.py")
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


def fixture():
    document = {"schema_version": 1, "qt_version": collector.QT_VERSION, "tag": collector.QT_TAG,
                "modules": {}, "files": []}
    payloads = {}

    def add(module, path, kind, payload):
        digest = hashlib.sha256(payload).hexdigest()
        payloads[digest] = payload
        document["files"].append({"module": module, "path": path, "kind": kind,
            "url": collector.source_url(module, path), "sha256": digest, "bytes": len(payload),
            "source_blob_sha1": hashlib.sha1(b"blob " + str(len(payload)).encode() + b"\0" + payload).hexdigest()})

    for module in sorted(collector.MODULES):
        document["modules"][module] = {"source_tree_sha1": "1" * 40, "license_files": 1,
                                       "attribution_files": 1 if module == "qtbase" else 0}
        add(module, "LICENSES/LGPL-3.0-only.txt", "license", b"Generated license fixture\n")
    # The actual upstream KWin JSON also contains a literal copyright newline.
    add("qtbase", "src/example/qt_attribution.json", "attribution",
        b'{"Id":"example","Path":"different/code/folder","Copyright":"First line\nSecond line",'
        b'"LicenseFiles":["../notices/COPYING"],"CopyrightFile":"COPYRIGHT"}\n')
    add("qtbase", "src/notices/COPYING", "notice", b"Generated copyright/license notice\n")
    add("qtbase", "src/example/COPYRIGHT", "notice", b"Generated copyright statement\n")
    return document, payloads


class QtNoticeChecks(unittest.TestCase):
    def test_collect_preserves_original_bytes_paths_and_source_inventory(self):
        document, payloads = fixture()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            stage.mkdir()

            def download(record, cache):
                path = cache / record["sha256"]
                path.write_bytes(payloads[record["sha256"]])
                return path

            result = collector.collect(stage, root / "private-cache", document, download)
            self.assertTrue(result["sha256_verified"])
            self.assertEqual(result["notice_files"], len(document["files"]))
            target = stage / "licenses/Qt" / collector.QT_VERSION
            for record in document["files"]:
                self.assertEqual((target / record["module"] / record["path"]).read_bytes(), payloads[record["sha256"]])
            self.assertEqual(json.loads((target / "sources.json").read_text()), document)
            self.assertNotIn(str(root), (target / "sources.json").read_text() + (target / "README.txt").read_text())

    def test_corrupt_cache_fails_without_network_or_overwriting_the_cache(self):
        document, _ = fixture()
        record = document["files"][0]
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            corrupt = cache / record["sha256"]
            corrupt.write_bytes(b"corrupt")
            with mock.patch.object(collector.urllib.request, "urlopen") as network:
                with self.assertRaises(RuntimeError):
                    collector.fetch(record, cache)
                network.assert_not_called()
            self.assertEqual(corrupt.read_bytes(), b"corrupt")

    def test_wrong_download_hash_fails_and_removes_partial_files(self):
        document, _ = fixture()
        record = document["files"][0]

        class Response(io.BytesIO):
            def geturl(self):
                return record["url"]

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            bad = b"x" * record["bytes"]
            with mock.patch.object(collector.urllib.request, "urlopen", return_value=Response(bad)):
                with self.assertRaises(RuntimeError):
                    collector.fetch(record, cache)
            self.assertEqual(list(cache.iterdir()), [])

    def test_unofficial_urls_traversal_duplicate_and_incomplete_locks_are_rejected(self):
        document, _ = fixture()
        variants = []
        changed = copy.deepcopy(document)
        changed["files"][0]["url"] = "https://example.invalid/license"
        variants.append(changed)
        changed = copy.deepcopy(document)
        changed["files"][0]["path"] = "../../outside"
        variants.append(changed)
        changed = copy.deepcopy(document)
        changed["files"].append(copy.deepcopy(changed["files"][0]))
        variants.append(changed)
        changed = copy.deepcopy(document)
        changed["files"].pop(0)
        variants.append(changed)
        for changed in variants:
            with self.subTest(changed=changed["files"][0]["path"]), self.assertRaises(ValueError):
                collector.validate_lock(changed)

    def test_missing_referenced_document_fails_before_copying_any_notices(self):
        document, payloads = fixture()
        document["files"] = [record for record in document["files"] if record["path"] != "src/notices/COPYING"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            stage.mkdir()

            def download(record, cache):
                path = cache / record["sha256"]
                path.write_bytes(payloads[record["sha256"]])
                return path

            with self.assertRaises(ValueError):
                collector.collect(stage, root / "cache", document, download)
            self.assertEqual(list(stage.iterdir()), [])

    def test_private_cache_must_remain_outside_the_release(self):
        document, _ = fixture()
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            with self.assertRaises(ValueError):
                collector.collect(stage, stage / "cache", document)
            self.assertEqual(list(stage.iterdir()), [])

    def test_references_cannot_escape_a_module(self):
        payload = json.dumps({"LicenseFile": "../../../outside"}).encode()
        with self.assertRaises(ValueError):
            collector.attribution_references("src/qt_attribution.json", payload)


if __name__ == "__main__":
    unittest.main(verbosity=2)
