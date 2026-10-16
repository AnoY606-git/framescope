# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic fixtures for parser/policy tests; never executable test applications."""
import os
from contextlib import contextmanager
from pathlib import Path
import struct
import tempfile


@contextmanager
def temporary_directory():
    approved = Path(os.environ.get("LOCALAPPDATA", "")) / "Temp/opencode"
    with tempfile.TemporaryDirectory(prefix="framescope-tests-", dir=approved if approved.is_dir() else None) as directory:
        # Windows TEMP may contain an 8.3 ancestor (for example RUNNER~1).
        # Keep cleanup with the original owner, but give every test one canonical root.
        yield str(Path(directory).resolve())


def existing_directory_alias(path):
    """Use a real 8.3 alias if available, without changing volume settings."""
    path = Path(path).resolve(strict=True)
    if os.name == "nt":
        import ctypes
        get_short = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW
        get_short.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint)
        get_short.restype = ctypes.c_uint
        length = get_short(str(path), None, 0)
        if length:
            buffer = ctypes.create_unicode_buffer(length)
            written = get_short(str(path), buffer, length)
            if 0 < written < length:
                alias = Path(buffer.value)
                if alias != path and alias.samefile(path):
                    return alias
    # Still exercise physical identity and normalization when short names are unavailable.
    child = path / "existing-alias-segment"
    child.mkdir(exist_ok=True)
    return child / ".."


def synthetic_pe(imports=(), delay=(), machine=0x8664):
    data = bytearray(0x1000)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, machine, 1)
    struct.pack_into("<H", data, 0x94, 0xF0)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<I", data, optional + 60, 0x200)
    struct.pack_into("<I", data, optional + 108, 16)
    section = optional + 0xF0
    struct.pack_into("<IIII", data, section + 8, 0xE00, 0x1000, 0xE00, 0x200)
    string_offset = 0x800
    for directory, names, raw, size, name_offset in (
            (1, imports, 0x200, 20, 12), (13, delay, 0x400, 32, 4)):
        if not names:
            continue
        struct.pack_into("<II", data, optional + 112 + directory * 8,
                         raw - 0x200 + 0x1000, size * (len(names) + 1))
        for index, name in enumerate(names):
            entry = raw + index * size
            if directory == 13:
                struct.pack_into("<I", data, entry, 1)
            struct.pack_into("<I", data, entry + name_offset, string_offset - 0x200 + 0x1000)
            encoded = name.encode("ascii") + b"\0"
            data[string_offset:string_offset + len(encoded)] = encoded
            string_offset += len(encoded)
    return bytes(data)
