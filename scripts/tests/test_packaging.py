# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_runtime
import package as package_tool
import release_support as support
from test_native_runtime import native_fixture
from test_support import synthetic_pe, temporary_directory


def verification_fixture(root, publish):
    info = json.loads((publish / "build-info.json").read_text())
    marker = support.artifact_paths(root, info["configuration"])["verification"]
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps({"version": info["version"], "source": info["source"],
                                 "configuration": info["configuration"], "native_cache": info["native_cache"],
                                 "build_info_sha256": support.sha256(publish / "build-info.json")}), encoding="utf-8")
    return marker


def publish_fixture(root, configuration="Release"):
    publish = root / ("publish-debug" if configuration == "Debug" else "publish")
    publish.mkdir()
    native = root / "native-source"
    native_fixture(native)
    native_runtime.resolve_runtime(native, publish / "native", [])
    (publish / "FrameScope.exe").write_bytes(synthetic_pe())
    state = {"commit": "a" * 40, "dirty": False, "tree_sha256": "b" * 64}
    info = {"version": "0.1.0-preview", "architecture": "x64", "source": state,
            "configuration": configuration, "development_build": configuration == "Debug",
            "native_cache": {"configuration": configuration, "cache_sha256": "c" * 64}}
    (publish / "build-info.json").write_text(json.dumps(info), encoding="utf-8")
    for name in support.LEGAL_FILES:
        (root / name).write_text("Synthetic legal fixture: " + name, encoding="utf-8")
        (publish / name).write_bytes((root / name).read_bytes())
    for runtime in ("microsoft.netcore.app.runtime.win-x64", "microsoft.windowsdesktop.app.runtime.win-x64"):
        notices = publish / "licenses/dotnet" / runtime
        notices.mkdir(parents=True)
        (notices / "LICENSE.TXT").write_text("Synthetic license fixture")
    (root / "Directory.Build.props").write_text('<Project><PropertyGroup><Version>0.1.0-preview</Version></PropertyGroup></Project>')
    verification_fixture(root, publish)
    return publish, state


