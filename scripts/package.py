#!/usr/bin/env python3
# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Package an explicitly selected publish directory and Git-tracked matching sources."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
from native_runtime import (DEBUG_CRT, DRIVER_NAME, OPTIONAL_PRODUCT_BINARIES, OWN_BINARIES,
                            PLATFORM_DLLS, is_system, pe_imports)
from release_support import (LEGAL_FILES, artifact_paths, configuration_suffix, git_state, is_reparse,
                             sha256, source_archive, source_paths, validate_sources, version,
                             write_checksums, write_zip)


def validate_publish(publish: Path, expected_version: str, configuration: str = "Release") -> dict:
    configuration_suffix(configuration)
    required = ["FrameScope.exe", "build-info.json", "native/native-runtime.json"]
    required += ["native/" + name for name in OWN_BINARIES]
    required += ["native/FrameScope.Vulkan.json"] + list(LEGAL_FILES)
    missing = [name for name in required if not (publish / name).is_file()]
    if missing:
        raise ValueError("Incomplete publish directory: " + ", ".join(missing))
    if (publish / "portable.txt").exists():
        raise ValueError("Publish directory must be installer-neutral; portable marker is added only to ZIP")
    info = json.loads((publish / "build-info.json").read_text(encoding="utf-8-sig"))
    if info.get("version") != expected_version or info.get("architecture") != "x64":
        raise ValueError("Publish metadata version/architecture does not match these sources")
    if info.get("configuration") != configuration or info.get("native_cache", {}).get("configuration") != configuration:
        raise ValueError("Publish/native configuration does not match the requested package")
    if info.get("development_build") is not (configuration == "Debug"):
        raise ValueError("Publish development marker does not match the build configuration")
    pe_imports(publish / "FrameScope.exe")
    native = publish / "native"
    report = json.loads((native / "native-runtime.json").read_text(encoding="utf-8"))
    capabilities = report.get("capabilities", {})
    has_intel = (native / "FrameScope.Intel.exe").is_file()
    if bool(capabilities.get("intel_helper")) != has_intel:
        raise ValueError("Intel helper capability does not match the packaged product executable")
    if "native_capabilities" in info and info["native_capabilities"] != capabilities:
        raise ValueError("Build metadata does not match native package capabilities")
    products = {name.lower() for name in OWN_BINARIES + OPTIONAL_PRODUCT_BINARIES}
    names = [record["name"].lower() for record in report["files"]]
    if len(names) != len(set(names)):
        raise ValueError("Duplicate native runtime record")
    for record in report["files"]:
        name = record["name"]
        if Path(name).name != name or DRIVER_NAME.fullmatch(name):
            raise ValueError("Unsafe or driver DLL in native runtime manifest")
        if DEBUG_CRT.fullmatch(name):
            raise ValueError("Microsoft debug CRT DLL is not redistributable: " + name)
        if name.lower() in PLATFORM_DLLS or is_system(name):
            raise ValueError("Platform/system DLL must not be bundled: " + name)
        if bool(record.get("runtime")) != (name.lower() not in products):
            raise ValueError("Native product/runtime classification mismatch: " + name)
        if sha256(native / name) != record["sha256"]:
            raise ValueError("Native runtime hash mismatch: " + name)
        _, imports = pe_imports(native / name)
        for imported in imports:
            if not is_system(imported) and imported.lower() not in PLATFORM_DLLS and imported.lower() not in names:
                raise ValueError("Incomplete native runtime closure: " + imported)
    expected = {record["name"].lower() for record in report["files"]}
    if expected != {path.name.lower() for path in native.iterdir()
                    if path.suffix.lower() in {".dll", ".exe"}}:
        raise ValueError("Unaccounted native PE file in publish directory")
    if any(record.get("runtime") for record in report["files"]):
        notices = publish / "licenses/native-runtime"
        if not notices.is_dir() or not any(path.is_file() for path in notices.rglob("*")):
            raise ValueError("Dynamic native runtime requires its redistribution license/notices")
    for runtime in ("microsoft.netcore.app.runtime.win-x64", "microsoft.windowsdesktop.app.runtime.win-x64"):
        notices = publish / "licenses/dotnet" / runtime
        if not notices.is_dir() or not any(path.is_file() for path in notices.rglob("*")):
            raise ValueError("Missing exact .NET runtime package notices: " + runtime)
    return info


def validate_verification(publish: Path, verification: Path, info: dict, configuration: str) -> None:
    if not verification.is_file():
        raise ValueError("Matching configuration verification marker is missing; run verify.ps1")
    marker = json.loads(verification.read_text(encoding="utf-8-sig"))
    if (marker.get("configuration") != configuration or marker.get("version") != info["version"]
            or marker.get("source") != info.get("source")
            or marker.get("native_cache") != info.get("native_cache")
            or marker.get("build_info_sha256") != sha256(publish / "build-info.json")):
        raise ValueError("Verification marker does not match this exact configuration/source/build metadata")


