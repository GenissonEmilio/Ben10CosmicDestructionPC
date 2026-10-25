#!/usr/bin/env python3
"""Import an owned Ben 10: Ultimate Alien — Cosmic Destruction USA ISO or nested 7Z/RAR/ZIP, without executing archive contents."""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import sys
import zipfile

from iso9660 import scan, write_layout

ROOT = Path(__file__).resolve().parents[2]
MAX_BYTES = 20 * 1024**3
MAX_ENTRIES = 4096
MAX_DEPTH = 3


class ImportError_(ValueError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_name(name: str) -> Path:
    path = PurePosixPath(name.replace("\\", "/"))
    if not name or path.is_absolute() or ".." in path.parts or any(":" in p for p in path.parts):
        raise ImportError_(f"Archive entry outside its folder: {name!r}")
    if not path.parts or "\x00" in name:
        raise ImportError_(f"Invalid archive entry: {name!r}")
    return Path(*path.parts)


class Budget:
    def __init__(self, maximum=MAX_BYTES):
        self.remaining = maximum
        self.entries = MAX_ENTRIES

    def entry(self, size):
        self.entries -= 1
        if size < 0 or size > self.remaining or self.entries < 0:
            raise ImportError_("Archive exceeds the import size or entry limit")

    def consume(self, size):
        self.remaining -= size
        if self.remaining < 0:
            raise ImportError_("Archive exceeds the import size limit")


def extract_zip(source, destination, budget):
    seen = set()
    with zipfile.ZipFile(source) as archive:
        for entry in archive.infolist():
            relative = safe_name(entry.filename)
            key = relative.as_posix().casefold()
            if key in seen:
                raise ImportError_(f"Duplicate archive path: {relative}")
            seen.add(key)
            mode = entry.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise ImportError_(f"Archive links are not accepted: {relative}")
            budget.entry(entry.file_size)
            target = destination / relative
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as stream, target.open("xb") as out:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    budget.consume(len(block))
                    out.write(block)


def libarchive():
    bundled = Path(getattr(sys, "_MEIPASS", "")) / "libarchive.so.13"
    name = str(bundled) if getattr(sys, "frozen", False) and bundled.is_file() else ctypes.util.find_library("archive")
    if not name:
        raise ImportError_("RAR import needs libarchive (Linux: libarchive; Ubuntu: libarchive13). "
                           "Alternatively unpack the RAR yourself and import its IMG/BIN file.")
    lib = ctypes.CDLL(name)
    declarations = {
        "archive_read_new": (ctypes.c_void_p, []),
        "archive_read_support_filter_all": (ctypes.c_int, [ctypes.c_void_p]),
        "archive_read_support_format_all": (ctypes.c_int, [ctypes.c_void_p]),
        "archive_read_open_filename": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]),
        "archive_read_next_header": (ctypes.c_int, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]),
        "archive_entry_pathname": (ctypes.c_char_p, [ctypes.c_void_p]),
        "archive_entry_size": (ctypes.c_longlong, [ctypes.c_void_p]),
        "archive_entry_filetype": (ctypes.c_uint, [ctypes.c_void_p]),
        "archive_entry_hardlink": (ctypes.c_char_p, [ctypes.c_void_p]),
        "archive_read_data": (ctypes.c_ssize_t, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]),
        "archive_error_string": (ctypes.c_char_p, [ctypes.c_void_p]),
        "archive_read_free": (ctypes.c_int, [ctypes.c_void_p]),
    }
    for name, (result, arguments) in declarations.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = result, arguments
    return lib


