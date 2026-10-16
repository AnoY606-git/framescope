#!/usr/bin/env python3
# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Repository policy and deterministic source/package helpers, standard library only."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import stat
import xml.etree.ElementTree as ET
import zipfile


REQUIRED_FILES = (
    "README.md", "README.en.md", "LICENSE", "NOTICE", "AUTHORS.md", "ATTRIBUTION.md",
    "THIRD_PARTY_NOTICES.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md",
    ".editorconfig", ".gitattributes", ".gitignore", ".clang-format", "global.json",
    "Directory.Build.props", "VERSION", "CMakeLists.txt", ".github/workflows/ci.yml",
    "docs/ARCHITECTURE.md", "docs/BUILDING.md", "docs/TESTING.md", "docs/RELEASING.md",
    "docs/VERIFICATION.md", "docs/METRICS.md", "docs/COMPATIBILITY.md",
    "docs/USER_GUIDE.md", "docs/SECURITY.md", "docs/SIGNING.md", "packaging/FrameScope.iss",
    "docs/licenses/ImGui-LICENSE.txt", "docs/licenses/MinHook-LICENSE.txt",
    "docs/licenses/Vulkan-Headers-LICENSE.md", "docs/licenses/Apache-2.0.txt",
    "docs/licenses/Vulkan-Headers-MIT.txt", "docs/licenses/LLVM-exception.txt",
)
LEGAL_FILES = ("LICENSE", "NOTICE", "AUTHORS.md", "ATTRIBUTION.md", "THIRD_PARTY_NOTICES.md")


def configuration_suffix(configuration: str) -> str:
    if configuration not in {"Debug", "Release"}:
        raise ValueError("Configuration must be Debug or Release")
    return "-debug" if configuration == "Debug" else ""


def artifact_paths(root: Path, configuration: str) -> dict[str, Path]:
    suffix = configuration_suffix(configuration)
    artifacts = root.resolve() / "artifacts"
    return {"native_build": artifacts / ("native" + suffix),
            "publish": artifacts / ("publish" + suffix),
            "verification": artifacts / ("verification" + suffix + ".json"),
            "packages": artifacts / ("packages" + suffix),
            "installer": artifacts / ("installer" + suffix),
            "test_results": artifacts / ("test-results" + suffix)}


FORBIDDEN_DIRECTORIES = frozenset({
    ".git", ".agents", ".codex", ".claude", ".superpowers", ".worktrees",
    ".toolchain", ".tools", ".vs", ".idea", ".vscode", "plans", "specs",
    "artifacts", "build", "dist", "bin", "obj", "__pycache__", "_deps",
    "cmakefiles", "testing",
})
PRIVATE_SUFFIXES = frozenset({".key", ".pem", ".pfx", ".p12", ".snk", ".jks", ".keystore"})
OUTPUT_SUFFIXES = frozenset({
    ".exe", ".dll", ".pdb", ".obj", ".o", ".lib", ".a", ".ilk", ".exp",
    ".pyc", ".pyo", ".log", ".tmp", ".hprof", ".user", ".suo",
})
IMAGE_SUFFIXES = frozenset({".png", ".jpg", ".jpeg", ".ico"})


