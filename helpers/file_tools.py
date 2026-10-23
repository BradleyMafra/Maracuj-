#!/usr/bin/env python3
"""Bounded, offline file utilities. The caller supplies a private output path.

Metadata editing rewrites JPEG/PNG/WebP containers without decoding pixel data.
It removes embedded metadata, not sensitive text visible in document contents.
"""
from __future__ import annotations

import argparse
import binascii
import io
import json
import os
import re
from pathlib import Path
import shutil
import struct
import sys
import warnings
import zipfile
import xml.etree.ElementTree as ET

MAX_SOURCE = 512 * 1024 * 1024
MAX_TOTAL = 2 * 1024 * 1024 * 1024
MAX_METADATA_SOURCE = 128 * 1024 * 1024
MAX_XML = 2 * 1024 * 1024
MAX_PAGES = 200
MAX_ROWS = 600
MAX_VALUE = 4096
IMAGE_EXTENSIONS = {"jpg", "jpeg", "png", "webp", "tif", "tiff"}
OOXML_EXTENSIONS = {"docx", "xlsx", "pptx"}
STRUCTURAL_EXIF = {256, 257, 258, 259, 262, 273, 274, 277, 278, 279, 282, 283, 284, 296, 317, 320, 322, 323, 324, 325, 338, 339, 34675, 40961, 40962, 40963}
EDITABLE_EXIF = {270: "Descrição", 315: "Autor", 33432: "Copyright", 306: "Data e hora"}
EDITABLE_PNG = {"Title", "Author", "Description", "Copyright", "Comment", "Disclaimer", "Source", "Software", "Creation Time"}
PDF_FIELDS = {"Title": "Título", "Author": "Autor", "Subject": "Assunto", "Keywords": "Palavras-chave", "Creator": "Criador", "Producer": "Produtor", "CreationDate": "Data de criação", "ModDate": "Data de alteração"}
EDITABLE_PDF = {"Title", "Author", "Subject", "Keywords", "Creator"}
CORE_NS = "http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
DC_NS = "http://purl.org/dc/elements/1.1/"
DCT_NS = "http://purl.org/dc/terms/"
APP_NS = "http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"
CORE_FIELDS = {"title": DC_NS, "subject": DC_NS, "creator": DC_NS, "description": DC_NS,
               "keywords": CORE_NS, "lastModifiedBy": CORE_NS, "category": CORE_NS,
               "contentStatus": CORE_NS, "version": CORE_NS, "revision": CORE_NS,
               "created": DCT_NS, "modified": DCT_NS, "lastPrinted": CORE_NS}
EDITABLE_CORE = {"title", "subject", "creator", "description", "keywords", "category", "contentStatus", "version"}


class ToolError(Exception):
    pass


def source_path(value, metadata=False):
    if not isinstance(value, str) or not value:
        raise ToolError("Selecione um arquivo local válido.")
    path = Path(value)
    limit = MAX_METADATA_SOURCE if metadata else MAX_SOURCE
    if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
        raise ToolError("Arquivo não disponível, atalho simbólico ou maior que o limite permitido.")
    return path.resolve()


def output_path(value, source):
    path = Path(value)
    if path.exists() or path.is_symlink() or not path.parent.is_dir() or path.resolve() == source:
        raise ToolError("O destino já existe ou não está disponível.")
    return path


def text_value(value):
    if isinstance(value, bytes):
        if len(value) > MAX_VALUE or b"\x00" in value:
            return "[" + str(len(value)) + " bytes]"
        value = value.decode("utf-8", "replace")
    elif isinstance(value, (dict, list, tuple)):
        value = str(value)
    return str(value).replace("\x00", "")[:MAX_VALUE]


def row(group, key, label, value, editable=False, removable=True):
    return {"group": group, "key": key, "label": label, "value": text_value(value),
            "editable": editable, "removable": removable}


def image_open(path):
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = 50_000_000
    warnings.simplefilter("error", Image.DecompressionBombWarning)
    image = Image.open(path)
    if image.width * image.height > 50_000_000:
        raise ToolError("A imagem excede o limite de 50 milhões de pixels.")
    return image


def image_exif(image):
    from PIL import Image
    original = image.getexif()
    result = Image.Exif()
    for key, value in original.items():
        result[key] = value
    for pointer in (34665, 34853):
        if pointer in original:
            result[pointer] = dict(original.get_ifd(pointer))
    if 34665 in result and 40965 in result[34665]:
        result[34665][40965] = dict(original.get_ifd(40965))
    return result


