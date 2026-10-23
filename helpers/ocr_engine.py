"""Local Tesseract C-API worker support; no network, downloads or logging."""

from __future__ import annotations

import ctypes
import importlib.util
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time


MAX_PIXELS = 16_000_000
MAX_TEXT_BYTES = 16 * 1024 * 1024
LANGUAGES = ("por", "eng")
_PLATFORM_MODULE = None


class OCRError(Exception):
    """Only fixed user-facing strings are raised from this module."""


def runtime(root: str = ""):
    global _PLATFORM_MODULE
    if _PLATFORM_MODULE is None:
        path = Path(__file__).resolve().with_name("platform_runtime.py")
        specification = importlib.util.spec_from_file_location("maracuja_ocr_platform", path)
        if specification is None or specification.loader is None:
            raise OCRError("O componente local de OCR não está disponível.")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        _PLATFORM_MODULE = module
    return _PLATFORM_MODULE.Runtime(root)


def data_directory(requested: str = "", runtime_root: str = "") -> Path:
    config = runtime(runtime_root)
    if requested:
        return config.tessdata(requested)
    configured = config.configured("tessdata")
    if configured is not None:
        return configured
    directory = Path(__file__).resolve().parent
    candidates = [config.tessdata(), directory / "ocr/tessdata",
                  directory.parent / "vendor/tessdata-fast-4.1.0"]
    for candidate in candidates:
        if all((candidate / (language + ".traineddata")).is_file() for language in LANGUAGES):
            return candidate
    return candidates[0]


def library_path(runtime_root: str = "") -> Path | None:
    return runtime(runtime_root).tool("tesseract_library")


def capabilities(requested: str = "", runtime_root: str = "") -> dict:
    try:
        pillow = importlib.util.find_spec("PIL") is not None
    except (ImportError, ValueError):
        pillow = False
    directory = data_directory(requested, runtime_root)
    models = all((directory / (language + ".traineddata")).is_file()
                 and os.access(directory / (language + ".traineddata"), os.R_OK)
                 for language in LANGUAGES)
    available = pillow and models and library_path(runtime_root) is not None
    return {"ocr": available, "langs": list(LANGUAGES) if available else []}