def is_reparse(path: Path) -> bool:
    """Reject Windows junctions as well as symbolic links, without following them."""
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.run(["git", "-C", os.fspath(root), *arguments], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def source_paths(root: Path, include_untracked: bool = False) -> list[str]:
    arguments = ["ls-files", "--cached", "-z"]
    paths = git(root, *arguments).split(b"\0")
    if include_untracked:
        paths += git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
    return sorted({os.fsdecode(path) for path in paths if path})


def path_error(name: str) -> str | None:
    parts = PurePosixPath(name).parts
    lower = tuple(part.lower() for part in parts)
    if (not parts or name.startswith(("/", "\\")) or "\\" in name
            or re.match(r"^[A-Za-z]:", name) or any(p in {".", "..", ""} for p in parts)):
        return "unsafe source path"
    filename = lower[-1]
    if set(lower[:-1]) & FORBIDDEN_DIRECTORIES:
        return "internal, toolchain or generated directory"
    if any(lower[i:i + 2] == ("docs", "internal") for i in range(len(lower) - 1)):
        return "internal documentation"
    if filename in {"agents.md", "claude.md", "gemini.md"}:
        return "internal agent instructions"
    if filename in {"local.settings.json", "local.properties", "keystore.properties",
                    "signing.properties", "id_rsa", "id_ed25519"}:
        return "machine configuration or credentials"
    if filename == ".env" or (filename.startswith(".env.") and filename != ".env.example"):
        return "environment credentials"
    suffix = PurePosixPath(filename).suffix
    if suffix in PRIVATE_SUFFIXES:
        return "private key or signing material"
    if suffix in OUTPUT_SUFFIXES:
        return "build output"
    return None


def scan_text(name: str, text: str) -> None:
    patterns = (
        r"-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----",
        r"\bgh[pousr]_[A-Za-z0-9]{20,}\b", r"\bgithub_pat_[A-Za-z0-9_]{20,}\b",
        r"\bAKIA[A-Z0-9]{16}\b",
    )
    if any(re.search(pattern, text) for pattern in patterns):
        raise ValueError(f"Possible credential (value hidden): {ascii(name)}")
    if re.search(r"(?:/(?:home|Users)/|[A-Za-z]:\\Users\\)[A-Za-z0-9_.-]+[/\\]", text):
        raise ValueError(f"Machine-specific home path: {ascii(name)}")


def version(root: Path) -> str:
    version_file = root / "VERSION"
    if version_file.is_file():
        value = version_file.read_text(encoding="utf-8-sig").strip()
        if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", value):
            raise ValueError("VERSION must contain one explicit semantic version")
        return value
    tree = ET.parse(root / "Directory.Build.props")
    values = [node.text.strip() for node in tree.findall(".//Version") if node.text]
    if len(values) != 1 or not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", values[0]):
        raise ValueError("Directory.Build.props must contain one explicit semantic Version")
    return values[0]


def audit_dependencies(root: Path, files: list[str]) -> None:
    sdk = json.loads((root / "global.json").read_text(encoding="utf-8-sig"))["sdk"]
    if not re.fullmatch(r"10\.\d+\.\d+", sdk.get("version", "")):
        raise ValueError("Pin an explicit .NET 10 SDK version in global.json")
    expected_version = version(root)
    for name in files:
        path = root / name
        if path.suffix in {".csproj", ".props"}:
            tree = ET.parse(path)
            for tag in ("PackageReference", "PackageVersion"):
                for node in tree.findall(f".//{tag}"):
                    value = node.get("Version") or node.findtext("Version")
                    if value is not None and not re.fullmatch(r"\d+\.\d+(?:\.\d+)?(?:-[0-9A-Za-z.-]+)?", value):
                        raise ValueError(f"Use an explicit package version: {ascii(name)}")
            if path.suffix == ".csproj":
                for node in tree.findall(".//Version"):
                    if node.text and node.text.strip() != expected_version:
                        raise ValueError(f"Project Version differs from Directory.Build.props: {ascii(name)}")
        if path.name == "CMakeLists.txt" or path.suffix == ".cmake":
            text = path.read_text(encoding="utf-8-sig")
            for match in re.finditer(r"\bGIT_TAG\s+([^\s)]+)", text):
                value = match[1].strip('"')
                if not (re.fullmatch(r"[0-9a-fA-F]{40}", value)
                        or re.fullmatch(r"v?\d+\.\d+(?:\.\d+)?[0-9A-Za-z._-]*", value)):
                    raise ValueError(f"Unpinned CMake git dependency: {ascii(name)}")
            for block in re.findall(r"FetchContent_Declare\s*\((.*?)\)", text, re.I | re.S):
                if re.search(r"\bURL\s+", block) and not re.search(r"\bURL_HASH\s+SHA(?:256|512)=[0-9a-fA-F]{64,128}\b", block):
                    raise ValueError(f"CMake archive dependency needs URL_HASH: {ascii(name)}")
        if name.startswith(".github/workflows/") and path.suffix in {".yml", ".yaml"}:
            for action in re.findall(r"\buses:\s*([^\s#]+)", path.read_text(encoding="utf-8-sig")):
                if not action.startswith("./") and not re.fullmatch(r"[\w.-]+/[\w./-]+@[0-9a-fA-F]{40}", action):
                    raise ValueError(f"Action must be pinned to a commit SHA: {ascii(name)}")


def validate_sources(root: Path, files: list[str], *, required: bool = True,
                     dependencies: bool = True) -> list[str]:
    if not files:
        raise ValueError("No source files selected; source archives only include Git-tracked files")
    if required:
        missing = sorted(set(REQUIRED_FILES) - set(files))
        if missing:
            raise ValueError("Missing required source/legal files: " + ", ".join(missing))
    root = root.resolve()
    for name in files:
        error = path_error(name)
        if error:
            raise ValueError(f"Forbidden source path ({error}): {ascii(name)}")
        path = root / name
        current = path
        while current != root:
            if is_reparse(current):
                raise ValueError(f"Source symlink is not allowed: {ascii(name)}")
            current = current.parent
        if not path.is_file():
            raise ValueError(f"Tracked file is missing: {ascii(name)}")
        if path.suffix.lower() not in IMAGE_SUFFIXES:
            try:
                text = path.read_text(encoding="utf-8-sig")
            except UnicodeError as error:
                raise ValueError(f"Unexpected binary source file: {ascii(name)}") from error
            if "\0" in text:
                raise ValueError(f"Unexpected NUL in source file: {ascii(name)}")
            scan_text(name, text)
    if dependencies:
        audit_dependencies(root, files)
    if required:
        license_text = (root / "LICENSE").read_text(encoding="utf-8")
        if "GNU GENERAL PUBLIC LICENSE" not in license_text or "Version 3, 29 June 2007" not in license_text:
            raise ValueError("Missing full GPL version 3 license text")
    return files


def git_state(root: Path) -> dict:
    try:
        commit = git(root, "rev-parse", "--verify", "HEAD").decode("ascii").strip()
    except subprocess.CalledProcessError:
        commit = None
    dirty = bool(git(root, "status", "--porcelain=v1", "--untracked-files=normal"))
    digest = hashlib.sha256()
    for name in source_paths(root, include_untracked=True):
        path = root / name
        if path_error(name) or not path.is_file() or is_reparse(path):
            continue
        digest.update(os.fsencode(name) + b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return {"commit": commit, "dirty": dirty, "tree_sha256": digest.hexdigest()}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_zip(destination: Path, entries: list[tuple[str, bytes, int]]) -> None:
    """Fixed timestamps/modes, stable order, atomic replacement, no stale directory scan."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            seen = set()
            for name, content, mode in sorted(entries):
                if (name.casefold() in seen or name.startswith(("/", "\\")) or "\\" in name
                        or re.match(r"^[A-Za-z]:", name) or ".." in PurePosixPath(name).parts):
                    raise ValueError("Duplicate or unsafe ZIP entry")
                seen.add(name.casefold())
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (0o100000 | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, content, compresslevel=9)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def source_archive(root: Path, files: list[str], destination: Path, metadata: dict) -> None:
    entries = []
    for name in files:
        if path_error(name) or is_reparse(root / name):
            raise ValueError("Unsafe source archive input")
        mode = 0o755 if name.endswith(".sh") else 0o644
        entries.append(("FrameScope/" + name, (root / name).read_bytes(), mode))
    entries.append(("FrameScope/SOURCE-INFO.json", json.dumps(metadata, indent=2,
                    sort_keys=True).encode("utf-8") + b"\n", 0o644))
    write_zip(destination, entries)


def write_checksums(destination: Path, files: list[Path]) -> None:
    names = [path.name for path in files]
    if len(names) != len(set(names)):
        raise ValueError("Duplicate artifact names")
    destination.write_text("".join(f"{sha256(path)}  {path.name}\n"
                                 for path in sorted(files)), encoding="utf-8", newline="\n")
