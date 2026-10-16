# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Configuration/provenance tests use synthetic caches, never run a compiler or renderer."""
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_info
import release_support as support
from test_support import temporary_directory


def cache_fixture(root, configuration="Debug", build=None, output=None):
    build = build or support.artifact_paths(root, configuration)["native_build"]
    output = output or build / "bin"
    build.mkdir(parents=True)
    (build / "CMakeCache.txt").write_text(
        f"CMAKE_BUILD_TYPE:STRING={configuration}\nCMAKE_GENERATOR:INTERNAL=Ninja\n"
        f"CMAKE_HOME_DIRECTORY:INTERNAL={root.as_posix()}\n"
        f"FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY:PATH={output.as_posix()}\n", encoding="utf-8")
    return build, output


class BuildInfoTests(unittest.TestCase):
    def test_configuration_defaults_are_isolated_and_release_is_backward_compatible(self):
        with temporary_directory() as directory:
            root = Path(directory)
            release, debug = support.artifact_paths(root, "Release"), support.artifact_paths(root, "Debug")
            self.assertEqual(root / "artifacts/native", release["native_build"])
            self.assertEqual(root / "artifacts/publish", release["publish"])
            self.assertEqual(root / "artifacts/packages", release["packages"])
            self.assertEqual(root / "artifacts/verification.json", release["verification"])
            self.assertEqual(root / "artifacts/native-debug", debug["native_build"])
            self.assertEqual(root / "artifacts/publish-debug", debug["publish"])
            self.assertEqual(root / "artifacts/packages-debug", debug["packages"])
            self.assertEqual(root / "artifacts/verification-debug.json", debug["verification"])
            self.assertFalse(set(release.values()) & set(debug.values()))

    def test_custom_debug_native_cache_records_actual_output_without_personal_paths(self):
        with temporary_directory() as directory:
            root = Path(directory).resolve()
            build, output = cache_fixture(root, build=root / "artifacts/build-debug", output=root / "artifacts/debug-native/bin")
            native = build_info.native_cache_info(root, build, "Debug", output)
            self.assertEqual("Debug", native["configuration"])
            self.assertEqual("artifacts/build-debug", native["build_directory"])
            self.assertEqual("artifacts/debug-native/bin", native["output_directory"])
            self.assertEqual(support.sha256(build / "CMakeCache.txt"), native["cache_sha256"])
            self.assertNotIn(str(root), json.dumps(native))

    def test_wrong_cache_configuration_and_output_are_not_relabelled(self):
        with temporary_directory() as directory:
            root = Path(directory)
            build, output = cache_fixture(root)
            with self.assertRaisesRegex(ValueError, "configuration"):
                build_info.native_cache_info(root, build, "Release", output)
            with self.assertRaisesRegex(ValueError, "output"):
                build_info.native_cache_info(root, build, "Debug", root / "wrong/bin")

    def test_debug_cannot_use_release_native_output(self):
        with temporary_directory() as directory:
            root = Path(directory)
            build, output = cache_fixture(root, output=root / "artifacts/native/bin")
            with self.assertRaisesRegex(ValueError, "cannot reuse"):
                build_info.native_cache_info(root, build, "Debug", output)

    def test_debug_metadata_keeps_real_source_state_and_makes_no_signing_claim(self):
        with temporary_directory() as directory:
            root = Path(directory)
            (root / "VERSION").write_text("0.1.0-preview")
            build, output = cache_fixture(root)
            publish = support.artifact_paths(root, "Debug")["publish"]
            (publish / "native").mkdir(parents=True)
            (publish / "native/native-runtime.json").write_text('{"capabilities":{"intel_helper":false}}')
            state = {"commit": None, "dirty": True, "tree_sha256": "d" * 64}
            with patch.object(build_info, "git_state", return_value=state):
                info = build_info.create_build_info(root, publish, build, "Debug", "10.0.402", output)
            self.assertEqual(state, info["source"])
            self.assertEqual("Debug", info["configuration"])
            self.assertTrue(info["development_build"])
            self.assertEqual("not-claimed", info["signing"])


if __name__ == "__main__":
    unittest.main()
