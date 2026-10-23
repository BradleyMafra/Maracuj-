#!/usr/bin/python3
"""Exercise real conversions using only disposable, generated fixtures."""
import hashlib
import importlib.util
import json
import pathlib
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

APP = pathlib.Path(sys.argv[1]).resolve()
_runtime_spec = importlib.util.spec_from_file_location("maracuja_test_runtime", pathlib.Path(__file__).with_name("test_runtime.py"))
_runtime_module = importlib.util.module_from_spec(_runtime_spec)
_runtime_spec.loader.exec_module(_runtime_module)
RUNTIME = _runtime_module.TestRuntime(APP)
checks = []


def run(fmt, sources, destination=None, expected=0):
    command = [str(APP), "--convert", fmt]
    if destination is not None:
        command += ["--output", str(destination)]
    command += ["--", *map(str, sources)]
    result = subprocess.run(command, capture_output=True, timeout=30,
                            **RUNTIME.process_options())
    assert result.returncode == expected, (command, result.stdout, result.stderr)
    return json.loads(result.stdout)


def pdf_pages(path):
    result = subprocess.run([str(RUNTIME.tool("pdfinfo")), str(path)], capture_output=True,
                            check=True, **RUNTIME.process_options())
    return int(re.search(rb"^Pages:\s*(\d+)", result.stdout, re.M)[1])


def text_pdf(path, pages=1):
    children = " ".join(f"{4 + index * 2} 0 R" for index in range(pages)).encode()
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [" + children + b"] /Count " + str(pages).encode() + b" >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    for index in range(pages):
        stream = ("BT /F1 18 Tf 30 100 Td (Mandarina - teste local - " + str(index + 1) + ") Tj ET").encode()
        objects += [
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 160] /Resources << /Font << /F1 3 0 R >> >> /Contents "
            + str(5 + index * 2).encode() + b" 0 R >>",
            b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream",
        ]
    data = b"%PDF-1.4\n"; offsets = [0]
    for number, obj in enumerate(objects, 1):
        offsets.append(len(data)); data += f"{number} 0 obj\n".encode() + obj + b"\nendobj\n"
    start = len(data)
    data += f"xref\n0 {len(objects) + 1}\n".encode() + b"0000000000 65535 f \n"
    data += b"".join(f"{offset:010} 00000 n \n".encode() for offset in offsets[1:])
    data += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n".encode()
    path.write_bytes(data)


def rendered_pages(result, source, destination, fmt, count, original_names=True):
    assert not result["errors"] and not result.get("cancelled")
    pages = list(map(pathlib.Path, result["outputs"]))
    assert len(pages) == count and len(set(pages)) == count
    for index, page in enumerate(pages, 1):
        assert page.is_file() and not page.is_symlink() and page.stat().st_size > 0
        assert page.resolve().parent == destination.resolve(), "page image was not saved directly in destination"
        assert page.suffix == "." + fmt
        if original_names:
            suffix = "" if count == 1 else "-pagina-" + str(index).zfill(len(str(count)))
            assert page.name == source.stem + "-convertido" + suffix + "." + fmt
        with Image.open(page) as image:
            assert image.format == ("PNG" if fmt == "png" else "JPEG")
            assert 300 < max(image.size) <= 4096
    return pages


