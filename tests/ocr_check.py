#!/usr/bin/python3
"""Sequential local OCR regression checks using generated, private fixtures.

Usage: python3 -I tests/ocr_check.py build/mandarina
Requires Pillow, reportlab, pypdf and python-docx in the selected test runtime. No
personal image, network request or existing desktop process is involved.
"""

import hashlib
import importlib.util
import io
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unicodedata
import zipfile

from docx import Document
from PIL import Image, ImageDraw, ImageFont
from pypdf import PdfReader, PdfWriter
from reportlab.lib.pagesizes import A4
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas


_runtime_spec = importlib.util.spec_from_file_location("maracuja_test_runtime", pathlib.Path(__file__).with_name("test_runtime.py"))
_runtime_module = importlib.util.module_from_spec(_runtime_spec)
_runtime_spec.loader.exec_module(_runtime_module)
APP = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                   _runtime_module.default_application(pathlib.Path(__file__).resolve().parents[1])).resolve()
RUNTIME = _runtime_module.TestRuntime(APP)
FIRST_MARKER = "MARACUJA 5729"
SECOND_MARKER = "SEGUNDA PAGINA 8614"
NATIVE_TOKEN = "NativeKeepCase_xQZ5729"
checks = []
current_check = "initialization"


def require(condition, reason):
    if not condition:
        raise AssertionError(reason)


def start(name):
    global current_check
    current_check = name


def passed():
    checks.append(current_check)


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def normalized(text):
    return re.sub(r"\s+", " ", unicodedata.normalize("NFKC", text)).strip()


def folded(text):
    return "".join(character for character in unicodedata.normalize("NFKD", normalized(text).casefold())
                   if not unicodedata.combining(character))


def run(arguments, timeout=150):
    return subprocess.run(list(map(str, arguments)), stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout, check=False, **RUNTIME.process_options())


def convert(source, fmt, destination, success=True):
    result = run([APP, "--convert", fmt, "--output", destination, "--", source])
    require(result.returncode == (0 if success else 1), "unexpected conversion exit status")
    try:
        report = json.loads(result.stdout)
    except (ValueError, UnicodeError) as error:
        raise AssertionError("conversion did not return a JSON report") from error
    outputs = report.get("outputs")
    errors = report.get("errors")
    require(isinstance(outputs, list) and isinstance(errors, list), "invalid conversion report")
    require(not report.get("cancelled"), "OCR was unexpectedly cancelled")
    if not success:
        require(not outputs and bool(errors), "failed OCR conversion published an output")
        return None, errors
    require(len(outputs) == 1 and not errors, "OCR conversion reported errors")
    output = pathlib.Path(outputs[0])
    require(output.is_file() and not output.is_symlink() and output.stat().st_size > 0,
            "OCR output is missing or empty")
    require(output.resolve().parent == destination.resolve(), "OCR output escaped destination")
    return output, errors


def read_output(path, fmt):
    if fmt == "txt":
        return path.read_text(encoding="utf-8-sig")
    if fmt == "docx":
        document = Document(path)
        require(len(document.inline_shapes) == 0, "DOCX embeds the scan instead of editable text")
        with zipfile.ZipFile(path) as archive:
            require(not any(name.startswith("word/media/") for name in archive.namelist()),
                    "DOCX contains scan pictures instead of text paragraphs")
        paragraphs = [paragraph.text for paragraph in document.paragraphs if paragraph.text.strip()]
        require(bool(paragraphs), "DOCX has no editable text paragraphs")
        return "\n".join(paragraphs)
    raise AssertionError("unknown OCR output validator")


def assert_first_page(text):
    plain = folded(text)
    require(FIRST_MARKER.casefold() in plain, "OCR lost the first page marker")
    require("portugues" in plain and "local conversion test" in plain,
            "OCR lost Portuguese or English fixture content")
    original = normalized(text).casefold()
    # OCR may alter punctuation, but must retain meaningful Portuguese accents.
    accented = sum(word in original for word in ("ação", "café", "açúcar", "são paulo"))
    require(accented >= 2, "Portuguese OCR lost the accented words")


def assert_second_page(text):
    require(SECOND_MARKER.casefold() in folded(text), "OCR lost the second page marker")
    require("banana 2486" in folded(text), "OCR lost the distinct second page content")


def assert_no_text_error(errors):
    message = " ".join(errors).casefold()
    require("texto" in message, "missing explanation that no text was recognized")
    require(not re.search(r"ocr.*(?:ainda|indispon|não.{0,15}dispon)", message),
            "OCR was unavailable instead of examining the empty image")