class _NativeEngine:
    """One recognition handle per conversion; input buffers live through recognition."""

    def __init__(self, requested: str = "", runtime_root: str = ""):
        if not capabilities(requested, runtime_root)["ocr"]:
            raise OCRError("O reconhecimento de texto local não está disponível. Verifique os idiomas do OCR.")
        # Applied only to this private worker, before loading libgomp/Tesseract.
        os.environ["OMP_THREAD_LIMIT"] = "1"
        os.environ["OMP_NUM_THREADS"] = "1"
        self.handle = None
        self.api = None
        self.dll_directories = []
        try:
            library = library_path(runtime_root)
            if library is None:
                raise OCRError("O motor local de OCR não está disponível.")
            config = runtime(runtime_root)
            self.windows = config.windows
            if config.windows:
                # Load dependencies only from trusted app directories; never add
                # the input file's directory or a global PATH entry.
                for directory in config.dll_directories(library):
                    if directory.is_dir():
                        self.dll_directories.append(os.add_dll_directory(str(directory)))
            # Tesseract's C API uses cdecl, including its Windows DLL builds.
            api = ctypes.CDLL(str(library.resolve()))
            declarations = {
                "TessBaseAPICreate": ([], ctypes.c_void_p),
                "TessBaseAPIDelete": ([ctypes.c_void_p], None),
                "TessBaseAPIInit2": ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int], ctypes.c_int),
                "TessBaseAPISetVariable": ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
                "TessBaseAPISetPageSegMode": ([ctypes.c_void_p, ctypes.c_int], None),
                "TessBaseAPISetImage": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int,
                                         ctypes.c_int, ctypes.c_int, ctypes.c_int], None),
                "TessBaseAPISetSourceResolution": ([ctypes.c_void_p, ctypes.c_int], None),
                "TessBaseAPIRecognize": ([ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
                "TessBaseAPIGetUTF8Text": ([ctypes.c_void_p], ctypes.c_void_p),
                "TessBaseAPIMeanTextConf": ([ctypes.c_void_p], ctypes.c_int),
                "TessDeleteText": ([ctypes.c_void_p], None),
                "TessBaseAPIClear": ([ctypes.c_void_p], None),
                "TessBaseAPIClearAdaptiveClassifier": ([ctypes.c_void_p], None),
                "TessMonitorCreate": ([], ctypes.c_void_p),
                "TessMonitorDelete": ([ctypes.c_void_p], None),
                "TessMonitorSetDeadlineMSecs": ([ctypes.c_void_p, ctypes.c_int], None),
            }
            for name, (arguments, result) in declarations.items():
                function = getattr(api, name)
                function.argtypes = arguments
                function.restype = result
            self.api = api
            self.handle = api.TessBaseAPICreate()
            if not self.handle:
                raise OCRError("Não foi possível iniciar o reconhecimento de texto local.")
            # Fast models contain the LSTM engine only; no legacy/OSD model needed.
            models = data_directory(requested, runtime_root).resolve()
            if config.windows:
                # The MinGW Tesseract build opens model paths with narrow fopen;
                # Python's UTF-8 filesystem encoding is not the Windows ANSI
                # code page. This single-conversion worker can set its cwd via
                # the Unicode Windows API and pass an ASCII relative path while
                # loading the models. Restore cwd even if initialization fails.
                previous_directory = Path.cwd()
                try:
                    os.chdir(models)
                    initialized = api.TessBaseAPIInit2(self.handle, b".", b"por+eng", 1)
                finally:
                    os.chdir(previous_directory)
            else:
                initialized = api.TessBaseAPIInit2(self.handle, os.fsencode(models), b"por+eng", 1)
            if initialized:
                raise OCRError("Não foi possível carregar os idiomas português e inglês do OCR.")
            api.TessBaseAPISetVariable(self.handle, b"debug_file", os.fsencode(os.devnull))
            api.TessBaseAPISetVariable(self.handle, b"classify_enable_learning", b"0")
            api.TessBaseAPISetPageSegMode(self.handle, 3)  # PSM_AUTO, without orientation detection.
        except OCRError:
            self.close()
            raise
        except Exception:
            self.close()
            raise OCRError("Não foi possível carregar o motor de reconhecimento de texto local.") from None

    def __enter__(self):
        return self

    def __exit__(self, _kind, _value, _traceback):
        self.close()

    def close(self) -> None:
        if self.api is not None and self.handle:
            self.api.TessBaseAPIDelete(self.handle)
            self.handle = None
        for directory in getattr(self, "dll_directories", []):
            directory.close()
        self.dll_directories = []

    def recognize(self, image, dpi: int = 300) -> str:
        from PIL import Image, ImageOps
        if not self.handle:
            raise OCRError("O reconhecimento de texto já foi encerrado.")
        width, height = image.size
        if width < 1 or height < 1 or width * height > MAX_PIXELS:
            raise OCRError("A imagem para OCR excede o limite de 16 milhões de pixels.")
        # Work with a private copy: normalize orientation and composite transparency
        # over white. This never modifies the original image/file.
        prepared = ImageOps.exif_transpose(image)
        if prepared.mode in {"RGBA", "LA"} or "transparency" in prepared.info:
            rgba = prepared.convert("RGBA")
            background = Image.new("RGBA", rgba.size, "white")
            background.alpha_composite(rgba)
            prepared = background.convert("L")
            rgba.close()
            background.close()
        else:
            prepared = prepared.convert("L")
        try:
            width, height = prepared.size
            # Tesseract names method 0 Otsu and method 1 Leptonica Otsu. The
            # Windows build can favor either for a scan, so compare its own
            # confidence scores without changing text or selecting languages.
            # Both attempts share the original 45s budget; ties retain method 0.
            methods = (0, 1) if self.windows else (None,)
            deadline = time.monotonic() + 45 if self.windows else None
            best_text, best_confidence = None, -1
            for method in methods:
                remaining_ms = int((deadline - time.monotonic()) * 1000) if self.windows else 45000
                if remaining_ms <= 0:
                    if best_text is not None:
                        return best_text
                    raise OCRError("O OCR não conseguiu concluir a leitura dentro do limite de tempo.")
                monitor = text_pointer = buffer = recognized = text = None
                try:
                    if method is not None and not self.api.TessBaseAPISetVariable(
                            self.handle, b"thresholding_method", str(method).encode("ascii")):
                        raise OCRError("Não foi possível configurar a preparação da imagem para o OCR.")
                    self.api.TessBaseAPIClearAdaptiveClassifier(self.handle)
                    raw = prepared.tobytes()
                    buffer = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw)
                    del raw
                    self.api.TessBaseAPISetImage(self.handle, buffer, width, height, 1, width)
                    self.api.TessBaseAPISetSourceResolution(self.handle, max(70, min(int(dpi), 600)))
                    monitor = self.api.TessMonitorCreate()
                    if not monitor:
                        raise OCRError("Não foi possível iniciar a leitura da imagem.")
                    if self.windows:
                        remaining_ms = int((deadline - time.monotonic()) * 1000)
                        if remaining_ms <= 0:
                            if best_text is not None:
                                return best_text
                            raise OCRError("O OCR não conseguiu concluir a leitura dentro do limite de tempo.")
                    self.api.TessMonitorSetDeadlineMSecs(monitor, remaining_ms)
                    if self.api.TessBaseAPIRecognize(self.handle, monitor):
                        if best_text is not None:
                            return best_text
                        raise OCRError("O OCR não conseguiu concluir a leitura dentro do limite de tempo.")
                    text_pointer = self.api.TessBaseAPIGetUTF8Text(self.handle)
                    if not text_pointer:
                        raise OCRError("Não foi possível extrair o texto reconhecido.")
                    recognized = ctypes.string_at(text_pointer)
                    if len(recognized) > MAX_TEXT_BYTES:
                        raise OCRError("O texto reconhecido excede o limite de tamanho.")
                    text = recognized.decode("utf-8", errors="replace")
                    confidence = self.api.TessBaseAPIMeanTextConf(self.handle) if self.windows else 0
                    if best_text is None or confidence > best_confidence:
                        best_text, best_confidence = text, confidence
                finally:
                    recognized = text = None
                    if text_pointer:
                        self.api.TessDeleteText(text_pointer)
                    if monitor:
                        self.api.TessMonitorDelete(monitor)
                    self.api.TessBaseAPIClear(self.handle)
                    buffer = None
            return best_text
        finally:
            prepared.close()