class PackagingTests(unittest.TestCase):
    def test_source_archive_is_repeatable_and_only_uses_explicit_selected_files(self):
        with temporary_directory() as directory:
            root = Path(directory)
            (root / "source.cs").write_text("// own source", encoding="utf-8")
            (root / "untracked.txt").write_text("must not ship", encoding="utf-8")
            first, second = root / "one.zip", root / "two.zip"
            for output in (first, second):
                support.source_archive(root, ["source.cs"], output, {"version": "test"})
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                self.assertEqual({"FrameScope/source.cs", "FrameScope/SOURCE-INFO.json"}, set(archive.namelist()))

    def test_portable_marker_is_only_in_zip_and_stale_output_is_not_republished(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, state = publish_fixture(root)
            output = root / "packages"
            output.mkdir()
            (output / "stale-setup.exe").write_bytes(b"stale")
            selected = list(support.LEGAL_FILES) + ["Directory.Build.props"]
            with patch.object(package_tool, "source_paths", return_value=selected), \
                    patch.object(package_tool, "validate_sources", side_effect=lambda _root, files: files), \
                    patch.object(package_tool, "git_state", return_value=state):
                artifacts = package_tool.package(root, publish, output)
            self.assertFalse((publish / "portable.txt").exists())
            with zipfile.ZipFile(artifacts[0]) as archive:
                self.assertIn("portable.txt", archive.namelist())
                self.assertIn("native/FrameScope.Hud.dll", archive.namelist())
            sums = (output / "SHA256SUMS").read_text()
            self.assertNotIn("stale", sums)
            self.assertEqual(len(artifacts), len(sums.splitlines()))

    def test_dirty_checkout_requires_explicit_dev_mode_and_dev_names(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, state = publish_fixture(root)
            state["dirty"] = True
            info = json.loads((publish / "build-info.json").read_text())
            info["source"] = state
            (publish / "build-info.json").write_text(json.dumps(info))
            verification_fixture(root, publish)
            selected = list(support.LEGAL_FILES)
            with patch.object(package_tool, "source_paths", return_value=selected), \
                    patch.object(package_tool, "validate_sources", side_effect=lambda _root, files: files), \
                    patch.object(package_tool, "git_state", return_value=state):
                with self.assertRaisesRegex(ValueError, "clean committed"):
                    package_tool.package(root, publish, root / "out")
                artifacts = package_tool.package(root, publish, root / "out", allow_dirty_source=True)
                self.assertTrue(all("-dev-" in path.name for path in artifacts))

    def test_mutated_native_binary_is_rejected(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, _ = publish_fixture(root)
            (publish / "native/FrameScope.Hud.dll").write_bytes(synthetic_pe(("USER32.dll",)))
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                package_tool.validate_publish(publish, "0.1.0-preview")

    def test_dynamic_native_runtime_without_redistribution_notices_is_rejected(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, _ = publish_fixture(root)
            native = publish / "native"
            (native / "extra-runtime.dll").write_bytes(synthetic_pe())
            report = json.loads((native / "native-runtime.json").read_text())
            report["files"].append({"name": "extra-runtime.dll", "runtime": True,
                                    "sha256": support.sha256(native / "extra-runtime.dll")})
            (native / "native-runtime.json").write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, "redistribution license"):
                package_tool.validate_publish(publish, "0.1.0-preview")

    def test_packaged_intel_helper_is_allowed_product_without_runtime_license_requirement(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, _ = publish_fixture(root)
            native = publish / "native"
            helper = native / "FrameScope.Intel.exe"
            helper.write_bytes(synthetic_pe(("ze_loader.dll",)))
            report = json.loads((native / "native-runtime.json").read_text())
            report["capabilities"] = {"intel_helper": True}
            report["files"].append({"name": helper.name, "runtime": False, "kind": "product",
                                    "sha256": support.sha256(helper)})
            (native / "native-runtime.json").write_text(json.dumps(report))
            package_tool.validate_publish(publish, "0.1.0-preview")
            helper.unlink()
            with self.assertRaisesRegex(ValueError, "capability"):
                package_tool.validate_publish(publish, "0.1.0-preview")

    def test_driver_provided_level_zero_loader_is_rejected_even_with_valid_manifest_hash(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, _ = publish_fixture(root)
            native = publish / "native"
            loader = native / "ze_loader.dll"
            loader.write_bytes(synthetic_pe())
            report = json.loads((native / "native-runtime.json").read_text())
            report["files"].append({"name": loader.name, "runtime": True,
                                    "sha256": support.sha256(loader)})
            (native / "native-runtime.json").write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, "Platform/system DLL"):
                package_tool.validate_publish(publish, "0.1.0-preview")

    def test_version_mismatch_and_installer_portable_mix_are_rejected(self):
        with temporary_directory() as directory:
            publish, _ = publish_fixture(Path(directory))
            with self.assertRaisesRegex(ValueError, "version/architecture"):
                package_tool.validate_publish(publish, "9.0.0")
            (publish / "portable.txt").write_text("marker")
            with self.assertRaisesRegex(ValueError, "installer-neutral"):
                package_tool.validate_publish(publish, "0.1.0-preview")

    def test_checksum_manifest_contains_only_explicit_current_files(self):
        with temporary_directory() as directory:
            root = Path(directory)
            current = root / "current.zip"
            current.write_bytes(b"current")
            (root / "old.exe").write_bytes(b"old")
            support.write_checksums(root / "SHA256SUMS", [current])
            self.assertEqual(f"{support.sha256(current)}  current.zip\n", (root / "SHA256SUMS").read_text())

    def test_unsafe_zip_paths_fail(self):
        with temporary_directory() as directory:
            for name in ("../outside", "C:/outside", "folder\\outside"):
                with self.assertRaisesRegex(ValueError, "unsafe ZIP"):
                    support.write_zip(Path(directory) / "bad.zip", [(name, b"test", 0o644)])

    def test_debug_package_is_explicitly_named_and_never_touches_release_packages(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, state = publish_fixture(root, "Debug")
            release_output = support.artifact_paths(root, "Release")["packages"]
            release_output.mkdir(parents=True)
            (release_output / "keep.txt").write_text("release output")
            output = support.artifact_paths(root, "Debug")["packages"]
            with patch.object(package_tool, "source_paths", return_value=list(support.LEGAL_FILES)), \
                    patch.object(package_tool, "validate_sources", side_effect=lambda _root, files: files), \
                    patch.object(package_tool, "git_state", return_value=state):
                artifacts = package_tool.package(root, publish, output, configuration="Debug")
            self.assertTrue(all("-debug-" in path.name for path in artifacts))
            manifest = json.loads(next(path for path in artifacts if path.name.endswith("-release.json")).read_text())
            self.assertEqual("Debug", manifest["configuration"])
            self.assertTrue(manifest["development_build"])
            self.assertEqual("not-claimed", manifest["signing"])
            self.assertEqual("release output", (release_output / "keep.txt").read_text())

    def test_debug_publish_cannot_be_packaged_as_release(self):
        with temporary_directory() as directory:
            publish, _ = publish_fixture(Path(directory), "Debug")
            with self.assertRaisesRegex(ValueError, "configuration"):
                package_tool.validate_publish(publish, "0.1.0-preview", "Release")

    def test_marker_from_wrong_configuration_or_changed_build_info_is_rejected(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, _ = publish_fixture(root, "Debug")
            info = package_tool.validate_publish(publish, "0.1.0-preview", "Debug")
            marker = verification_fixture(root, publish)
            record = json.loads(marker.read_text())
            record["configuration"] = "Release"
            marker.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "Verification marker"):
                package_tool.validate_verification(publish, marker, info, "Debug")
            marker = verification_fixture(root, publish)
            info["dotnet_sdk"] = "different SDK, same source/configuration"
            (publish / "build-info.json").write_text(json.dumps(info))
            with self.assertRaisesRegex(ValueError, "Verification marker"):
                package_tool.validate_verification(publish, marker, info, "Debug")

    def test_debug_does_not_implicitly_allow_dirty_sources(self):
        with temporary_directory() as directory:
            root = Path(directory)
            publish, state = publish_fixture(root, "Debug")
            state["dirty"] = True
            info = json.loads((publish / "build-info.json").read_text())
            info["source"] = state
            (publish / "build-info.json").write_text(json.dumps(info))
            verification_fixture(root, publish)
            with patch.object(package_tool, "source_paths", return_value=list(support.LEGAL_FILES)), \
                    patch.object(package_tool, "validate_sources", side_effect=lambda _root, files: files), \
                    patch.object(package_tool, "git_state", return_value=state):
                with self.assertRaisesRegex(ValueError, "clean committed"):
                    package_tool.package(root, publish, root / "out", configuration="Debug")
                artifacts = package_tool.package(root, publish, root / "out", configuration="Debug", allow_dirty_source=True)
                self.assertTrue(all("-debug-dev-" in path.name for path in artifacts))


if __name__ == "__main__":
    unittest.main()