def image_rows(path):
    from PIL import ExifTags
    with image_open(path) as image:
        rows = [row("Arquivo", "file:format", "Formato", image.format, removable=False),
                row("Imagem", "image:width", "Largura", image.width, removable=False),
                row("Imagem", "image:height", "Altura", image.height, removable=False),
                row("Imagem", "image:mode", "Cores", image.mode, removable=False)]
        exif = image.getexif()
        existing = set()
        for key, value in exif.items():
            if key == 34665:
                for subkey, subvalue in exif.get_ifd(key).items():
                    rows.append(row("EXIF · Câmera", f"exifSub:{subkey}", ExifTags.TAGS.get(subkey, str(subkey)), subvalue, removable=subkey not in STRUCTURAL_EXIF))
            elif key == 34853:
                rows.append(row("EXIF · Localização", "exif:34853", "GPS", exif.get_ifd(key)))
            else:
                rows.append(row("EXIF", f"exif:{key}", EDITABLE_EXIF.get(key, ExifTags.TAGS.get(key, str(key))), value,
                                key in EDITABLE_EXIF, key not in STRUCTURAL_EXIF))
                existing.add(key)
        for key, label in EDITABLE_EXIF.items():
            if key not in existing:
                rows.append(row("EXIF", f"exif:{key}", label, "", True))
        if image.format == "PNG":
            for key, value in getattr(image, "text", {}).items():
                rows.append(row("Texto PNG", "text:" + key, key, value, key in EDITABLE_PNG))
            for key in ("Title", "Author", "Description", "Copyright"):
                if key not in getattr(image, "text", {}):
                    rows.append(row("Texto PNG", "text:" + key, key, "", True))
        for key in ("xmp", "XML:com.adobe.xmp", "comment"):
            if key in image.info and image.info[key]:
                rows.append(row("Metadados", "image:" + key, "XMP" if "xmp" in key.lower() else "Comentário", image.info[key]))
        if image.format == "JPEG":
            data = path.read_bytes()
            segments, _ = jpeg_segments(data)
            if any(marker == 0xED for marker, _ in segments):
                rows.append(row("Metadados", "image:iptc", "IPTC / Photoshop", "Bloco de metadados incorporado"))
        return rows[:MAX_ROWS]


def jpeg_segments(data):
    if not data.startswith(b"\xff\xd8"):
        raise ToolError("A imagem JPEG não é válida.")
    result, pos = [], 2
    while pos < len(data):
        begin = pos
        if data[pos] != 0xFF:
            raise ToolError("A imagem JPEG não é válida.")
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        if pos >= len(data):
            break
        marker = data[pos]
        pos += 1
        if marker in (0xDA, 0xD9):
            return result, data[begin:]
        if marker in (0x01, *range(0xD0, 0xD8)):
            result.append((marker, data[begin:pos]))
            continue
        if pos + 2 > len(data):
            break
        length = int.from_bytes(data[pos:pos + 2], "big")
        if length < 2 or pos + length > len(data):
            break
        result.append((marker, data[begin:pos + length]))
        pos += length
    raise ToolError("A imagem JPEG está incompleta.")


def jpeg_metadata(data, exif_bytes, remove, remove_all):
    segments, tail = jpeg_segments(data)
    output = bytearray(b"\xff\xd8")
    if exif_bytes:
        if len(exif_bytes) > 65533:
            raise ToolError("O bloco EXIF é grande demais para JPEG.")
        output.extend(b"\xff\xe1" + (len(exif_bytes) + 2).to_bytes(2, "big") + exif_bytes)
    for marker, segment in segments:
        payload = segment[4:]
        is_exif = marker == 0xE1 and payload.startswith(b"Exif\x00\x00")
        is_xmp = marker == 0xE1 and (payload.startswith(b"http://ns.adobe.com/xap/") or payload.startswith(b"http://ns.adobe.com/xmp/"))
        if is_exif or (is_xmp and (remove_all or "image:xmp" in remove)):
            continue
        if marker == 0xED and (remove_all or "image:iptc" in remove):
            continue
        if marker == 0xFE and (remove_all or "image:comment" in remove):
            continue
        # JFIF, ICC and Adobe color transform are needed for faithful display.
        if remove_all and 0xE0 <= marker <= 0xEF:
            display = (marker == 0xE0 and payload.startswith(b"JFIF\x00")) or (marker == 0xE2 and payload.startswith(b"ICC_PROFILE\x00")) or (marker == 0xEE and payload.startswith(b"Adobe"))
            if not display:
                continue
        output.extend(segment)
    output.extend(tail)
    return output


def png_chunks(data):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ToolError("A imagem PNG não é válida.")
    pos = 8
    while pos + 12 <= len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        if length > MAX_METADATA_SOURCE or pos + length + 12 > len(data):
            break
        payload = data[pos + 8:pos + 8 + length]
        yield kind, payload
        pos += length + 12
        if kind == b"IEND":
            if pos != len(data):
                raise ToolError("A imagem PNG contém dados extras não suportados.")
            return
    raise ToolError("A imagem PNG está incompleta.")


