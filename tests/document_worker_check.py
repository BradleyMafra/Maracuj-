#!/usr/bin/env python3
"""Bounded worker protocol and per-job isolation with disposable synthetic data."""
from __future__ import annotations

import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("document_worker_test", Path(__file__).resolve().parents[1] / "helpers/document_convert.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="maracuja-worker-check-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.pdf"
        self.source.write_bytes(b"%PDF-synthetic")

    def tearDown(self):
        self.temporary.cleanup()

    def request(self, index=1, **values):
        return {"id": index, "input": str(self.source), "output": str(self.root / f"out-{index}.txt"),
                "format": "txt", "image_ocr": False, **values}

    def serve(self, requests, converter=None):
        payload = b"".join(json.dumps(request).encode() + b"\n" for request in requests)
        output = io.StringIO()
        def convert(source, target, destination, tessdata, progress):
            progress(0, 1, "text")
            destination.write_text(source.name, encoding="utf-8")
            progress(1, 1, "text")
        with patch.object(worker, "has_module", return_value=True), \
                patch.object(worker, "convert_pdf", side_effect=converter or convert), \
                patch.object(worker, "capabilities", side_effect=AssertionError("global probe forbidden")):
            status = worker.serve("", "", io.BytesIO(payload), output)
        return status, [json.loads(line) for line in output.getvalue().splitlines()]

    def test_multiple_jobs_match_progress_and_results_without_global_probe(self):
        status, events = self.serve([self.request(1), self.request(2)])
        self.assertEqual(status, 0)
        self.assertEqual([e["id"] for e in events], [1, 1, 1, 2, 2, 2])
        self.assertEqual([e["ok"] for e in events if e["type"] == "result"], [True, True])
        self.assertEqual((self.root / "out-1.txt").read_text(), "source.pdf")
        self.assertEqual(self.source.read_bytes(), b"%PDF-synthetic")

    def test_existing_output_is_untouched_and_worker_stops_after_failure(self):
        destination = self.root / "out-1.txt"
        destination.write_text("original")
        _, events = self.serve([self.request(1), self.request(2)])
        self.assertFalse(events[-1]["ok"])
        self.assertEqual(destination.read_text(), "original")
        self.assertFalse((self.root / "out-2.txt").exists())

    def test_partial_output_is_removed_and_exception_content_is_sanitized(self):
        def fail(source, target, destination, tessdata, progress):
            destination.write_text("partial secret")
            raise RuntimeError("private filename and document content")
        _, events = self.serve([self.request()], fail)
        self.assertFalse(events[-1]["ok"])
        self.assertNotIn("private", events[-1]["error"])
        self.assertFalse((self.root / "out-1.txt").exists())

    def test_batch_has_finite_job_limit(self):
        _, events = self.serve([self.request(index) for index in range(1, 35)])
        self.assertEqual(sum(event["type"] == "result" for event in events), 32)
        self.assertFalse((self.root / "out-33.txt").exists())

    def test_repeated_and_boolean_ids_are_rejected(self):
        for requests in ([self.request(1), self.request(1)], [self.request(True)]):
            status, events = self.serve(requests)
            self.assertEqual(status, 2)
            self.assertIsNone(events[-1]["id"])
            self.assertFalse(events[-1]["ok"])
            for path in self.root.glob("out-*.txt"):
                path.unlink()

    def test_oversized_truncated_or_malformed_protocol_is_bounded(self):
        for payload in (b"x" * (worker.MAX_REQUEST_BYTES + 2), b"{}", b"[1]\n", b"\xff\n"):
            output = io.StringIO()
            status = worker.serve("", "", io.BytesIO(payload), output)
            self.assertEqual(status, 2)
            self.assertLess(len(output.getvalue()), 512)
            self.assertFalse(json.loads(output.getvalue())["ok"])

    def test_only_requested_pdf_dependencies_are_consulted(self):
        with patch.object(worker, "ocr_module", side_effect=AssertionError("OCR not requested")), \
                patch.object(worker, "office_root", side_effect=AssertionError("Office not requested")), \
                patch.object(worker, "has_module", side_effect=lambda name: name == "pypdf"):
            self.assertTrue(worker.route_available("pdf", True, False, "txt", ""))
            self.assertFalse(worker.route_available("pdf", True, False, "docx", ""))
            self.assertFalse(worker.route_available("pdf", True, False, "xlsx", ""))

    def test_selectable_pdf_does_not_load_ocr(self):
        with patch.object(worker, "ocr_module", side_effect=AssertionError("OCR not needed")), \
                patch.object(worker, "pdf_page_text", return_value="selectable text"):
            pages = worker.pdf_text(SimpleNamespace(pages=[object(), object()]), self.source,
                                    self.root / "unused", progress=lambda *args: None)
        self.assertEqual(pages, ["selectable text", "selectable text"])

    def test_blank_pdf_requests_ocr_only_when_needed(self):
        module = SimpleNamespace(capabilities=lambda *args: {"ocr": False})
        with patch.object(worker, "ocr_module", return_value=module) as probe, \
                patch.object(worker, "pdf_page_text", side_effect=["text", "", ""]):
            pages = worker.pdf_text(SimpleNamespace(pages=[object(), object(), object()]), self.source,
                                    self.root / "unused", progress=lambda *args: None)
        self.assertEqual(pages, ["text", "", ""])
        self.assertEqual(probe.call_count, 1)


if __name__ == "__main__":
    unittest.main()
