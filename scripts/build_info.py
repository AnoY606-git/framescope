#!/usr/bin/env python3
# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Write build provenance without personal paths or unearned validation claims."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
from release_support import artifact_paths, git_state, sha256, version


def repository_path(root: Path, path: Path) -> Path:
    return (path if path.is_absolute() else root / path).resolve()


def safe_relative(root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return "external"


def native_cache_info(root: Path, native_build: Path, configuration: str,
                      expected_output: Path | None = None) -> dict:
    cache_file = native_build / "CMakeCache.txt"
    cache = {}
    for line in cache_file.read_text(encoding="utf-8-sig").splitlines():
        match = re.fullmatch(r"([^#/:=]+):[^=]+=(.*)", line)
        if match:
            cache[match[1]] = match[2]
    if cache.get("CMAKE_BUILD_TYPE") != configuration or cache.get("CMAKE_CONFIGURATION_TYPES"):
        raise ValueError("Native cache does not describe the requested single build configuration")
    if cache.get("CMAKE_GENERATOR") != "Ninja":
        raise ValueError("Use a single-configuration Ninja native cache")
    if cache.get("CMAKE_HOME_DIRECTORY") and os.path.normcase(str(repository_path(
            root, Path(cache["CMAKE_HOME_DIRECTORY"])))) != os.path.normcase(str(root.resolve())):
        raise ValueError("Native cache belongs to another source checkout")
    output = cache.get("FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY")
    if not output or "$<" in output or "${" in output:
        raise ValueError("Native cache needs a literal FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY")
    output_path = repository_path(root, Path(output))
    if expected_output is not None and os.path.normcase(str(output_path)) != os.path.normcase(
            str(repository_path(root, expected_output))):
        raise ValueError("Native cache output does not match the selected artifact directory")
    opposite = artifact_paths(root, "Release" if configuration == "Debug" else "Debug")["native_build"]
    for path in (native_build.resolve(), output_path):
        if path == opposite or opposite in path.parents:
            raise ValueError("Debug and Release cannot reuse the other configuration native tree/output")
    return {"configuration": configuration, "cache_sha256": sha256(cache_file),
            "build_directory": safe_relative(root, native_build),
            "output_directory": safe_relative(root, output_path)}


def create_build_info(root: Path, publish: Path, native_build: Path, configuration: str,
                      sdk_version: str, native_output: Path | None = None) -> dict:
    native = native_cache_info(root, native_build, configuration, native_output)
    compiler = {}
    files = sorted((native_build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"),
                   key=lambda path: path.stat().st_mtime, reverse=True)
    if files:
        text = files[0].read_text(encoding="utf-8")
        for key in ("CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION"):
            match = re.search(r'set\(' + key + r' "?([^"\)]+)', text)
            if match:
                compiler[key] = match[1]
    native_manifest = json.loads((publish / "native/native-runtime.json").read_text(encoding="utf-8"))
    return {
        "version": version(root), "architecture": "x64", "configuration": configuration,
        "development_build": configuration == "Debug", "source": git_state(root),
        "dotnet_sdk": sdk_version, "compiler": compiler, "native_cache": native,
        "built_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "hardware_validation": "not implied by build", "signing": "not-claimed",
        "native_capabilities": native_manifest.get("capabilities", {}),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--publish-dir", type=Path)
    parser.add_argument("--native-build", type=Path)
    parser.add_argument("--native-output", type=Path)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--sdk-version", required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    paths = artifact_paths(root, args.configuration)
    publish = repository_path(root, args.publish_dir or paths["publish"])
    native_build = repository_path(root, args.native_build or paths["native_build"])
    try:
        info = create_build_info(root, publish, native_build, args.configuration, args.sdk_version, args.native_output)
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error)) from error
    publish.mkdir(parents=True, exist_ok=True)
    (publish / "build-info.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