def png_chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF)


def png_metadata(data, exif_bytes, changes, remove, remove_all):
    chunks = list(png_chunks(data))
    output = bytearray(data[:8])
    for kind, payload in chunks:
        if kind == b"eXIf":
            continue
        if kind in (b"tEXt", b"zTXt", b"iTXt"):
            keyword = payload.split(b"\x00", 1)[0].decode("latin-1")
            if remove_all or "text:" + keyword in remove or "text:" + keyword in changes or ("xmp" in keyword.lower() and "image:XML:com.adobe.xmp" in remove):
                continue
        # Unknown private ancillary chunks can themselves carry identifying data.
        if remove_all and kind[:1].islower() and kind not in {b"cHRM", b"gAMA", b"iCCP", b"sRGB", b"sBIT", b"tRNS", b"pHYs", b"acTL", b"fcTL", b"fdAT"}:
            continue
        if kind == b"IDAT" and exif_bytes is not None:
            if exif_bytes:
                output.extend(png_chunk(b"eXIf", exif_bytes.removeprefix(b"Exif\x00\x00")))
            for key, value in changes.items():
                if key.startswith("text:") and value:
                    keyword = key[5:].encode("latin-1")
                    output.extend(png_chunk(b"iTXt", keyword + b"\x00\x00\x00\x00\x00" + value.encode("utf-8")))
            exif_bytes = None
        output.extend(png_chunk(kind, payload))
    return output


def webp_metadata(data, exif_bytes, remove, remove_all):
    if data[:4] != b"RIFF" or data[8:12] != b"WEBP" or int.from_bytes(data[4:8], "little") + 8 != len(data):
        raise ToolError("A imagem WebP não é válida.")
    chunks, pos = [], 12
    while pos + 8 <= len(data):
        kind, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        if pos + 8 + size + (size % 2) > len(data):
            raise ToolError("A imagem WebP está incompleta.")
        payload = data[pos + 8:pos + 8 + size]
        if kind != b"EXIF" and not (kind == b"XMP " and (remove_all or "image:xmp" in remove)):
            chunks.append((kind, payload))
        pos += 8 + size + (size % 2)
    if pos != len(data):
        raise ToolError("A imagem WebP está incompleta.")
    if exif_bytes:
        chunks.append((b"EXIF", exif_bytes))
    has_exif, has_xmp = any(k == b"EXIF" for k, _ in chunks), any(k == b"XMP " for k, _ in chunks)
    if has_exif and not any(k == b"VP8X" for k, _ in chunks):
        with image_open(io.BytesIO(data)) as image:
            flags = 0x08 | (0x10 if "A" in image.mode else 0)
            chunks.insert(0, (b"VP8X", bytes([flags, 0, 0, 0]) + (image.width - 1).to_bytes(3, "little") + (image.height - 1).to_bytes(3, "little")))
    payload = bytearray(b"WEBP")
    for kind, block in chunks:
        if kind == b"VP8X":
            flags = (block[0] & ~0x0C) | (0x08 if has_exif else 0) | (0x04 if has_xmp else 0)
            block = bytes([flags]) + block[1:]
        payload.extend(kind + struct.pack("<I", len(block)) + block + (b"\x00" if len(block) % 2 else b""))
    return b"RIFF" + struct.pack("<I", len(payload)) + payload


