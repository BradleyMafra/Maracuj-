#!/usr/bin/env python3
"""Small generated-fixture checks; never starts Office, OCR, network or tools."""

from __future__ import annotations

import base64
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile


HELPERS = Path(__file__).resolve().parents[1] / "helpers"


def load_helper(name):
    specification = importlib.util.spec_from_file_location("maracuja_check_" + name, HELPERS / (name + ".py"))
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


platform = load_helper("platform_runtime")
documents = load_helper("document_convert")
ocr = load_helper("ocr_engine")


class RuntimeChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="maracuja-platform-check-")
        self.root = Path(self.temporary.name) / "Maracujá com espaços"
        self.root.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def file(self, name, contents=b"fixture"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def configuration(self, values):
        self.file("runtime-paths.json", json.dumps(values, ensure_ascii=False).encode("utf-8"))
        config = platform.Runtime(str(self.root))
        config.windows = True
        return config

    def test_relative_and_nested_windows_tools_use_application_directory(self):
        poppler = self.file("tools/pdftoppm.exe")
        library = self.file("ocr/lib/libtesseract-5.5.dll")
        python = self.file("runtime/python/python.exe")
        config = self.configuration({"python": "runtime/python/python.exe", "pdftoppm": "missing.exe",
                                     "tools": {"pdftoppm": "tools/pdftoppm.exe",
                                               "tesseract_library": "ocr/lib/libtesseract-5.5.dll"}})
        with patch.dict(os.environ, {"PATH": str(self.root.parent)}):
            self.assertEqual(config.tool("python"), python.resolve())
            self.assertEqual(config.tool("pdftoppm"), poppler.resolve())
            self.assertEqual(config.tool("tesseract_library"), library.resolve())
            self.assertIsNone(config.tool("pdfunite"))
        self.assertIn((self.root / "tools").resolve(), config.dll_directories(library))
        self.assertIn(library.parent.resolve(), config.dll_directories(library))
        self.assertEqual(config.subprocess_options(), {"creationflags": 0x08000000})

    def test_missing_configured_dependency_never_falls_back_to_another_file(self):
        self.file("tools/pdftoppm.exe")
        config = self.configuration({"pdftoppm": "not-installed/pdftoppm.exe"})
        self.assertIsNone(config.tool("pdftoppm"))

    def test_explicit_relative_paths_and_tessdata_are_application_relative(self):
        office = self.file("runtime/libreoffice/program/soffice.com")
        config = self.configuration({"tessdata": "ocr/tessdata",
                                     "tesseract_library": "ocr/lib/libtesseract-5.5.dll"})
        self.assertEqual(config.tool("soffice", "runtime/libreoffice/program/soffice.com"), office.resolve())
        self.assertEqual(config.tessdata(), (self.root / "ocr/tessdata").resolve())
        self.assertEqual(config.tessdata("other models"), (self.root / "other models").resolve())
        # Resolving OCR dependencies reads metadata only: no CDLL/recognition call.
        self.file("ocr/lib/libtesseract-5.5.dll")
        with patch.object(ocr.ctypes, "CDLL", side_effect=AssertionError("DLL must not load")):
            self.assertEqual(ocr.library_path(str(self.root)), self.root / "ocr/lib/libtesseract-5.5.dll")

    def test_markdown_requires_real_import_and_export_filter_flags(self):
        root = self.root / "office"
        registry = self.file("office/share/registry/writer.xcd")
        template = ('<root xmlns:oor="http://openoffice.org/2001/registry">'
                    '<node oor:name="Markdown"><prop oor:name="Flags"><value>{}</value>'
                    '</prop></node></root>')
        registry.write_text(template.format("IMPORT EXPORT ALIEN"), encoding="utf-8")
        self.assertTrue(platform.markdown_filter(root))
        registry.write_text(template.format("IMPORT ALIEN"), encoding="utf-8")
        self.assertFalse(platform.markdown_filter(root))
        registry.write_text("<invalid>", encoding="utf-8")
        self.assertFalse(platform.markdown_filter(root))

    def archive(self, name, entries):
        path = self.root / name
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as package:
            for member, data in entries.items():
                package.writestr(member, data)
        return path

    @staticmethod
    def word_document(extra=""):
        return ('<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                '<w:body><w:p><w:r><w:t>Maracujá normal</w:t></w:r></w:p>' + extra
                + '</w:body></w:document>')

    def assert_rejected_without_mutation(self, path, extension):
        original = path.read_bytes()
        with self.assertRaises(documents.ConversionError):
            documents.prepare_windows_source(path, extension)
        self.assertEqual(path.read_bytes(), original)

    def test_normal_docx_with_internal_relationship_is_preserved(self):
        path = self.archive("normal.docx", {
            "word/document.xml": self.word_document(),
            "word/_rels/document.xml.rels": ('<Relationships><Relationship Id="image" '
                                             'Target="../media/picture.png"/></Relationships>'),
            "media/picture.png": b"private fixture",
        })
        original = path.read_bytes()
        documents.prepare_windows_source(path, "docx")
        self.assertEqual(path.read_bytes(), original)

    def test_docx_external_relationship_and_active_field_are_rejected(self):
        for index, target in enumerate(["https://example.invalid/no-request", "file:///C:/outside.txt"]):
            with self.subTest(target=target):
                path = self.archive("external-" + str(index) + ".docx", {
                    "word/document.xml": self.word_document(),
                    "word/_rels/document.xml.rels": ('<Relationships><Relationship Id="external" '
                                                     'TargetMode="External" Target="' + target
                                                     + '"/></Relationships>'),
                })
                self.assert_rejected_without_mutation(path, "docx")
        field = self.archive("field.docx", {
            "word/document.xml": self.word_document('<w:p><w:r><w:instrText>INCLUDEPICTURE '
                                                     'file:///C:/outside.png</w:instrText></w:r></w:p>')})
        self.assert_rejected_without_mutation(field, "docx")

    def test_docx_macros_svg_and_utf16_doctype_are_rejected(self):
        for index, member in enumerate(["word/vbaProject.bin", "word/media/picture.svg"]):
            with self.subTest(member=member):
                path = self.archive("active-" + str(index) + ".docx", {
                    "word/document.xml": self.word_document(), member: b"fixture"})
                self.assert_rejected_without_mutation(path, "docx")
        xml = ('<?xml version="1.0" encoding="utf-16"?>'
               '<!DOCTYPE root SYSTEM "https://example.invalid/not-fetched"><root/>').encode("utf-16")
        path = self.archive("doctype.docx", {"word/document.xml": xml})
        self.assert_rejected_without_mutation(path, "docx")

    def test_normal_odt_and_embedded_reference_are_preserved(self):
        content = ('<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
                   'xmlns:xlink="http://www.w3.org/1999/xlink"><office:body>'
                   '<image xlink:href="Pictures/embedded.png"/></office:body></office:document-content>')
        path = self.archive("normal.odt", {"content.xml": content, "Pictures/embedded.png": b"fixture"})
        original = path.read_bytes()
        documents.prepare_windows_source(path, "odt")
        self.assertEqual(path.read_bytes(), original)

    def test_odt_external_and_parent_paths_are_rejected(self):
        for index, reference in enumerate(["https://example.invalid/image.png", "../outside.png", "file:///C:/outside.png"]):
            with self.subTest(reference=reference):
                path = self.archive("external-" + str(index) + ".odt", {
                    "content.xml": '<root xmlns:xlink="http://www.w3.org/1999/xlink"><image xlink:href="'
                    + reference + '"/></root>'})
                self.assert_rejected_without_mutation(path, "odt")

    def test_normal_rtf_is_preserved_and_external_fields_rejected(self):
        normal = self.file("normal.rtf", br"{\rtf1\ansi Texto normal}")
        original = normal.read_bytes()
        documents.prepare_windows_source(normal, "rtf")
        self.assertEqual(normal.read_bytes(), original)
        unsafe = self.file("field.rtf", br'{\rtf1{\field{\*\fldinst INCLUDEPICTURE "https://example.invalid/x"}}}')
        self.assert_rejected_without_mutation(unsafe, "rtf")

    def test_html_keeps_text_and_removes_executable_and_automatic_resources(self):
        source = ('<html><head><style>@import url(https://example.invalid/not-fetched);</style>'
                  '<meta http-equiv="refresh" content="0;url=https://example.invalid/"/></head>'
                  '<body><h1 onclick="run()">Maracujá</h1><script>doNotRun()</script>'
                  '<iframe src="https://example.invalid/"></iframe>'
                  '<p><a href="https://example.invalid/">Texto do link</a></p></body></html>')
        path = self.file("normal.html", source.encode("utf-8"))
        documents.prepare_windows_source(path, "html")
        converted = path.read_text(encoding="utf-8")
        self.assertIn("<h1>Maracujá</h1>", converted)
        self.assertIn("Texto do link", converted)
        for removed in ["https:", "onclick", "<script", "doNotRun", "<iframe", "<style", "refresh"]:
            self.assertNotIn(removed, converted)

    def test_html_remote_image_and_svg_disguised_as_png_are_rejected(self):
        remote = self.file("remote.html", b'<img src="https://example.invalid/not-fetched.png">')
        self.assert_rejected_without_mutation(remote, "html")
        svg = b'<svg xmlns="http://www.w3.org/2000/svg"><image href="https://example.invalid/x"/></svg>'
        disguised = self.file("disguised.html", b'<img src="data:image/png;base64,'
                              + base64.b64encode(svg) + b'">')
        self.assert_rejected_without_mutation(disguised, "html")

    def test_html_real_embedded_raster_is_reencoded_as_self_contained_png(self):
        from PIL import Image
        with io.BytesIO() as buffer:
            image = Image.new("RGB", (3, 2), "white")
            image.save(buffer, format="PNG")
            image.close()
            encoded = base64.b64encode(buffer.getvalue())
        path = self.file("embedded.html", b'<p>Texto</p><img src="data:image/png;base64,' + encoded + b'">')
        documents.prepare_windows_source(path, "html")
        converted = path.read_text(encoding="utf-8")
        self.assertIn("data:image/png;base64,", converted)
        self.assertIn("<p>Texto</p>", converted)

    def test_markdown_removes_link_destination_and_rejects_images(self):
        path = self.file("normal.md", "# Maracujá\n\n[Texto](https://example.invalid/)".encode("utf-8"))
        documents.prepare_windows_source(path, "md")
        converted = path.read_text(encoding="utf-8")
        self.assertIn("# Maracujá", converted)
        self.assertIn("Texto", converted)
        self.assertNotIn("https:", converted)
        unsafe = self.file("image.md", b'![image](https://example.invalid/not-fetched.png)')
        self.assert_rejected_without_mutation(unsafe, "md")

    def test_legacy_windows_text_is_normalized_to_utf8(self):
        path = self.file("legacy.txt", "Maracujá e conversão".encode("cp1252"))
        documents.prepare_windows_source(path, "txt")
        self.assertEqual(path.read_text(encoding="utf-8"), "Maracujá e conversão")


if __name__ == "__main__":
    unittest.main(verbosity=2)
