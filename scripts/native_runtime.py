#!/usr/bin/env python3
# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Resolve x64 PE import closures without copying Windows or GPU driver DLLs."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct


OWN_BINARIES = (
    "FrameScope.Launcher.exe", "FrameScope.Hud.dll", "FrameScope.Vulkan.dll",
    "FrameScope.Demo.exe",
)
OPTIONAL_PRODUCT_BINARIES = ("FrameScope.Intel.exe",)
SYSTEM_DLLS = frozenset(name.lower() for name in (
    "advapi32.dll", "avrt.dll", "bcrypt.dll", "bcryptprimitives.dll", "cabinet.dll",
    "cfgmgr32.dll", "combase.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll",
    "cryptbase.dll", "cryptsp.dll", "d2d1.dll", "d3d11.dll", "d3d12.dll",
    "d3dcompiler_47.dll", "dcomp.dll", "dbghelp.dll", "dnsapi.dll", "dwmapi.dll",
    "dwrite.dll", "dxcore.dll", "dxgi.dll", "gdi32.dll", "gdi32full.dll",
    "hid.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll", "kernelbase.dll",
    "mpr.dll", "msimg32.dll", "msvcrt.dll", "ncrypt.dll", "netapi32.dll",
    "normaliz.dll", "ntdll.dll", "ole32.dll", "oleacc.dll", "oleaut32.dll",
    "pdh.dll", "powrprof.dll", "profapi.dll", "propsys.dll", "psapi.dll",
    "rpcrt4.dll", "secur32.dll", "setupapi.dll", "shell32.dll", "shlwapi.dll",
    "sspicli.dll", "ucrtbase.dll", "user32.dll", "userenv.dll", "usp10.dll",
    "uxtheme.dll", "version.dll", "win32u.dll", "windowscodecs.dll", "winhttp.dll",
    "wininet.dll", "winmm.dll", "winspool.drv", "wintrust.dll", "wldap32.dll",
    "ws2_32.dll", "wtsapi32.dll",
))
# Loaders come from the installed graphics runtime, never from the packaged toolchain.
PLATFORM_DLLS = frozenset({"vulkan-1.dll", "ze_loader.dll"})
DRIVER_NAME = re.compile(r"^(?:nv|amd|ati|igd|igc|igfx|ze_intel)[a-z0-9_.-]*\.dll$", re.I)
DEBUG_CRT = re.compile(r"^(?:ucrtbased|(?:vcruntime|msvcp|msvcr|concrt)\d+(?:_\d+)*d(?:_[a-z0-9_]+)?)\.dll$", re.I)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pe_imports(path: Path) -> tuple[int, list[str]]:
    """Read normal and RVA-based delay imports with bounded, validated offsets."""
    data = path.read_bytes()

    def unpack(fmt: str, offset: int):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError(f"Truncated PE: {path.name}")
        return struct.unpack_from(fmt, data, offset)

    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"Not a PE file: {path.name}")
    pe, = unpack("<I", 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path.name}")
    machine, sections = unpack("<HH", pe + 4)
    optional_size, = unpack("<H", pe + 20)
    optional = pe + 24
    magic, = unpack("<H", optional)
    if magic != 0x20B or machine != 0x8664:
        raise ValueError(f"Expected x64 PE32+ image: {path.name}")
    if not 0 < sections <= 96 or optional_size < 112:
        raise ValueError(f"Invalid PE headers: {path.name}")
    directory_count, = unpack("<I", optional + 108)
    header_size, = unpack("<I", optional + 60)
    if optional + optional_size + sections * 40 > len(data):
        raise ValueError(f"Truncated PE section table: {path.name}")
    ranges = []
    for index in range(sections):
        start = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw = unpack("<IIII", start + 8)
        ranges.append((rva, virtual_size, raw_size, raw))

    def offset_of(rva: int, count: int = 1) -> int:
        if rva < header_size and rva + count <= min(header_size, len(data)):
            return rva
        for start, virtual_size, raw_size, raw in ranges:
            if start <= rva < start + max(virtual_size, raw_size):
                delta = rva - start
                if delta + count <= raw_size and raw + delta + count <= len(data):
                    return raw + delta
        raise ValueError(f"Invalid PE RVA: {path.name}")

    def directory(index: int) -> tuple[int, int]:
        if index >= directory_count or 112 + (index + 1) * 8 > optional_size:
            return 0, 0
        return unpack("<II", optional + 112 + index * 8)

    def dll_name(rva: int) -> str:
        offset = offset_of(rva)
        end = data.find(b"\0", offset, min(offset + 261, len(data)))
        if end < 0:
            raise ValueError(f"Unterminated PE import: {path.name}")
        name = data[offset:end].decode("ascii")
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.(?:dll|drv)", name, re.I):
            raise ValueError(f"Unsafe PE import name: {path.name}")
        return name

    imports = set()
    for index, descriptor_size, name_offset in ((1, 20, 12), (13, 32, 4)):
        rva, size = directory(index)
        if not rva:
            continue
        if size < descriptor_size or size // descriptor_size > 4096:
            raise ValueError(f"Invalid PE import directory: {path.name}")
        terminated = False
        for position in range(size // descriptor_size):
            offset = offset_of(rva + position * descriptor_size, descriptor_size)
            descriptor = data[offset:offset + descriptor_size]
            if not any(descriptor):
                terminated = True
                break
            if index == 13 and unpack("<I", offset)[0] != 1:
                raise ValueError(f"Unsupported VA-based delay import: {path.name}")
            name_rva, = unpack("<I", offset + name_offset)
            imports.add(dll_name(name_rva))
        if not terminated:
            raise ValueError(f"Unterminated PE import directory: {path.name}")
    return machine, sorted(imports, key=str.lower)


def is_system(name: str) -> bool:
    lower = name.lower()
    return lower in SYSTEM_DLLS or lower.startswith(("api-ms-win-", "ext-ms-win-"))


def resolve_runtime(source: Path, destination: Path, search: list[Path]) -> dict:
    """Resolve before copying: missing dependencies never yield a claimed complete set."""
    source, destination = source.resolve(), destination.resolve()
    directories = [source] + [path.resolve() for path in search]
    for directory in directories:
        if not directory.is_dir():
            raise ValueError("A native runtime search directory does not exist")
        if {part.lower() for part in directory.parts} & {"system32", "syswow64", "driverstore"}:
            raise ValueError("Do not use Windows driver/system directories as runtime sources")
    indexes = []
    for directory in directories:
        indexes.append({p.name.lower(): p for p in directory.iterdir() if p.is_file()})
    resolved: dict[str, Path] = {}
    optional_products = [name for name in OPTIONAL_PRODUCT_BINARIES if name.lower() in indexes[0]]
    queue = list(OWN_BINARIES) + optional_products
    system, platform = set(), set()
    while queue:
        name = queue.pop(0)
        lower = name.lower()
        if lower in resolved:
            continue
        if DRIVER_NAME.fullmatch(name):
            raise ValueError(f"GPU driver DLL must not be bundled: {name}")
        if DEBUG_CRT.fullmatch(name):
            raise ValueError(f"Microsoft debug CRT DLL is not redistributable: {name}")
        path = next((index[lower] for index in indexes if lower in index), None)
        if path is None:
            raise ValueError(f"Unresolved native dependency: {name}")
        if path.is_symlink():
            raise ValueError(f"Symlink runtime is not allowed: {name}")
        _, imports = pe_imports(path)
        resolved[lower] = path
        for imported in imports:
            if is_system(imported):
                system.add(imported.lower())
            elif imported.lower() in PLATFORM_DLLS:
                platform.add(imported.lower())
            else:
                queue.append(imported)
    manifest = source / "FrameScope.Vulkan.json"
    if not manifest.is_file():
        raise ValueError("Missing FrameScope.Vulkan.json")
    layer = json.loads(manifest.read_text(encoding="utf-8-sig"))["layer"]
    library_path = layer.get("library_path", "").replace("\\", "/")
    if library_path not in {"FrameScope.Vulkan.dll", "./FrameScope.Vulkan.dll"}:
        raise ValueError("Vulkan manifest must refer to its adjacent FrameScope.Vulkan.dll")
    destination.mkdir(parents=True, exist_ok=True)
    records = []
    own = {name.lower() for name in OWN_BINARIES + OPTIONAL_PRODUCT_BINARIES}
    for name, path in sorted(resolved.items()):
        target = destination / path.name
        if target.resolve() != path:
            if target.exists() and sha256(target) != sha256(path):
                raise ValueError(f"Conflicting runtime destination: {target.name}")
            shutil.copy2(path, target)
        records.append({"name": path.name, "sha256": sha256(target), "runtime": name not in own,
                        "kind": "product" if name in own else "runtime"})
    if manifest.resolve() != (destination / manifest.name).resolve():
        shutil.copy2(manifest, destination / manifest.name)
    report = {"architecture": "x64", "files": records,
              "system_imports": sorted(system), "platform_imports": sorted(platform),
              "capabilities": {"intel_helper": bool(optional_products)}}
    (destination / "native-runtime.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--search", type=Path, action="append", default=[])
    args = parser.parse_args()
    try:
        report = resolve_runtime(args.source, args.destination, args.search)
    except (OSError, ValueError, KeyError, UnicodeError, struct.error) as error:
        raise SystemExit(str(error)) from error
    print(f"Native closure: {len(report['files'])} x64 PE files; system/driver DLLs not copied")


if __name__ == "__main__":
    main()
