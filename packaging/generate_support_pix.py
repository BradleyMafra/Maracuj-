#!/usr/bin/env python3
"""Generate the optional support assets offline, without a payment service.

Build-time dependency: qrcode 8.2 (BSD), never bundled with the application.
Pass its verified wheel with --qrcode-wheel; this script downloads nothing and
installs nothing. The public PyPI wheel SHA-256 is pinned below.

Regenerate from the already approved JSON without placing a key in a command:
  python3 packaging/generate_support_pix.py --from-config assets/support-pix.json \
      --qrcode-wheel /path/to/qrcode-8.2-py3-none-any.whl --output-dir assets

For initial setup, replace --from-config with --key UUID --name NAME --city CITY.
The resulting static Pix has no fixed amount or expiration. A bank must still
confirm the recipient; an offline generator cannot verify account ownership.
"""

import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import re
import struct
import sys
import zlib

QRCODE_VERSION = "8.2"
QRCODE_WHEEL_SHA256 = "16e64e0716c14960108e85d853062c9e8bba5ca8252c0b4d0231b9df4060ff4f"
QUIET_ZONE = 4
PIXEL_SCALE = 8
ASSET_NAMES = ("support-pix.json", "support-pix-qr.svg", "support-pix-qr.png")
RANDOM_KEY = re.compile(r"\A[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\Z")


def ascii_field(value, name, maximum):
    if not isinstance(value, str) or not value.strip() or len(value) > maximum:
        raise ValueError(f"{name} must contain 1 to {maximum} printable ASCII characters")
    if any(not 0x20 <= ord(character) <= 0x7e for character in value):
        raise ValueError(f"{name} must use printable ASCII without accents or control characters")
    return value


def tlv(identifier, value):
    encoded = value.encode("ascii")
    if not re.fullmatch(r"[0-9]{2}", identifier) or len(encoded) > 99:
        raise ValueError("BR Code field exceeds the two-digit TLV bounds")
    return identifier + f"{len(encoded):02d}" + value


def crc16(value):
    crc = 0xffff
    for byte in value.encode("ascii"):
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return f"{crc:04X}"


def build_payload(key, name, city):
    if not isinstance(key, str) or RANDOM_KEY.fullmatch(key) is None:
        raise ValueError("Pix support requires a random UUID key")
    key = key.lower()
    name = ascii_field(name, "Merchant name", 25)
    city = ascii_field(city, "Merchant city", 15)
    account = tlv("00", "br.gov.bcb.pix") + tlv("01", key)
    body = (tlv("00", "01") + tlv("01", "11") + tlv("26", account) + tlv("52", "0000")
            + tlv("53", "986") + tlv("58", "BR") + tlv("59", name) + tlv("60", city)
            + tlv("62", tlv("05", "***")) + "6304")
    return body + crc16(body)


def read_fields(payload):
    """Read the bounded ASCII TLV fields used by our own checked-in assets."""
    if not isinstance(payload, str) or not payload.isascii() or len(payload) > 512:
        raise ValueError("Invalid support BR Code")
    fields = {}
    position = 0
    while position < len(payload):
        header = payload[position:position + 4]
        if len(header) != 4 or not header.isdigit():
            raise ValueError("Invalid BR Code TLV header")
        identifier, count = header[:2], int(header[2:])
        position += 4
        if identifier in fields or position + count > len(payload):
            raise ValueError("Invalid or duplicate BR Code TLV field")
        fields[identifier] = payload[position:position + count]
        position += count
    return fields


def from_config(path):
    configuration = Path(path).read_bytes()
    if len(configuration) > 16 * 1024:
        raise ValueError("Support configuration is too large")
    data = json.loads(configuration)
    if not isinstance(data, dict):
        raise ValueError("Support configuration must be an object")
    key, payload = data.get("pixKey"), data.get("pixPayload")
    fields = read_fields(payload)
    name, city = fields.get("59"), fields.get("60")
    regenerated = build_payload(key, name, city)
    if payload != regenerated:
        raise ValueError("Support configuration is not our static amount-free Pix format")
    return key.lower(), name, city