def edit_image(path, output, options, allowed):
    from PIL import Image
    changes, remove, remove_all = validate_edits(options, allowed)
    with image_open(path) as image:
        image_format = image.format
        exif = image_exif(image)
        if remove_all:
            orientation = exif.get(274)
            exif = Image.Exif()
            if orientation is not None:
                exif[274] = orientation
        for key in remove:
            if key.startswith("exif:"):
                exif.pop(int(key[5:]), None)
            elif key.startswith("exifSub:") and 34665 in exif:
                exif[34665].pop(int(key[8:]), None)
        for key, value in changes.items():
            if key.startswith("exif:"):
                if value:
                    exif[int(key[5:])] = value
                else:
                    exif.pop(int(key[5:]), None)
        exif_bytes = exif.tobytes() if len(exif) else b""
        if image_format in ("TIFF",):
            if getattr(image, "n_frames", 1) != 1:
                raise ToolError("A edição de metadados de TIFF com várias páginas ainda não está disponível.")
            image.load()
            tiffinfo = dict(image.tag_v2)
            # Source IFD offsets become invalid after rewriting the pixels.
            # Supply the edited dictionaries so Pillow writes fresh offsets.
            for pointer in (34665, 34853):
                if pointer in exif:
                    if exif[pointer]:
                        tiffinfo[pointer] = exif[pointer]
                    else:
                        tiffinfo.pop(pointer, None)
            # Keep only structural/display tags when removing private information.
            if remove_all:
                tiffinfo = {key: value for key, value in tiffinfo.items() if key in {256, 257, 258, 259, 262, 274, 277, 282, 283, 284, 296, 317, 320, 338, 339, 34675}}
            for key in remove:
                if key.startswith("exif:"):
                    tiffinfo.pop(int(key[5:]), None)
            for key, value in changes.items():
                if key.startswith("exif:"):
                    tiffinfo[int(key[5:])] = value
            clean = image.copy()
            clean.info = {}
            # The pinned libtiff encoder cannot serialize nested IFD dictionaries.
            # Pillow's raw encoder preserves them with correct offsets, losslessly.
            compression = "raw" if any(pointer in tiffinfo for pointer in (34665, 34853)) else "tiff_lzw"
            clean.save(output, format="TIFF", compression=compression, tiffinfo=tiffinfo)
            return "Os pixels foram preservados; o TIFF foi regravado sem perdas."
    data = path.read_bytes()
    if image_format == "JPEG":
        data = jpeg_metadata(data, exif_bytes, remove, remove_all)
    elif image_format == "PNG":
        data = png_metadata(data, exif_bytes, changes, remove, remove_all)
    elif image_format == "WEBP":
        data = webp_metadata(data, exif_bytes, remove, remove_all)
    else:
        raise ToolError("A edição de metadados desta imagem ainda não está disponível.")
    output.write_bytes(data)
    return "Pixels preservados sem recompressão. Orientação e perfil de cores são mantidos."


def pdf_reader(path):
    from pypdf import PdfReader
    reader = PdfReader(path, strict=False)
    if reader.is_encrypted:
        raise ToolError("PDF protegido: exporte uma cópia sem senha antes de usar esta ferramenta.")
    if len(reader.pages) > MAX_PAGES:
        raise ToolError("O PDF excede o limite de 200 páginas.")
    fields = reader.get_fields() or {}
    if any(str(value.get("/FT", "")) == "/Sig" for value in fields.values()):
        raise ToolError("PDF assinado: editar ou comprimir invalidaria sua assinatura digital.")
    return reader


def pdf_rows(path):
    reader = pdf_reader(path)
    rows = [row("Arquivo", "file:format", "Formato", "PDF", removable=False),
            row("Documento", "pdf:pages", "Páginas", len(reader.pages), removable=False)]
    metadata = reader.metadata or {}
    for key, value in metadata.items():
        clean = str(key).lstrip("/")
        rows.append(row("PDF · Propriedades", "pdf:" + clean, PDF_FIELDS.get(clean, clean), value, clean in EDITABLE_PDF))
    for key in EDITABLE_PDF:
        if "/" + key not in metadata:
            rows.append(row("PDF · Propriedades", "pdf:" + key, PDF_FIELDS[key], "", True))
    if reader.trailer["/Root"].get("/Metadata") is not None:
        rows.append(row("PDF · XMP", "pdf:xmp", "XMP", "Bloco XMP incorporado"))
    return rows[:MAX_ROWS]


def edit_pdf(path, output, options, allowed):
    from pypdf import PdfWriter
    changes, remove, remove_all = validate_edits(options, allowed)
    reader = pdf_reader(path)
    writer = PdfWriter(clone_from=reader)
    values = {} if remove_all else {str(k): str(v) for k, v in (reader.metadata or {}).items()}
    for key in remove:
        if key.startswith("pdf:"):
            values.pop("/" + key[4:], None)
    for key, value in changes.items():
        if value:
            values["/" + key[4:]] = value
        else:
            values.pop("/" + key[4:], None)
    writer.metadata = values or None
    if remove_all or "pdf:xmp" in remove:
        writer.root_object.pop("/Metadata", None)
    writer.compress_identical_objects(remove_duplicates=True, remove_unreferenced=True)
    with output.open("xb") as stream:
        writer.write(stream)
    return "Propriedades e XMP foram tratados; conteúdo, comentários e anexos do PDF não são removidos."


def safe_xml(data):
    if len(data) > MAX_XML or b"<!DOCTYPE" in data.upper() or b"<!ENTITY" in data.upper():
        raise ToolError("O bloco de metadados XML excede os limites permitidos.")
    return ET.fromstring(data)


def open_ooxml(path):
    archive = zipfile.ZipFile(path)
    entries = archive.infolist()
    if len(entries) > 20000 or sum(entry.file_size for entry in entries) > 256 * 1024 * 1024 or len({entry.filename for entry in entries}) != len(entries):
        archive.close()
        raise ToolError("O documento excede os limites de processamento.")
    if any(entry.flag_bits & 1 for entry in entries):
        archive.close()
        raise ToolError("Documentos criptografados não são suportados.")
    if any(entry.filename.startswith("_xmlsignatures/") for entry in entries):
        archive.close()
        raise ToolError("Documento assinado: a alteração invalidaria sua assinatura digital.")
    if "[Content_Types].xml" not in archive.namelist():
        archive.close()
        raise ToolError("O documento Office não é válido.")
    return archive


