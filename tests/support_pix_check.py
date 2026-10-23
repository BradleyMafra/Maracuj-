#!/usr/bin/env python3
"""Check static Pix assets; optional independent decoding is a release gate.

Run with --qrcode-wheel to check reproducible generation without installation.
Use --zbar-library /path/to/libzbar.so.0 --require-decode for an independent
decoder already available locally. OpenCV can also be used if installed.
Neither this test nor the asset generator downloads or installs dependencies.
"""

import argparse
import binascii
import ctypes
import ctypes.util
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("support_pix_generator", ROOT / "packaging/generate_support_pix.py")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)
OPTIONS = argparse.Namespace(qrcode_wheel=None, zbar_library=None, require_decode=False)
EXAMPLE_KEY = "123e4567-e12b-12d1-a456-426655440000"
# Public Banco Central vector, Manual de Padrões para Iniciação do Pix 2.6.3.
BCB_VECTOR = ("00020126580014br.gov.bcb.pix0136123e4567-e12b-12d1-a456-426655440000"
              "5204000053039865802BR5913Fulano de Tal6008BRASILIA62070503***63041D3D")


def fields(value):
    """Independent byte-oriented TLV reader for the emitted BR Code."""
    data = value.encode("ascii") if isinstance(value, str) else value
    result = {}
    while data:
        if len(data) < 4 or not re.fullmatch(rb"[0-9]{4}", data[:4]):
            raise ValueError("Bad TLV header")
        key, count = data[:2].decode("ascii"), int(data[2:4])
        if key in result or len(data) < 4 + count:
            raise ValueError("Bad TLV length or duplicate")
        result[key] = data[4:4 + count].decode("ascii")
        data = data[4 + count:]
    return result


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Bad PNG signature")
    data = data[8:]
    chunks = []
    while data:
        if len(data) < 12:
            raise ValueError("Truncated PNG")
        count = struct.unpack(">I", data[:4])[0]
        kind, value = data[4:8], data[8:8 + count]
        if len(data) < count + 12:
            raise ValueError("Truncated PNG chunk")
        if binascii.crc32(kind + value) & 0xffffffff != struct.unpack(">I", data[8 + count:12 + count])[0]:
            raise ValueError("Bad PNG chunk CRC")
        chunks.append((kind, value))
        data = data[12 + count:]
    if [kind for kind, _ in chunks] != [b"IHDR", b"IDAT", b"IEND"]:
        raise ValueError("Unexpected PNG metadata or chunks")
    width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunks[0][1])
    if not (0 < width == height <= 2048) or (depth, color, compression, filtering, interlace) != (8, 0, 0, 0, 0):
        raise ValueError("Unexpected PNG encoding")
    raw = zlib.decompress(chunks[1][1])
    if len(raw) != (width + 1) * height:
        raise ValueError("Bad PNG image size")
    rows = []
    for index in range(height):
        row = raw[index * (width + 1):(index + 1) * (width + 1)]
        if row[0] != 0 or any(pixel not in (0, 255) for pixel in row[1:]):
            raise ValueError("QR must be unfiltered black/white grayscale")
        rows.append(row[1:])
    return width, b"".join(rows), rows


def read_svg_matrix(path):
    root = ET.fromstring(path.read_bytes())
    namespace = "{http://www.w3.org/2000/svg}"
    if root.tag != namespace + "svg" or root.attrib.get("shape-rendering") != "crispEdges":
        raise ValueError("Unexpected SVG root")
    view = root.attrib["viewBox"].split()
    if len(view) != 4 or view[:2] != ["0", "0"] or view[2] != view[3]:
        raise ValueError("Unexpected QR view box")
    size = int(view[2])
    if not 29 <= size <= 185:
        raise ValueError("Unexpected QR module count")
    if [child.tag for child in root] != [namespace + "rect", namespace + "path"]:
        raise ValueError("Unexpected SVG data, links or metadata")
    background, modules = root
    if background.attrib != {"width": str(size), "height": str(size), "fill": "#fff"} or modules.attrib["fill"] != "#000":
        raise ValueError("QR must have a white background and black modules")
    matrix = [[False] * size for _ in range(size)]
    commands = modules.attrib["d"]
    position = 0
    for match in re.finditer(r"M([0-9]+),([0-9]+)h([0-9]+)v1h-([0-9]+)z", commands):
        if match.start() != position:
            raise ValueError("Unexpected SVG path command")
        position = match.end()
        x, y, count, back = map(int, match.groups())
        if not (0 <= x < size and 0 <= y < size and 0 < count == back and x + count <= size):
            raise ValueError("SVG module outside view box")
        for column in range(x, x + count):
            if matrix[y][column]:
                raise ValueError("Overlapping SVG modules")
            matrix[y][column] = True
    if position != len(commands):
        raise ValueError("Unparsed SVG path")
    return matrix