def _read_exact(stream, size):
    chunks = bytearray()
    while len(chunks) < size:
        part = stream.read(size - len(chunks))
        if not part:
            raise EOFError("OCR worker pipe closed")
        chunks.extend(part)
    return bytes(chunks)


class _IsolatedEngine:
    """Keep MSYS2 OCR DLLs separate from Python/PDF OpenSSL DLLs on Windows."""

    def __init__(self, requested: str = "", runtime_root: str = ""):
        self.process = None
        if not capabilities(requested, runtime_root)["ocr"]:
            raise OCRError("O reconhecimento de texto local não está disponível. Verifique os idiomas do OCR.")
        try:
            # Same private interpreter and trusted sibling; inherit the parent's
            # Job Object so cancellation still terminates the whole conversion.
            self.process = subprocess.Popen(
                [sys.executable, "-I", str(Path(__file__).resolve()), "--ocr-worker",
                 requested, runtime_root], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, **runtime(runtime_root).subprocess_options())
            self._exchange(None, timeout=60)
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, _kind, _value, _traceback):
        self.close()

    def _exchange(self, header, pixels=b"", timeout=60):
        result = queue.Queue(maxsize=1)
        process = self.process

        def communicate():
            try:
                if header is not None:
                    process.stdin.write(json.dumps(header).encode("ascii") + b"\n")
                    process.stdin.write(pixels)
                    process.stdin.flush()
                line = process.stdout.readline(4097)
                if len(line) > 4096 or not line.endswith(b"\n"):
                    raise ValueError("Invalid OCR worker response")
                response = json.loads(line)
                if not isinstance(response, dict) or not isinstance(response.get("ok"), bool):
                    raise ValueError("Invalid OCR worker status")
                if not response["ok"]:
                    raise OCRError(response.get("error", "Não foi possível concluir o reconhecimento de texto."))
                length = response.get("bytes")
                if type(length) is not int or not 0 <= length <= MAX_TEXT_BYTES:
                    raise ValueError("Invalid OCR text length")
                result.put(_read_exact(process.stdout, length).decode("utf-8"))
            except BaseException as error:
                result.put(error)

        # Bound initialization, both pipe writes and response reads as well as
        # recognition. The native worker also retains its 45-second OCR limit.
        thread = threading.Thread(target=communicate, daemon=True)
        thread.start()
        try:
            value = result.get(timeout=timeout)
        except queue.Empty:
            self.close()
            raise OCRError("O reconhecimento de texto excedeu o limite de tempo.") from None
        if isinstance(value, BaseException):
            self.close()
            if isinstance(value, OCRError):
                raise value
            raise OCRError("Não foi possível executar o reconhecimento de texto local.") from None
        return value

    def recognize(self, image, dpi: int = 300) -> str:
        from PIL import Image, ImageOps
        if self.process is None:
            raise OCRError("O reconhecimento de texto já foi encerrado.")
        width, height = image.size
        if width < 1 or height < 1 or width * height > MAX_PIXELS:
            raise OCRError("A imagem para OCR excede o limite de 16 milhões de pixels.")
        oriented = ImageOps.exif_transpose(image)
        rgba = background = None
        try:
            if oriented.mode in {"RGBA", "LA"} or "transparency" in oriented.info:
                rgba = oriented.convert("RGBA")
                background = Image.new("RGBA", rgba.size, "white")
                background.alpha_composite(rgba)
                grayscale = background.convert("L")
            else:
                grayscale = oriented.convert("L")
            try:
                width, height = grayscale.size
                return self._exchange({"width": width, "height": height,
                                       "dpi": max(70, min(int(dpi), 600))}, grayscale.tobytes())
            finally:
                grayscale.close()
        finally:
            oriented.close()
            if rgba is not None:
                rgba.close()
            if background is not None:
                background.close()

    def close(self):
        process, self.process = self.process, None
        if process is None:
            return
        # Terminate only this private worker, including on pipe timeouts. It
        # never starts subprocesses of its own or accesses the original file.
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        for pipe in (process.stdin, process.stdout):
            if pipe is not None:
                try:
                    pipe.close()
                except OSError:
                    pass