def ooxml_rows(path):
    rows = [row("Arquivo", "file:format", "Formato", path.suffix[1:].upper(), removable=False)]
    with open_ooxml(path) as archive:
        names = set(archive.namelist())
        for name, group, prefix in [("docProps/core.xml", "Office · Propriedades", "core:"), ("docProps/app.xml", "Office · Aplicativo", "app:")]:
            existing = set()
            if name in names:
                for node in safe_xml(archive.read(name)):
                    key = node.tag.rsplit("}", 1)[-1]
                    if prefix == "core:" or key in {"Company", "Manager", "Application", "AppVersion", "Template", "TotalTime", "HyperlinkBase"}:
                        rows.append(row(group, prefix + key, key, node.text or "", key in (EDITABLE_CORE if prefix == "core:" else {"Company", "Manager"})))
                        existing.add(key)
            defaults = {"title", "creator", "subject", "description", "keywords"} if prefix == "core:" else {"Company", "Manager"}
            for key in defaults - existing:
                rows.append(row(group, prefix + key, key, "", True))
        if "docProps/custom.xml" in names:
            root = safe_xml(archive.read("docProps/custom.xml"))
            for node in root:
                key = node.get("name", "")
                if key and len(key) < 200:
                    rows.append(row("Office · Personalizadas", "custom:" + key, key, " ".join(node.itertext())))
        if any(name.startswith("docProps/thumbnail.") for name in names):
            rows.append(row("Office · Miniatura", "office:thumbnail", "Miniatura", "Prévia incorporada"))
    return rows[:MAX_ROWS]


def edit_ooxml(path, output, options, allowed):
    changes, remove, remove_all = validate_edits(options, allowed)
    with open_ooxml(path) as archive:
        replacements, deleted = {}, set()
        for name, prefix, root_tag in [("docProps/core.xml", "core:", "{" + CORE_NS + "}coreProperties"), ("docProps/app.xml", "app:", "{" + APP_NS + "}Properties")]:
            root = safe_xml(archive.read(name)) if name in archive.namelist() else ET.Element(root_tag)
            for node in list(root):
                key = node.tag.rsplit("}", 1)[-1]
                private = prefix == "core:" or key in {"Company", "Manager", "Application", "AppVersion", "Template", "TotalTime", "HyperlinkBase"}
                if private and (remove_all or prefix + key in remove or prefix + key in changes):
                    root.remove(node)
            for key, value in changes.items():
                if key.startswith(prefix) and value:
                    local = key[len(prefix):]
                    namespace = CORE_FIELDS[local] if prefix == "core:" else APP_NS
                    ET.SubElement(root, "{" + namespace + "}" + local).text = value
            if name in archive.namelist():
                replacements[name] = ET.tostring(root, encoding="utf-8", xml_declaration=True)
            elif list(root):
                # Real Office packages normally have both properties parts. Do not
                # add an unreferenced part to a nonconforming package silently.
                raise ToolError("Este documento não tem o bloco de propriedades necessário para adicionar o campo.")
        if "docProps/custom.xml" in archive.namelist():
            root = safe_xml(archive.read("docProps/custom.xml"))
            for node in list(root):
                if remove_all or "custom:" + node.get("name", "") in remove:
                    root.remove(node)
            if list(root):
                replacements["docProps/custom.xml"] = ET.tostring(root, encoding="utf-8", xml_declaration=True)
            else:
                deleted.add("docProps/custom.xml")
        if remove_all or "office:thumbnail" in remove:
            deleted.update(name for name in archive.namelist() if name.startswith("docProps/thumbnail."))
        for name in ("[Content_Types].xml", "_rels/.rels"):
            if name in archive.namelist() and deleted:
                root = safe_xml(archive.read(name))
                for node in list(root):
                    target = (node.get("PartName") or node.get("Target") or "").lstrip("/")
                    if target in deleted:
                        root.remove(node)
                replacements[name] = ET.tostring(root, encoding="utf-8", xml_declaration=True)
        with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as result:
            for item in archive.infolist():
                if item.filename not in deleted:
                    result.writestr(item, replacements.get(item.filename, archive.read(item.filename)))
    return "Propriedades Office e miniatura foram tratadas; comentários e conteúdo do documento não são removidos."