def fixture_font():
    return ImageFont.truetype(str(RUNTIME.fixture_font()), 74)


def page_image(second=False):
    image = Image.new("RGB", (2200, 1550), "white")
    draw = ImageDraw.Draw(image)
    font = fixture_font()
    lines = ([SECOND_MARKER, "Banana 2486", "Página digitalizada diferente.",
              "Texto da segunda página para converter."] if second else
             [FIRST_MARKER, "Português: ação, café e açúcar.",
              "São Paulo, conversão de documentos.", "Local conversion test."])
    for index, line in enumerate(lines):
        draw.text((130, 180 + index * 190), line, font=font, fill="black")
    return image


def image_pdf(path, images, native_first=False):
    document = canvas.Canvas(str(path), pagesize=A4)
    if native_first:
        document.setFont("Helvetica", 16)
        document.drawString(48, 745, "Native page exact token: " + NATIVE_TOKEN)
        document.drawString(48, 705, "Selectable native text must remain unchanged.")
        document.showPage()
    for image in images:
        width, height = image.size
        scale = min((A4[0] - 80) / width, (A4[1] - 100) / height)
        target_width, target_height = width * scale, height * scale
        document.drawImage(ImageReader(image), 40, A4[1] - 60 - target_height,
                           width=target_width, height=target_height)
        document.showPage()
    document.save()


def cropped_scan_pdf(path):
    # Both texts belong to the same raster image. The outside text remains in
    # the PDF's MediaBox/resources, so only respecting CropBox can exclude it.
    with page_image() as visible, Image.new("RGB", (2200, 3100), "white") as full:
        full.paste(visible, (0, 0))
        ImageDraw.Draw(full).text((130, 2500), "FORA RECORTE 9931", font=fixture_font(), fill="black")
        with io.BytesIO() as encoded:
            document = canvas.Canvas(encoded, pagesize=A4)
            document.drawImage(ImageReader(full), 0, 0, width=A4[0], height=A4[1])
            document.save()
            encoded.seek(0)
            reader = PdfReader(encoded)
            page = reader.pages[0]
            page.cropbox.lower_left = (0, A4[1] / 2)
            page.cropbox.upper_right = (A4[0], A4[1])
            writer = PdfWriter()
            writer.add_page(page)
            with path.open("wb") as destination:
                writer.write(destination)


