# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_runtime
from test_support import synthetic_pe, temporary_directory


def native_fixture(source):
    source.mkdir()
    for name in native_runtime.OWN_BINARIES:
        (source / name).write_bytes(synthetic_pe(("KERNEL32.dll",)))
    (source / "FrameScope.Vulkan.json").write_text(json.dumps({
        "file_format_version": "1.2.0", "layer": {"library_path": ".\\FrameScope.Vulkan.dll"},
    }), encoding="utf-8")


class NativeRuntimeTests(unittest.TestCase):
    def test_reads_normal_and_delay_imports(self):
        with temporary_directory() as directory:
            path = Path(directory) / "test.dll"
            path.write_bytes(synthetic_pe(("KERNEL32.dll",), ("runtime.dll",)))
            machine, imports = native_runtime.pe_imports(path)
            self.assertEqual(0x8664, machine)
            self.assertEqual({"KERNEL32.dll", "runtime.dll"}, set(imports))

    def test_copies_recursive_runtime_closure_not_system_or_vulkan_loader(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source, search, output = root / "source", root / "runtime", root / "output"
            native_fixture(source)
            search.mkdir()
            (source / "FrameScope.Hud.dll").write_bytes(synthetic_pe(("first.dll", "vulkan-1.dll")))
            (search / "first.dll").write_bytes(synthetic_pe(("second.dll",)))
            (search / "second.dll").write_bytes(synthetic_pe(("USER32.dll", "api-ms-win-core-test-l1-1-0.dll")))
            (search / "unused.dll").write_bytes(synthetic_pe())
            report = native_runtime.resolve_runtime(source, output, [search])
            self.assertEqual({"first.dll", "second.dll"}, {r["name"] for r in report["files"] if r["runtime"]})
            self.assertFalse((output / "unused.dll").exists())
            self.assertFalse((output / "USER32.dll").exists())
            self.assertEqual(["vulkan-1.dll"], report["platform_imports"])
            self.assertNotIn(str(search), json.dumps(report))

    def test_unresolved_dependency_does_not_produce_complete_manifest(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source, output = root / "source", root / "output"
            native_fixture(source)
            (source / "FrameScope.Hud.dll").write_bytes(synthetic_pe(("missing.dll",)))
            with self.assertRaisesRegex(ValueError, "Unresolved"):
                native_runtime.resolve_runtime(source, output, [])
            self.assertFalse((output / "native-runtime.json").exists())

    def test_gpu_driver_must_not_be_bundled(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source = root / "source"
            native_fixture(source)
            (source / "FrameScope.Hud.dll").write_bytes(synthetic_pe(("nvoglv64.dll",)))
            (source / "nvoglv64.dll").write_bytes(synthetic_pe())
            with self.assertRaisesRegex(ValueError, "driver DLL"):
                native_runtime.resolve_runtime(source, root / "output", [])

    def test_rejects_system_directory_as_runtime_search_source(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source = root / "source"
            native_fixture(source)
            system = root / "System32"
            system.mkdir()
            with self.assertRaisesRegex(ValueError, "system directories"):
                native_runtime.resolve_runtime(source, root / "output", [system])

    def test_manifest_cannot_load_an_absolute_or_parent_library(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source = root / "source"
            native_fixture(source)
            (source / "FrameScope.Vulkan.json").write_text('{"layer":{"library_path":"../other.dll"}}')
            with self.assertRaisesRegex(ValueError, "adjacent"):
                native_runtime.resolve_runtime(source, root / "output", [])

    def test_rejects_wrong_machine_truncated_and_unsafe_imports(self):
        with temporary_directory() as directory:
            path = Path(directory) / "bad.dll"
            for contents in (synthetic_pe(machine=0x14C), b"MZ", synthetic_pe(("../bad.dll",))):
                path.write_bytes(contents)
                with self.assertRaises(ValueError):
                    native_runtime.pe_imports(path)

    def test_rejects_unterminated_directory(self):
        with temporary_directory() as directory:
            path = Path(directory) / "bad.dll"
            data = bytearray(synthetic_pe(("KERNEL32.dll",)))
            struct.pack_into("<I", data, 0x98 + 112 + 8 + 4, 20)
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "Unterminated"):
                native_runtime.pe_imports(path)

    def test_dependency_cycle_terminates(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source = root / "source"
            native_fixture(source)
            (source / "FrameScope.Hud.dll").write_bytes(synthetic_pe(("cycle.dll",)))
            (source / "cycle.dll").write_bytes(synthetic_pe(("FrameScope.Hud.dll",)))
            result = native_runtime.resolve_runtime(source, root / "output", [])
            self.assertEqual(5, len(result["files"]))

    def test_optional_intel_helper_is_product_and_level_zero_loader_is_not_copied(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source, output = root / "source", root / "output"
            native_fixture(source)
            (source / "FrameScope.Intel.exe").write_bytes(synthetic_pe(("ze_loader.dll", "KERNEL32.dll")))
            (source / "ze_loader.dll").write_bytes(synthetic_pe())
            result = native_runtime.resolve_runtime(source, output, [])
            helper = next(record for record in result["files"] if record["name"] == "FrameScope.Intel.exe")
            self.assertFalse(helper["runtime"])
            self.assertEqual("product", helper["kind"])
            self.assertEqual({"intel_helper": True}, result["capabilities"])
            self.assertIn("ze_loader.dll", result["platform_imports"])
            self.assertFalse((output / "ze_loader.dll").exists())

    def test_optional_product_is_not_picked_up_from_compiler_search_directory(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source, search, output = root / "source", root / "search", root / "output"
            native_fixture(source)
            search.mkdir()
            (search / "FrameScope.Intel.exe").write_bytes(synthetic_pe())
            result = native_runtime.resolve_runtime(source, output, [search])
            self.assertEqual({"intel_helper": False}, result["capabilities"])
            self.assertFalse((output / "FrameScope.Intel.exe").exists())

    def test_microsoft_debug_crt_dll_is_never_redistributed(self):
        with temporary_directory() as directory:
            root = Path(directory)
            source = root / "source"
            native_fixture(source)
            (source / "FrameScope.Hud.dll").write_bytes(synthetic_pe(("ucrtbased.dll",)))
            (source / "ucrtbased.dll").write_bytes(synthetic_pe())
            with self.assertRaisesRegex(ValueError, "not redistributable"):
                native_runtime.resolve_runtime(source, root / "output", [])
            self.assertFalse((root / "output/ucrtbased.dll").exists())


if __name__ == "__main__":
    unittest.main()