with tempfile.TemporaryDirectory(prefix="mandarina-check-") as temporary:
    root = pathlib.Path(temporary); a = root / "pasta a"; b = root / "pasta b"; output = root / "saidas"
    a.mkdir(); b.mkdir(); output.mkdir()
    photo = a / "foto ç com espaços.png"
    image = Image.new("RGBA", (180, 120), (0, 0, 0, 0))
    ImageDraw.Draw(image).rectangle((50, 30, 140, 90), fill=(240, 160, 80, 255))
    image.save(photo)
    other = b / "outra.png"; Image.new("RGB", (80, 140), "#a6cce9").save(other)
    original_hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in [photo, other]}

    formats = json.loads(subprocess.run([str(APP), "--list-formats"], capture_output=True,
                                        check=True,
                                        **RUNTIME.process_options()).stdout)["images"]
    for fmt in formats:
        result = run(fmt, [photo], output)
        assert len(result["outputs"]) == 1 and pathlib.Path(result["outputs"][0]).stat().st_size > 0
        if fmt == "jpg":
            with Image.open(result["outputs"][0]) as jpg:
                assert min(jpg.getpixel((0, 0))) >= 250, "JPEG transparency must flatten on white"
        checks.append("image_to_" + fmt)

    sentinel = output / "foto ç com espaços-convertido.jpg"
    sentinel_hash = hashlib.sha256(sentinel.read_bytes()).hexdigest()
    result = run("jpg", [photo], output)
    assert pathlib.Path(result["outputs"][0]).resolve() != sentinel.resolve()
    assert hashlib.sha256(sentinel.read_bytes()).hexdigest() == sentinel_hash
    checks.append("collision_never_overwrites")

    result = run("webp", [photo, other])
    assert {pathlib.Path(p).parent for p in result["outputs"]} == {a, b}
    checks.append("batch_beside_each_original")

    result = run("pdf", [photo, other], output)
    pictures_pdf = pathlib.Path(result["outputs"][0]); assert pdf_pages(pictures_pdf) == 2
    checks.append("two_photos_two_pdf_pages")

    for fmt in ["png", "jpg"]:
        result = run(fmt, [pictures_pdf], output)
        pages = rendered_pages(result, pictures_pdf, output, fmt, 2)
        checks.append("pdf_all_pages_to_" + fmt)
        previous = {page: hashlib.sha256(page.read_bytes()).hexdigest() for page in pages}
        repeated = rendered_pages(run(fmt, [pictures_pdf], output), pictures_pdf, output, fmt, 2,
                                  original_names=False)
        assert set(repeated).isdisjoint(pages)
        assert all(hashlib.sha256(page.read_bytes()).hexdigest() == digest for page, digest in previous.items())
        checks.append("pdf_page_" + fmt + "_collision_never_overwrites")

    known_pdf = a / "texto.pdf"; text_pdf(known_pdf)
    default_pdf = b / "duas páginas.pdf"; default_pdf.write_bytes(pictures_pdf.read_bytes())
    numbered_pdf = a / "dez páginas.pdf"; text_pdf(numbered_pdf, pages=10)
    original_hashes.update({path: hashlib.sha256(path.read_bytes()).hexdigest()
                            for path in [known_pdf, pictures_pdf, default_pdf, numbered_pdf]})
    for fmt in ["png", "jpg"]:
        single = rendered_pages(run(fmt, [known_pdf], output), known_pdf, output, fmt, 1)
        checks.append("single_pdf_page_direct_" + fmt)
        previous = hashlib.sha256(single[0].read_bytes()).hexdigest()
        repeated = rendered_pages(run(fmt, [known_pdf], output), known_pdf, output, fmt, 1,
                                  original_names=False)
        assert repeated[0] != single[0] and hashlib.sha256(single[0].read_bytes()).hexdigest() == previous
        checks.append("single_pdf_" + fmt + "_collision_never_overwrites")
        result = run(fmt, [known_pdf, default_pdf])
        assert not result["errors"] and len(result["outputs"]) == 3
        for source, count in [(known_pdf, 1), (default_pdf, 2)]:
            selected = [value for value in result["outputs"] if pathlib.Path(value).parent == source.parent]
            rendered_pages({**result, "outputs": selected}, source, source.parent, fmt, count)
        checks.append("pdf_" + fmt + "_without_output_beside_each_original")
    rendered_pages(run("png", [numbered_pdf], output), numbered_pdf, output, "png", 10)
    checks.append("pdf_page_numbers_are_zero_padded_and_ordered")
    rollback_output = root / "colisao completa"
    rollback_output.mkdir()
    reserved = b"existing user file"
    for sequence in range(1, 10001):
        suffix = "" if sequence == 1 else f" ({sequence})"
        (rollback_output / (default_pdf.stem + "-convertido-pagina-2" + suffix + ".png")).write_bytes(reserved)
    result = run("png", [default_pdf], rollback_output, expected=1)
    assert not result["outputs"] and result["errors"]
    assert not (rollback_output / (default_pdf.stem + "-convertido-pagina-1.png")).exists()
    remaining = list(rollback_output.iterdir())
    assert len(remaining) == 10000 and all(path.is_file() and path.read_bytes() == reserved for path in remaining)
    checks.append("pdf_page_publication_failure_rolls_back_only_new_images")
    result = run("txt", [known_pdf], output)
    assert "Mandarina - teste local" in pathlib.Path(result["outputs"][0]).read_text(encoding="utf-8-sig")
    checks.append("pdf_extract_existing_text")
    blank_pdf = a / "digitalizado sem letras.pdf"
    with Image.new("RGB", (900, 600), "white") as blank:
        blank.save(blank_pdf, "PDF", resolution=150)
    result = run("txt", [blank_pdf], output, expected=1)
    error_text = " ".join(result["errors"]).lower()
    assert not result["outputs"] and "texto" in error_text
    assert not re.search(r"ocr.*(?:ainda|indispon|não.{0,15}dispon)", error_text)
    checks.append("blank_scanned_pdf_does_not_publish_empty_text")

    result = run("pdf", [known_pdf, pictures_pdf], output)
    assert pdf_pages(result["outputs"][0]) == 3
    checks.append("pdf_merge")
    bad = a / "quebrado.png"; bad.write_bytes(b"not an image")
    result = run("png", [photo, bad], output, expected=1)
    assert len(result["outputs"]) == 1 and result["errors"]
    checks.append("partial_failure_reports_error")
    result = run("not-a-format", [photo], output, expected=1)
    assert not result["outputs"] and result["errors"]
    checks.append("unsupported_output_rejected")

    uri_result = run("png", [photo.as_uri()], output)
    assert uri_result["outputs"]
    checks.append("file_uri_unicode_and_spaces")
    assert all(hashlib.sha256(p.read_bytes()).hexdigest() == digest for p, digest in original_hashes.items())
    assert not list(root.rglob(".mandarina-*")) and not list(root.rglob(".maracuja-*"))
    assert {path.resolve() for path in root.rglob("*") if path.is_dir()} == {
        a.resolve(), b.resolve(), output.resolve(), rollback_output.resolve()}
    checks += ["originals_unchanged", "temporary_files_cleaned", "no_rendered_page_subdirectories"]

print(json.dumps({"passed": len(checks), "checks": checks}, indent=2))