class Engine(_IsolatedEngine if os.name == "nt" else _NativeEngine):
    """One bounded recognition handle per conversion, preserving the public API."""


def _worker(requested, runtime_root):
    def respond(text="", error=None):
        encoded = text.encode("utf-8")
        header = {"ok": error is None, "bytes": len(encoded)}
        if error is not None:
            header["error"] = error
        sys.stdout.buffer.write(json.dumps(header).encode("ascii") + b"\n")
        sys.stdout.buffer.write(encoded)
        sys.stdout.buffer.flush()

    try:
        with _NativeEngine(requested, runtime_root) as engine:
            from PIL import Image
            respond()
            while True:
                line = sys.stdin.buffer.readline(4097)
                if not line:
                    return 0
                if len(line) > 4096 or not line.endswith(b"\n"):
                    raise ValueError("Invalid OCR image header")
                header = json.loads(line)
                width, height, dpi = (header.get(key) for key in ("width", "height", "dpi"))
                if (any(type(value) is not int for value in (width, height, dpi))
                        or width < 1 or height < 1 or width * height > MAX_PIXELS or not 70 <= dpi <= 600):
                    raise ValueError("Invalid OCR image dimensions")
                pixels = _read_exact(sys.stdin.buffer, width * height)
                with Image.frombytes("L", (width, height), pixels) as image:
                    text = engine.recognize(image, dpi)
                respond(text)
    except BaseException as error:
        message = str(error) if isinstance(error, OCRError) else "Não foi possível executar o reconhecimento de texto local."
        try:
            respond(error=message)
        except (OSError, ValueError):
            pass
        return 1


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] != "--ocr-worker":
        raise SystemExit(2)
    raise SystemExit(_worker(sys.argv[2], sys.argv[3]))