def load_qrcode(wheel=None):
    if wheel is not None:
        path = Path(wheel).resolve(strict=True)
        if path.stat().st_size > 1024 * 1024 or hashlib.sha256(path.read_bytes()).hexdigest() != QRCODE_WHEEL_SHA256:
            raise ValueError("qrcode wheel SHA-256 does not match the pinned 8.2 release")
        if "qrcode" in sys.modules and not str(sys.modules["qrcode"].__file__).startswith(str(path)):
            raise ValueError("A different qrcode module is already loaded")
        sys.path.insert(0, str(path))
    try:
        version = importlib.metadata.version("qrcode")
        if version != QRCODE_VERSION:
            raise ValueError("Use qrcode 8.2 for reproducible support assets")
        import qrcode
    except importlib.metadata.PackageNotFoundError as error:
        raise ValueError("Pass the verified qrcode 8.2 wheel with --qrcode-wheel") from error
    return qrcode


def qr_matrix(payload, qrcode):
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, border=QUIET_ZONE)
    qr.add_data(payload.encode("ascii"), optimize=0)
    qr.make(fit=True)
    return qr.get_matrix()


def svg_bytes(matrix):
    size = len(matrix)
    runs = []
    for y, row in enumerate(matrix):
        x = 0
        while x < size:
            if not row[x]:
                x += 1
                continue
            start = x
            while x < size and row[x]:
                x += 1
            length = x - start
            runs.append(f"M{start},{y}h{length}v1h-{length}z")
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size * PIXEL_SCALE}" '
            f'height="{size * PIXEL_SCALE}" viewBox="0 0 {size} {size}" shape-rendering="crispEdges">\n'
            f'  <rect width="{size}" height="{size}" fill="#fff"/>\n'
            f'  <path d="{"".join(runs)}" fill="#000"/>\n'
            '</svg>\n').encode("ascii")


def png_bytes(matrix):
    size = len(matrix) * PIXEL_SCALE

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    rows = bytearray()
    for row in matrix:
        pixels = b"".join((b"\x00" if black else b"\xff") * PIXEL_SCALE for black in row)
        rows.extend((b"\x00" + pixels) * PIXEL_SCALE)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), level=9)) + chunk(b"IEND", b""))


def generate(key, name, city, qrcode):
    payload = build_payload(key, name, city)
    matrix = qr_matrix(payload, qrcode)
    configuration = json.dumps({"pixKey": key.lower(), "pixPayload": payload}, indent=2, ensure_ascii=True) + "\n"
    return {"support-pix.json": configuration.encode("ascii"),
            "support-pix-qr.svg": svg_bytes(matrix), "support-pix-qr.png": png_bytes(matrix)}, len(matrix)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--qrcode-wheel", type=Path, help="Verified qrcode 8.2 wheel; loaded without installation")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parents[1] / "assets")
    parser.add_argument("--from-config", type=Path, help="Regenerate from the approved support-pix.json")
    parser.add_argument("--key", help="Public random Pix UUID key")
    parser.add_argument("--name", help="ASCII merchant name, at most 25 characters")
    parser.add_argument("--city", help="ASCII merchant city, at most 15 characters")
    arguments = parser.parse_args()
    try:
        if arguments.from_config:
            if any(value is not None for value in (arguments.key, arguments.name, arguments.city)):
                raise ValueError("Use --from-config or --key/--name/--city, not both")
            key, name, city = from_config(arguments.from_config)
        else:
            key, name, city = arguments.key, arguments.name, arguments.city
        outputs, modules = generate(key, name, city, load_qrcode(arguments.qrcode_wheel))
        arguments.output_dir.mkdir(parents=True, exist_ok=True)
        for filename, content in outputs.items():
            (arguments.output_dir / filename).write_bytes(content)
        print(json.dumps({"qrcode_version": QRCODE_VERSION, "qrcode_wheel_sha256": QRCODE_WHEEL_SHA256,
                          "error_correction": "M", "quiet_zone_modules": QUIET_ZONE, "matrix_modules": modules,
                          "files_sha256": {name: hashlib.sha256(content).hexdigest()
                                           for name, content in outputs.items()}}, indent=2))
    except (ValueError, OSError, ImportError) as error:
        parser.exit(1, f"Support generation failed: {error}\n")


if __name__ == "__main__":
    main()