def package(root: Path, publish: Path, output: Path, *, installer: Path | None = None,
            allow_dirty_source: bool = False, configuration: str = "Release",
            verification: Path | None = None) -> list[Path]:
    debug_suffix = configuration_suffix(configuration)
    release_version = version(root)
    files = validate_sources(root, source_paths(root))
    state = git_state(root)
    if (state["dirty"] or not state["commit"]) and not allow_dirty_source:
        raise ValueError("Packaging needs a clean committed checkout; use --allow-dirty-source only for local development")
    info = validate_publish(publish, release_version, configuration)
    if info.get("source") != state:
        raise ValueError("Publish source state differs from this checkout; rebuild before packaging")
    validate_verification(publish, verification or artifact_paths(root, configuration)["verification"], info, configuration)
    for name in LEGAL_FILES:
        if (publish / name).read_bytes() != (root / name).read_bytes():
            raise ValueError("Published legal file differs from source: " + name)
    output.mkdir(parents=True, exist_ok=True)
    suffix = "-dev" if state["dirty"] or not state["commit"] else ""
    stem = f"FrameScope-{release_version}{debug_suffix}{suffix}"
    entries = []
    for path in sorted(publish.rglob("*")):
        if is_reparse(path):
            raise ValueError("Publish symlinks are not allowed")
        if path.is_file():
            entries.append((path.relative_to(publish).as_posix(), path.read_bytes(), 0o644))
    entries.append(("portable.txt", b"FrameScope portable settings mode.\n", 0o644))
    portable = output / f"{stem}-win-x64-portable.zip"
    write_zip(portable, entries)
    source = output / f"{stem}-source.zip"
    source_archive(root, files, source, {"version": release_version, "source": state,
                                      "configuration": configuration,
                                      "tracked_files_only": True})
    artifacts = [portable, source]
    if installer is not None:
        if not installer.is_file():
            raise ValueError("Explicit installer path does not exist")
        # Inno Setup's bootstrap can be x86 while installing x64 files.
        with installer.open("rb") as stream:
            header = stream.read(64)
            if len(header) < 64 or header[:2] != b"MZ":
                raise ValueError("Installer is not a PE executable")
            offset, = struct.unpack_from("<I", header, 0x3C)
            stream.seek(offset)
            if stream.read(4) != b"PE\0\0":
                raise ValueError("Installer has no PE signature")
        target = output / f"{stem}-win-x64-setup.exe"
        if target.resolve() != installer.resolve():
            shutil.copy2(installer, target)
        artifacts.append(target)
    manifest = output / f"{stem}-release.json"
    manifest.write_text(json.dumps({
        "version": release_version, "source": state, "architecture": "x64",
        "configuration": configuration, "development_build": configuration == "Debug",
        "native_capabilities": info.get("native_capabilities", {}),
        "signing": "not-claimed", "hardware_validation": "see docs/VERIFICATION.md",
        "artifacts": [{"name": path.name, "sha256": sha256(path)} for path in artifacts],
    }, indent=2) + "\n", encoding="utf-8")
    artifacts.append(manifest)
    write_checksums(output / "SHA256SUMS", artifacts)
    return artifacts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--publish-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--verification", type=Path)
    parser.add_argument("--check-publish", action="store_true")
    parser.add_argument("--installer", type=Path)
    parser.add_argument("--allow-dirty-source", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    paths = artifact_paths(root, args.configuration)
    publish = (root / args.publish_dir).resolve() if args.publish_dir else paths["publish"]
    output = (root / args.output_dir).resolve() if args.output_dir else paths["packages"]
    verification = (root / args.verification).resolve() if args.verification else paths["verification"]
    try:
        if args.check_publish:
            info = validate_publish(publish, version(root), args.configuration)
            if info.get("source") != git_state(root):
                raise ValueError("Publish does not match current source state; rebuild before verification")
            if args.verification:
                validate_verification(publish, verification, info, args.configuration)
            print("Publish/runtime audit passed; source fingerprint matches")
            return
        installer = (root / args.installer).resolve() if args.installer else None
        artifacts = package(root, publish, output, installer=installer,
                            allow_dirty_source=args.allow_dirty_source, configuration=args.configuration,
                            verification=verification)
    except subprocess.CalledProcessError as error:
        raise SystemExit("Unable to read Git source state") from error
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error)) from error
    print("Packaged only the current invocation's artifacts:")
    for path in artifacts:
        print(path.name)


if __name__ == "__main__":
    main()