def main():
    start("images_advertise_available_text_recognition")
    require(APP.is_file(), "converter binary missing")
    result = run([APP, "--list-formats"], timeout=30)
    require(result.returncode == 0, "format listing failed")
    capabilities = json.loads(result.stdout)
    require({"txt", "docx"} <= set(capabilities.get("images_ocr", [])),
            "image OCR TXT/DOCX capabilities unavailable")
    require({"txt", "docx"} <= set(capabilities.get("pdf", [])), "PDF text capabilities unavailable")
    passed()

    with tempfile.TemporaryDirectory(prefix="maracuja-ocr-check-") as temporary:
        root = pathlib.Path(temporary)
        inputs = root / "entradas ação"
        output = root / "saidas"
        inputs.mkdir(mode=0o700)
        output.mkdir(mode=0o700)
        originals = {}

        def track(path):
            originals[path] = digest(path)
            return path

        with page_image() as first, page_image(second=True) as second:
            png = inputs / "foto português.png"
            jpg = inputs / "foto português.jpg"
            first.save(png, dpi=(300, 300))
            first.save(jpg, quality=96, dpi=(300, 300), subsampling=0)
            track(png)
            track(jpg)
            scanned_pdf = inputs / "digitalizado duas páginas.pdf"
            image_pdf(scanned_pdf, [first, second])
            track(scanned_pdf)
            mixed_pdf = inputs / "misto texto e digitalização.pdf"
            image_pdf(mixed_pdf, [second], native_first=True)
            track(mixed_pdf)
            # Actual EXIF orientation, rather than pre-rotating an already
            # upright bitmap: readers must apply the metadata to recover it.
            rotated_jpg = inputs / "foto orientação EXIF.jpg"
            with first.transpose(Image.Transpose.ROTATE_90) as sideways:
                orientation = Image.Exif()
                orientation[274] = 6
                sideways.save(rotated_jpg, quality=96, dpi=(300, 300), subsampling=0,
                              exif=orientation)
            track(rotated_jpg)

        first_txt = None
        for source in [png, jpg]:
            for fmt in ["txt", "docx"]:
                start(source.suffix[1:] + "_ocr_to_editable_" + fmt + "_with_portuguese_accents")
                product, _ = convert(source, fmt, output)
                assert_first_page(read_output(product, fmt))
                if source == png and fmt == "txt":
                    first_txt = product
                passed()

        for fmt in ["txt", "docx"]:
            start("exif_orientation_is_applied_before_ocr_" + fmt)
            product, _ = convert(rotated_jpg, fmt, output)
            assert_first_page(read_output(product, fmt))
            passed()

        for fmt in ["txt", "docx"]:
            start("scanned_pdf_preserves_every_page_in_" + fmt)
            product, _ = convert(scanned_pdf, fmt, output)
            text = read_output(product, fmt)
            assert_first_page(text)
            assert_second_page(text)
            require(folded(text).index(FIRST_MARKER.casefold()) <
                    folded(text).index(SECOND_MARKER.casefold()), "OCR reordered PDF pages")
            passed()

        for fmt in ["txt", "docx"]:
            start("mixed_pdf_preserves_native_text_and_recognizes_scan_" + fmt)
            product, _ = convert(mixed_pdf, fmt, output)
            text = read_output(product, fmt)
            require(NATIVE_TOKEN in text, "native PDF text was replaced or altered by OCR")
            assert_second_page(text)
            require(folded(text).index(NATIVE_TOKEN.casefold()) <
                    folded(text).index(SECOND_MARKER.casefold()), "mixed PDF page order changed")
            passed()

        cropped_pdf = inputs / "digitalizado com recorte.pdf"
        cropped_scan_pdf(cropped_pdf)
        track(cropped_pdf)
        for fmt in ["txt", "docx"]:
            start("pdf_cropbox_limits_recognition_to_visible_content_" + fmt)
            product, _ = convert(cropped_pdf, fmt, output)
            text = read_output(product, fmt)
            assert_first_page(text)
            require(not re.search(r"\b(?:fora|recorte|9931)\b", folded(text)),
                    "OCR included text outside the PDF's visible CropBox")
            passed()

        blank_png = inputs / "imagem branca.png"
        blank_pdf = inputs / "digitalizado branco.pdf"
        with Image.new("RGB", (1200, 1700), "white") as blank:
            blank.save(blank_png, dpi=(300, 300))
            image_pdf(blank_pdf, [blank])
        track(blank_png)
        track(blank_pdf)
        for source in [blank_png, blank_pdf]:
            for fmt in ["txt", "docx"]:
                start("empty_" + source.suffix[1:] + "_does_not_publish_empty_" + fmt)
                _, errors = convert(source, fmt, output, success=False)
                assert_no_text_error(errors)
                passed()

        start("pdf_page_limit_is_enforced_before_ocr")
        too_many = inputs / "limite 201 páginas.pdf"
        document = canvas.Canvas(str(too_many), pagesize=(100, 100))
        for _ in range(201):
            document.showPage()
        document.save()
        track(too_many)
        _, errors = convert(too_many, "txt", output, success=False)
        require("200" in " ".join(errors), "page-count preflight did not explain the limit")
        passed()

        for suffix, payload in [("png", b"not an image\x00"), ("pdf", b"%PDF-1.7\ninvalid\x00")]:
            start("invalid_" + suffix + "_is_rejected_without_output")
            invalid = inputs / ("invalido." + suffix)
            invalid.write_bytes(payload)
            track(invalid)
            convert(invalid, "docx", output, success=False)
            passed()

        start("ocr_collision_preserves_the_existing_output")
        require(first_txt is not None, "initial OCR fixture output missing")
        before = digest(first_txt)
        again, _ = convert(png, "txt", output)
        require(again != first_txt and digest(first_txt) == before,
                "OCR overwrote an existing output")
        assert_first_page(read_output(again, "txt"))
        passed()

        start("ocr_never_modifies_the_original_files")
        require(all(path.is_file() and digest(path) == checksum for path, checksum in originals.items()),
                "OCR changed or removed an original")
        passed()
        start("ocr_successes_and_failures_clean_temporary_files")
        require(not list(root.rglob(".mandarina-*")) and not list(root.rglob(".maracuja-*")),
                "OCR left temporary files behind")
        passed()

    print(json.dumps({"passed": len(checks), "checks": checks}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        reason = str(error) if isinstance(error, AssertionError) else type(error).__name__
        print(json.dumps({"failed": current_check, "reason": reason, "passed": len(checks)}))
        sys.exit(1)