def validate_edits(options, rows):
    changes = options.get("changes", {})
    remove = options.get("remove", [])
    remove_all = options.get("removeAll", False)
    if not isinstance(changes, dict) or not isinstance(remove, list) or len(changes) > 100 or len(remove) > MAX_ROWS or not isinstance(remove_all, bool):
        raise ToolError("As alterações de metadados não são válidas.")
    allowed = {item["key"]: item for item in rows}
    for key, value in changes.items():
        if key not in allowed or not allowed[key]["editable"] or not isinstance(value, str) or len(value) > MAX_VALUE or "\x00" in value:
            raise ToolError("Um campo estrutural ou não suportado não pode ser alterado.")
    if any(not isinstance(key, str) or key not in allowed or not allowed[key]["removable"] for key in remove):
        raise ToolError("Um campo estrutural ou não suportado não pode ser removido.")
    # Explicit removal wins over a stale editor draft, including Remove All.
    changes = {} if remove_all else {key: value for key, value in changes.items() if key not in remove}
    return changes, set(remove), remove_all


def inspect(path):
    extension = path.suffix[1:].lower()
    if extension in IMAGE_EXTENSIONS:
        rows = image_rows(path)
    elif extension == "pdf":
        rows = pdf_rows(path)
    elif extension in OOXML_EXTENSIONS:
        rows = ooxml_rows(path)
    else:
        rows = [row("Arquivo", "file:format", "Tipo", extension.upper() or "Arquivo", removable=False),
                row("Arquivo", "file:size", "Tamanho em bytes", path.stat().st_size, removable=False)]
    return {"ok": True, "metadata": rows, "details": {"kind": extension, "editable": any(item["editable"] or item["removable"] for item in rows),
            "note": "A remoção trata os metadados suportados; não apaga texto visível, comentários, anexos ou histórico de revisão dentro dos documentos."}}


def compression_target(options, size):
    target = options.get("targetBytes", 0)
    percent = options.get("targetPercent", 0)
    if not isinstance(target, (int, float)) or not isinstance(percent, (int, float)) or target < 0 or not 0 <= percent < 100:
        raise ToolError("O tamanho-alvo de compressão não é válido.")
    if target:
        return max(1, int(target))
    return max(1, int(size * (1 - percent / 100))) if percent else 0


def compress_image(path, output, options):
    from PIL import Image
    strong = options.get("preset", "balanced") == "strong"
    max_edge = options.get("maxEdge", 0)
    if not isinstance(max_edge, int) or max_edge < 0 or max_edge > 20000:
        raise ToolError("A dimensão máxima da imagem não é válida.")
    target = compression_target(options, path.stat().st_size)
    with image_open(path) as image:
        if getattr(image, "n_frames", 1) != 1 or image.format not in {"JPEG", "PNG", "WEBP"}:
            raise ToolError("Para imagens animadas ou este formato, escolha o modo ZIP.")
        image.load()
        original_format = image.format
        metadata = {}
        if image.info.get("icc_profile"):
            metadata["icc_profile"] = image.info["icc_profile"]
        if image.info.get("exif"):
            metadata["exif"] = image.info["exif"]
        if image.info.get("xmp") and original_format in {"JPEG", "WEBP"}:
            metadata["xmp"] = image.info["xmp"]
        if original_format == "PNG":
            from PIL.PngImagePlugin import PngInfo
            info = PngInfo()
            for key, value in getattr(image, "text", {}).items():
                info.add_itxt(key, value)
            metadata["pnginfo"] = info
        original_size = image.size
        if max_edge:
            image.thumbnail((max_edge, max_edge), Image.Resampling.LANCZOS)
        resized = image.size != original_size
        quality = 65 if strong else 85
        candidates = [quality]
        if target and original_format != "PNG":
            candidates.extend(q for q in (70, 55, 40, 25) if q < quality)
        smallest = None
        for quality in candidates:
            stream = io.BytesIO()
            parameters = dict(metadata)
            if original_format == "JPEG":
                parameters.update(quality=quality, optimize=True, progressive=True)
            elif original_format == "PNG":
                parameters.update(optimize=True, compress_level=9)
            else:
                parameters.update(quality=quality, method=6)
            image.save(stream, format=original_format, **parameters)
            data = stream.getvalue()
            if smallest is None or len(data) < len(smallest):
                smallest = data
            if target and len(data) <= target:
                break
        # A requested resize is an operation in its own right: a small,
        # optimized source can produce a larger antialiased thumbnail. Keep
        # the requested dimensions and report the actual byte increase.
        if resized or len(smallest) < path.stat().st_size:
            output.write_bytes(smallest)
        else:
            shutil.copyfile(path, output)
    return ("A dimensão máxima foi aplicada à cópia. " if resized else "") + "PNG usa compressão sem perdas; JPEG e WebP podem perder qualidade. A meta é aproximada."