def extract_archive(source, destination, budget):
    lib = libarchive()
    archive = lib.archive_read_new()
    if not archive:
        raise ImportError_("Cannot allocate archive reader")
    def fail():
        message = lib.archive_error_string(archive)
        raise ImportError_(message.decode("utf-8", "replace") if message else "Cannot read RAR")
    try:
        lib.archive_read_support_filter_all(archive)
        lib.archive_read_support_format_all(archive)
        if lib.archive_read_open_filename(archive, os.fsencode(source), 65536) != 0:
            fail()
        entry = ctypes.c_void_p()
        buffer = ctypes.create_string_buffer(1024 * 1024)
        seen = set()
        while True:
            code = lib.archive_read_next_header(archive, ctypes.byref(entry))
            if code == 1:  # ARCHIVE_EOF
                break
            if code != 0:
                fail()
            name = lib.archive_entry_pathname(entry)
            if not name:
                raise ImportError_("Archive entry has no name")
            relative = safe_name(os.fsdecode(name))
            key = relative.as_posix().casefold()
            if key in seen:
                raise ImportError_(f"Duplicate archive path: {relative}")
            seen.add(key)
            kind = lib.archive_entry_filetype(entry)
            if kind not in (stat.S_IFREG, stat.S_IFDIR) or lib.archive_entry_hardlink(entry):
                raise ImportError_(f"Archive links or special files are not accepted: {relative}")
            budget.entry(lib.archive_entry_size(entry))
            target = destination / relative
            if kind == stat.S_IFDIR:
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as out:
                while True:
                    count = lib.archive_read_data(archive, buffer, len(buffer))
                    if count == 0:
                        break
                    if count < 0:
                        fail()
                    budget.consume(count)
                    out.write(buffer.raw[:count])
    finally:
        lib.archive_read_free(archive)


def candidates(source, work, budget, depth=0):
    if source.suffix.lower() == ".iso":
        yield source
        return
    if source.suffix.lower() not in (".zip", ".rar", ".7z"):
        raise ImportError_("Escolha ISO, ZIP, RAR ou 7Z do Ben 10: Ultimate Alien — Cosmic Destruction USA.")
    if depth >= MAX_DEPTH:
        raise ImportError_("Há arquivos compactados demais dentro de outros arquivos.")
    stage = Path(tempfile.mkdtemp(prefix="archive-", dir=work))
    (extract_zip if zipfile.is_zipfile(source) else extract_archive)(source, stage, budget)
    for path in sorted(stage.rglob("*")):
        if path.is_file() and path.suffix.lower() in (".iso", ".zip", ".rar", ".7z"):
            yield from candidates(path, work, budget, depth + 1)


EXPECTED = {"SLUS_219.38": (4698528, "f4d62e6475a2b168401f0e21a689a9b37252cf80e054deda1076a74729b01cde")}
REQUIRED = ("SYSTEM.CNF", "MODULES/FMODEX.IRX", "GLOBAL/GLOBAL.PS2", "L_TITL/L_TITL.PS2")


def inspect_disc(source):
    with Path(source).open("rb") as stream:
        try:
            files, layer1 = scan(stream)
        except (ValueError, AssertionError, IndexError) as exc:
            raise ImportError_("Imagem DVD ISO inválida ou truncada.") from exc
        entries = {name: (lba, size) for name, lba, size in files}
        for name, (size, digest) in EXPECTED.items():
            if name not in entries or entries[name][1] != size:
                raise ImportError_("É necessário Ben 10: Ultimate Alien — Cosmic Destruction USA SLUS-21938 v1.01.")
            stream.seek(entries[name][0]*2048)
            if hashlib.sha256(stream.read(size)).hexdigest() != digest:
                raise ImportError_("Executável incompatível. Use SLUS-21938 v1.01 sem modificações.")
        if any(name not in entries for name in REQUIRED):
            raise ImportError_("O disco está incompleto: faltam arquivos obrigatórios do jogo.")
        return {"serial": "SLUS-21938", "files": files, "layer1": layer1}


def extract_iso(image, destination):
    info = inspect_disc(image)
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with Path(image).open("rb") as stream:
        for name, lba, size in info["files"]:
            target = destination / safe_name(name)
            target.parent.mkdir(parents=True, exist_ok=True)
            stream.seek(lba*2048)
            with target.open("xb") as output:
                remaining = size
                while remaining:
                    block = stream.read(min(1024*1024, remaining))
                    if not block:
                        raise ImportError_("ISO truncada.")
                    output.write(block); remaining -= len(block)
        write_layout(stream, info["files"], info["layer1"], str(destination))
    (destination / ".import.json").write_text(json.dumps({"serial":info["serial"],"files":{n:s for n,_,s in info["files"]}})+"\n")


def import_disc(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".import-", dir=destination.parent) as temporary:
        work = Path(temporary)
        images = list(candidates(source, work, Budget()))
        if len(images) != 1:
            raise ImportError_(f"Esperado exatamente um disco; encontrados {len(images)}.")
        inspect_disc(images[0])
        extract_iso(images[0], work / "disc")
        if destination.exists():
            raise ImportError_("A pasta de destino já existe. Os dados foram preservados.")
        os.replace(work / "disc", destination)
    return destination / "SLUS_219.38"
