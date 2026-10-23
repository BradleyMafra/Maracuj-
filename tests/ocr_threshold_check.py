#!/usr/bin/env python3
"""Check OCR candidate deadlines and native-resource ownership without a DLL."""
import ctypes
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

from PIL import Image

spec = importlib.util.spec_from_file_location("ocr_threshold_engine", Path(__file__).resolve().parents[1] / "helpers/ocr_engine.py")
ocr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ocr)


class FakeAPI:
    def __init__(self, clock, texts, confidences, durations=None, results=None, rejected_method=None, image_durations=None):
        self.clock, self.texts, self.confidences = clock, texts, confidences
        self.durations = durations or [0] * len(texts)
        self.results = results or [0] * len(texts)
        self.rejected_method = rejected_method
        self.image_durations = image_durations or [0] * len(texts)
        self.prepared = 0
        self.attempt = -1
        self.methods, self.deadlines = [], []
        self.buffers = {}
        self.deleted_texts = self.deleted_monitors = self.cleared = 0

    def TessBaseAPISetVariable(self, _handle, name, value):
        assert name == b"thresholding_method"
        method = int(value)
        self.methods.append(method)
        return method != self.rejected_method

    def TessBaseAPIClearAdaptiveClassifier(self, _handle):
        pass

    def TessBaseAPISetImage(self, *_arguments):
        self.clock[0] += self.image_durations[self.prepared]
        self.prepared += 1

    def TessBaseAPISetSourceResolution(self, *_arguments):
        pass

    def TessMonitorCreate(self):
        return 1

    def TessMonitorSetDeadlineMSecs(self, _monitor, duration):
        self.deadlines.append(duration)

    def TessBaseAPIRecognize(self, _handle, _monitor):
        self.attempt += 1
        self.clock[0] += self.durations[self.attempt]
        return self.results[self.attempt]

    def TessBaseAPIGetUTF8Text(self, _handle):
        buffer = ctypes.create_string_buffer(self.texts[self.attempt].encode("utf-8"))
        pointer = ctypes.addressof(buffer)
        self.buffers[pointer] = buffer
        return pointer

    def TessBaseAPIMeanTextConf(self, _handle):
        return self.confidences[self.attempt]

    def TessDeleteText(self, pointer):
        del self.buffers[pointer]
        self.deleted_texts += 1

    def TessMonitorDelete(self, _monitor):
        self.deleted_monitors += 1

    def TessBaseAPIClear(self, _handle):
        self.cleared += 1


class OCRThresholdChecks(unittest.TestCase):
    def recognize(self, api, windows=True):
        engine = ocr._NativeEngine.__new__(ocr._NativeEngine)
        engine.api, engine.handle, engine.windows = api, 1, windows
        with patch.object(ocr.time, "monotonic", side_effect=lambda: api.clock[0]):
            with Image.new("RGB", (32, 24), "white") as image:
                return engine.recognize(image)

    def test_higher_confidence_wins_and_ties_keep_the_original_method(self):
        for confidence, expected in [([93, 95], "second"), ([95, 95], "first"), ([96, 93], "first"), ([-1, -1], "first")]:
            api = FakeAPI([0], ["first", "second"], confidence)
            self.assertEqual(self.recognize(api), expected)
            self.assertEqual(api.methods, [0, 1])
            self.assertEqual(api.deleted_texts, 2)
            self.assertEqual(api.deleted_monitors, 2)
            self.assertEqual(api.cleared, 2)
            self.assertFalse(api.buffers)

    def test_second_attempt_uses_only_remaining_budget_and_timeout_keeps_first(self):
        api = FakeAPI([0], ["valid first", "unavailable"], [94, 0], durations=[40, 5], results=[0, 1])
        self.assertEqual(self.recognize(api), "valid first")
        self.assertEqual(api.deadlines, [45000, 5000])
        self.assertEqual(api.deleted_texts, 1)
        self.assertEqual(api.deleted_monitors, 2)
        self.assertEqual(api.cleared, 2)
        self.assertFalse(api.buffers)

    def test_exhausted_budget_never_starts_a_second_attempt(self):
        api = FakeAPI([0], ["valid first"], [94], durations=[45])
        self.assertEqual(self.recognize(api), "valid first")
        self.assertEqual(api.methods, [0])
        self.assertEqual(api.deadlines, [45000])

    def test_monitor_deadlines_include_time_spent_preparing_each_attempt(self):
        api = FakeAPI([0], ["first", "second"], [94, 99], durations=[3, 0], image_durations=[5, 7])
        self.assertEqual(self.recognize(api), "second")
        self.assertEqual(api.deadlines, [40000, 30000])

    def test_preparation_exhausting_remaining_budget_keeps_first_and_cleans_up(self):
        api = FakeAPI([0], ["first", "unused"], [94, 99], durations=[40, 0], image_durations=[0, 6])
        self.assertEqual(self.recognize(api), "first")
        self.assertEqual(api.deadlines, [45000])
        self.assertEqual(api.deleted_monitors, 2)
        self.assertEqual(api.cleared, 2)
        self.assertFalse(api.buffers)

    def test_first_timeout_has_no_fallback_and_releases_its_monitor(self):
        api = FakeAPI([0], ["unavailable"], [0], durations=[45], results=[1])
        with self.assertRaises(ocr.OCRError):
            self.recognize(api)
        self.assertEqual(api.deleted_texts, 0)
        self.assertEqual(api.deleted_monitors, 1)
        self.assertEqual(api.cleared, 1)

    def test_required_configuration_and_text_limit_errors_are_not_hidden(self):
        api = FakeAPI([0], ["first", "second"], [94, 99], rejected_method=1)
        with self.assertRaises(ocr.OCRError):
            self.recognize(api)
        self.assertEqual(api.deleted_texts, 1)
        api = FakeAPI([0], ["ok", "too long"], [94, 99])
        with patch.object(ocr, "MAX_TEXT_BYTES", 3):
            with self.assertRaises(ocr.OCRError):
                self.recognize(api)
        self.assertEqual(api.deleted_texts, 2)
        self.assertFalse(api.buffers)

    def test_linux_keeps_one_pass_without_changing_threshold_parameters(self):
        api = FakeAPI([0], ["one pass"], [94], image_durations=[5])
        self.assertEqual(self.recognize(api, windows=False), "one pass")
        self.assertEqual(api.methods, [])
        self.assertEqual(api.deadlines, [45000])
        self.assertEqual(api.deleted_texts, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