def check_pdf_image_bounds(page):
    """Check XObject dimensions before pypdf decodes image samples."""
    seen = set()

    def resources_images(resources, depth=0):
        if depth > 16:
            raise ToolError("O PDF excede o limite de complexidade das imagens.")
        resources = resources.get_object() if hasattr(resources, "get_object") else resources
        if not isinstance(resources, dict):
            return
        objects = resources.get("/XObject", {})
        objects = objects.get_object() if hasattr(objects, "get_object") else objects
        if not isinstance(objects, dict):
            return
        for reference in objects.values():
            obj = reference.get_object() if hasattr(reference, "get_object") else reference
            if id(obj) in seen:
                continue
            seen.add(id(obj))
            if len(seen) > 2000:
                raise ToolError("O PDF excede o limite de complexidade das imagens.")
            if not isinstance(obj, dict):
                continue
            if obj.get("/Subtype") == "/Image":
                width, height = int(obj.get("/Width", 0)), int(obj.get("/Height", 0))
                if width <= 0 or height <= 0 or width * height > 50_000_000:
                    raise ToolError("Uma imagem do PDF excede o limite de pixels.")
            elif obj.get("/Subtype") == "/Form":
                resources_images(obj.get("/Resources", {}), depth + 1)
    resources_images(page.get("/Resources", {}))


def compress_pdf(path, output, options):
    from pypdf import PdfWriter
    reader = pdf_reader(path)
    writer = PdfWriter(clone_from=reader)
    strong = options.get("preset", "balanced") == "strong"
    max_edge = options.get("maxEdge", 0)
    if not isinstance(max_edge, int) or max_edge < 0 or max_edge > 20000:
        raise ToolError("A dimensão máxima não é válida.")
    resized = False
    for page in writer.pages:
        check_pdf_image_bounds(page)
        page.compress_content_streams(level=9 if strong else 6)
        if strong or max_edge:
            for item in page.images:
                image = item.image
                if image.width * image.height > 50_000_000:
                    raise ToolError("Uma imagem do PDF excede o limite de pixels.")
                original_size = image.size
                if max_edge:
                    from PIL import Image
                    image.thumbnail((max_edge, max_edge), Image.Resampling.LANCZOS)
                # pypdf retains transparency with image replacement. Avoid
                # replacing unusual CMYK or masks and never drop images.
                if image.mode in {"RGB", "L"}:
                    item.replace(image, quality=65 if strong else 85)
                    resized = resized or image.size != original_size
    writer.compress_identical_objects(remove_duplicates=True, remove_unreferenced=True)
    with output.open("xb") as stream:
        writer.write(stream)
    if not resized and output.stat().st_size >= path.stat().st_size:
        shutil.copyfile(path, output)
    return "O modo equilibrado otimiza a estrutura; o forte também reduz a qualidade das imagens. A meta é aproximada."


def archive_name(name):
    """A single portable basename even when produced on a POSIX filesystem.

    Backslashes and drive colons are ordinary POSIX filename characters but
    become paths when a ZIP is opened on Windows. Archive entries never use
    them, dot prefixes, reserved Windows devices, or trailing dots/spaces.
    """
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f\x7f]', '_', name).lstrip('. ').rstrip('. ')
    if not name:
        name = "arquivo"
    stem = name.split('.', 1)[0].rstrip('. ').upper()
    if stem in {"CON", "PRN", "AUX", "NUL"} or re.fullmatch(r'(?:COM|LPT)[1-9]', stem):
        name = '_' + name
    return name


def compress_zip(paths, output, options):
    names = set()
    adjusted_names = False
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9 if options.get("preset") == "strong" else 6) as archive:
        for path in paths:
            base, number = archive_name(path.name), 1
            name = base
            while name.casefold() in names:
                number += 1
                portable = Path(base)
                name = portable.stem + f" ({number})" + portable.suffix
            adjusted_names = adjusted_names or name != path.name
            names.add(name.casefold())
            # No parent directories or source filesystem timestamps are embedded.
            entry = zipfile.ZipInfo(name)
            entry.compress_type = zipfile.ZIP_DEFLATED
            # Python 3.12 ZipFile.open does not inherit the archive level when
            # given a ZipInfo. Keep streaming while selecting the requested level.
            entry._compresslevel = archive.compresslevel
            entry.external_attr = 0o600 << 16
            with path.open("rb") as source, archive.open(entry, "w", force_zip64=True) as destination:
                shutil.copyfileobj(source, destination, 1024 * 1024)
    return ("Alguns nomes foram ajustados para compatibilidade entre Linux e Windows. " if adjusted_names else "") + "ZIP preserva integralmente os arquivos; arquivos já comprimidos podem gerar um ZIP maior."