def decode_zbar(width, pixels, library):
    zbar = ctypes.CDLL(str(library))
    pointer = ctypes.c_void_p
    signatures = {
        "zbar_image_scanner_create": ([], pointer),
        "zbar_image_scanner_destroy": ([pointer], None),
        "zbar_image_scanner_set_config": ([pointer, ctypes.c_int, ctypes.c_int, ctypes.c_int], ctypes.c_int),
        "zbar_image_create": ([], pointer),
        "zbar_image_destroy": ([pointer], None),
        "zbar_image_set_format": ([pointer, ctypes.c_ulong], None),
        "zbar_image_set_size": ([pointer, ctypes.c_uint, ctypes.c_uint], None),
        "zbar_image_set_data": ([pointer, pointer, ctypes.c_ulong, pointer], None),
        "zbar_scan_image": ([pointer, pointer], ctypes.c_int),
        "zbar_image_first_symbol": ([pointer], pointer),
        "zbar_symbol_next": ([pointer], pointer),
        "zbar_symbol_get_type": ([pointer], ctypes.c_int),
        "zbar_symbol_get_data": ([pointer], pointer),
        "zbar_symbol_get_data_length": ([pointer], ctypes.c_uint),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(zbar, name)
        function.argtypes, function.restype = arguments, result
    scanner, image = zbar.zbar_image_scanner_create(), zbar.zbar_image_create()
    if not scanner or not image:
        if scanner:
            zbar.zbar_image_scanner_destroy(scanner)
        if image:
            zbar.zbar_image_destroy(image)
        raise ValueError("Could not initialize local QR decoder")
    buffer = ctypes.create_string_buffer(pixels)
    try:
        zbar.zbar_image_scanner_set_config(scanner, 0, 0, 0)
        zbar.zbar_image_scanner_set_config(scanner, 64, 0, 1)
        zbar.zbar_image_set_format(image, int.from_bytes(b"Y800", "little"))
        zbar.zbar_image_set_size(image, width, width)
        zbar.zbar_image_set_data(image, ctypes.cast(buffer, pointer), len(pixels), None)
        if zbar.zbar_scan_image(scanner, image) <= 0:
            raise ValueError("Independent decoder found no QR code")
        decoded = []
        symbol = zbar.zbar_image_first_symbol(image)
        while symbol:
            if zbar.zbar_symbol_get_type(symbol) == 64:
                decoded.append(ctypes.string_at(zbar.zbar_symbol_get_data(symbol),
                                                zbar.zbar_symbol_get_data_length(symbol)).decode("ascii"))
            symbol = zbar.zbar_symbol_next(symbol)
        return decoded
    finally:
        zbar.zbar_image_destroy(image)
        zbar.zbar_image_scanner_destroy(scanner)


class SupportPixChecks(unittest.TestCase):
    def test_crc_uses_official_external_vectors(self):
        self.assertEqual(binascii.crc_hqx(b"123456789", 0xffff), 0x29b1)
        self.assertEqual(generator.crc16("123456789"), "29B1")
        self.assertEqual(binascii.crc_hqx(BCB_VECTOR[:-4].encode("ascii"), 0xffff), 0x1d3d)
        self.assertEqual(generator.crc16(BCB_VECTOR[:-4]), "1D3D")

    def test_committed_payload_is_static_amount_free_and_matches_key(self):
        data = json.loads((ROOT / "assets/support-pix.json").read_text("ascii"))
        self.assertEqual(set(data), {"pixKey", "pixPayload"})
        key, payload = data["pixKey"], data["pixPayload"]
        self.assertRegex(key, r"\A[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}\Z")
        parsed = fields(payload)
        self.assertEqual(set(parsed), {"00", "01", "26", "52", "53", "58", "59", "60", "62", "63"})
        self.assertEqual(parsed["00"], "01")
        self.assertEqual(parsed["01"], "11")
        self.assertEqual(fields(parsed["26"]), {"00": "br.gov.bcb.pix", "01": key})
        self.assertEqual((parsed["52"], parsed["53"], parsed["58"]), ("0000", "986", "BR"))
        self.assertEqual((parsed["59"], parsed["60"]), ("MARACUJA", "BLUMENAU"))
        self.assertEqual(fields(parsed["62"]), {"05": "***"})
        self.assertEqual(payload[-8:-4], "6304")
        self.assertEqual(parsed["63"], f'{binascii.crc_hqx(payload[:-4].encode("ascii"), 0xffff):04X}')
        self.assertEqual(generator.build_payload(key, "MARACUJA", "BLUMENAU"), payload)

    def test_input_bounds_and_non_ascii_are_rejected(self):
        for key in (None, "", "+5561999999999", "user@example.invalid", "{" + EXAMPLE_KEY + "}", EXAMPLE_KEY + "\n"):
            with self.subTest(key_type=type(key).__name__), self.assertRaises(ValueError):
                generator.build_payload(key, "MARACUJA", "BLUMENAU")
        for name in (None, "", "   ", "A" * 26, "Árvore", "A\nB", "A\x00B"):
            with self.subTest(name_type=type(name).__name__), self.assertRaises(ValueError):
                generator.build_payload(EXAMPLE_KEY, name, "BLUMENAU")
        for city in (None, "", "B" * 16, "São Paulo", "A\tB"):
            with self.subTest(city_type=type(city).__name__), self.assertRaises(ValueError):
                generator.build_payload(EXAMPLE_KEY, "MARACUJA", city)
        fields(generator.build_payload(EXAMPLE_KEY, "A" * 25, "B" * 15))

    def test_reusing_configuration_requires_exact_static_format(self):
        key, name, city = generator.from_config(ROOT / "assets/support-pix.json")
        self.assertEqual((name, city), ("MARACUJA", "BLUMENAU"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "support.json"
            payload = generator.build_payload(EXAMPLE_KEY, name, city)
            for data in ([], {}, {"pixKey": key, "pixPayload": payload},
                         {"pixKey": EXAMPLE_KEY, "pixPayload": payload[:-4] + "0000"}):
                path.write_text(json.dumps(data), encoding="ascii")
                with self.assertRaises(ValueError):
                    generator.from_config(path)

    def test_svg_png_have_identical_modules_and_four_white_border_modules(self):
        matrix = read_svg_matrix(ROOT / "assets/support-pix-qr.svg")
        width, _, rows = read_png(ROOT / "assets/support-pix-qr.png")
        self.assertEqual(width, len(matrix) * 8)
        for y, row in enumerate(matrix):
            expected = b"".join((b"\x00" if value else b"\xff") * 8 for value in row)
            for pixel_row in rows[y * 8:(y + 1) * 8]:
                self.assertEqual(pixel_row, expected)
            if y < 4 or y >= len(matrix) - 4:
                self.assertFalse(any(row))
            self.assertFalse(any(row[:4]) or any(row[-4:]))
        self.assertTrue(any(any(row) for row in matrix[4:-4]))

    def test_independent_decoder_matches_pix_text(self):
        width, pixels, _ = read_png(ROOT / "assets/support-pix-qr.png")
        payload = json.loads((ROOT / "assets/support-pix.json").read_text("ascii"))["pixPayload"]
        library = OPTIONS.zbar_library or ctypes.util.find_library("zbar")
        if library:
            self.assertEqual(decode_zbar(width, pixels, library), [payload])
        elif importlib.util.find_spec("cv2"):
            import cv2
            image = cv2.imread(str(ROOT / "assets/support-pix-qr.png"), cv2.IMREAD_GRAYSCALE)
            decoded, _, _ = cv2.QRCodeDetector().detectAndDecode(image)
            self.assertEqual(decoded, payload)
        elif OPTIONS.require_decode:
            self.fail("No independent decoder available; supply a local zbar library or OpenCV")
        else:
            self.skipTest("Independent QR decoder is optional; release validation must use --require-decode")

    def test_regeneration_matches_committed_assets(self):
        if OPTIONS.qrcode_wheel is None and importlib.util.find_spec("qrcode") is None:
            self.skipTest("Pass --qrcode-wheel to validate reproducible offline generation")
        key, name, city = generator.from_config(ROOT / "assets/support-pix.json")
        outputs, _ = generator.generate(key, name, city, generator.load_qrcode(OPTIONS.qrcode_wheel))
        self.assertEqual(set(outputs), set(generator.ASSET_NAMES))
        for filename, content in outputs.items():
            self.assertEqual(content, (ROOT / "assets" / filename).read_bytes(), filename)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--qrcode-wheel", type=Path)
    parser.add_argument("--zbar-library", type=Path)
    parser.add_argument("--require-decode", action="store_true")
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