def execute(request):
    if not isinstance(request, dict) or not isinstance(request.get("options", {}), dict):
        raise ToolError("A solicitação da ferramenta não é válida.")
    action, options = request.get("action"), request.get("options", {})
    if action == "inspect":
        return inspect(source_path(request.get("input"), True))
    inputs = request.get("inputs", [])
    if not isinstance(inputs, list) or not inputs or len(inputs) > 128:
        raise ToolError("Selecione de 1 a 128 arquivos.")
    paths = [source_path(value, action == "metadata") for value in inputs]
    if sum(path.stat().st_size for path in paths) > MAX_TOTAL:
        raise ToolError("O conjunto de arquivos excede o limite de 2 GiB.")
    output = output_path(request.get("output", ""), paths[0])
    try:
        if action == "metadata":
            if len(paths) != 1:
                raise ToolError("Edite os metadados de um arquivo por vez.")
            rows = inspect(paths[0])["metadata"]
            extension = paths[0].suffix[1:].lower()
            if extension in IMAGE_EXTENSIONS:
                note = edit_image(paths[0], output, options, rows)
            elif extension == "pdf":
                note = edit_pdf(paths[0], output, options, rows)
            elif extension in OOXML_EXTENSIONS:
                note = edit_ooxml(paths[0], output, options, rows)
            else:
                raise ToolError("A edição de metadados deste formato ainda não está disponível.")
        elif action == "compress":
            if options.get("preset", "balanced") not in {"balanced", "strong"} or options.get("mode", "auto") not in {"auto", "zip"}:
                raise ToolError("Escolha uma opção de compressão válida.")
            extension = paths[0].suffix[1:].lower()
            if len(paths) == 1 and options.get("mode", "auto") != "zip" and extension in {"jpg", "jpeg", "png", "webp"}:
                note = compress_image(paths[0], output, options)
            elif len(paths) == 1 and options.get("mode", "auto") != "zip" and extension == "pdf":
                note = compress_pdf(paths[0], output, options)
            else:
                note = compress_zip(paths, output, options)
        else:
            raise ToolError("Esta ferramenta não está disponível.")
        size = output.stat().st_size
        if size <= 0 or size > MAX_TOTAL + 4 * 1024 * 1024:
            raise ToolError("O resultado excedeu os limites permitidos.")
        source_size = sum(path.stat().st_size for path in paths)
        target = compression_target(options, source_size) if action == "compress" else 0
        details = {"inputBytes": source_size, "outputBytes": size,
                   "reducedPercent": round(100 * (1 - size / source_size), 2) if source_size else 0,
                   "targetBytes": target, "targetReached": size <= target if target else True,
                   "note": note}
        details.update(originalBytes=source_size, savedPercent=details["reducedPercent"], targetMet=details["targetReached"])
        if action == "compress" and size >= source_size:
            details["note"] = "Não houve redução de tamanho. " + note if output.suffix.lower() != ".zip" else note
        return {"ok": True, "details": details}
    except Exception:
        if output.is_file():
            output.unlink()
        raise


def limit_worker_memory():
    """Lower only this CLI worker's address-space budget, never the app/host.

    Imported tests do not call this. Existing stricter limits are preserved.
    Windows still has source/page/decoded-pixel guards; POSIX additionally
    bounds parser allocations and expanded PDF streams before they exhaust RAM.
    """
    if os.name != "posix":
        return
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_AS)
        budget = 2 * 1024 * 1024 * 1024
        if soft != resource.RLIM_INFINITY:
            budget = min(budget, soft)
        if hard != resource.RLIM_INFINITY:
            budget = min(budget, hard)
        resource.setrlimit(resource.RLIMIT_AS, (budget, hard))
    except (ImportError, AttributeError, OSError, ValueError):
        # A platform without RLIMIT_AS retains the explicit format limits.
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--request", required=True)
    parser.add_argument("--runtime-root", default="")
    arguments = parser.parse_args()
    request_path = Path(arguments.request)
    try:
        limit_worker_memory()
        if not request_path.is_file() or request_path.stat().st_size > 128 * 1024:
            raise ToolError("A solicitação da ferramenta excede o limite permitido.")
        result = execute(json.loads(request_path.read_text(encoding="utf-8")))
        print(json.dumps(result, ensure_ascii=True), flush=True)
        return 0
    except ToolError as error:
        message = str(error)
    except MemoryError:
        message = "O arquivo excedeu o limite de memória da ferramenta local."
    except (OSError, ValueError, KeyError, TypeError, ImportError, ET.ParseError, zipfile.BadZipFile):
        message = "Não foi possível processar este arquivo com os componentes locais."
    except Exception:
        message = "O arquivo contém dados não suportados ou não pôde ser processado."
    print(json.dumps({"ok": False, "error": message}, ensure_ascii=True), flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
