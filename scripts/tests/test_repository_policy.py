# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_support as support
from test_support import temporary_directory


class RepositoryPolicyTests(unittest.TestCase):
    def test_rejects_internal_keys_outputs_at_any_depth(self):
        for name in ("AGENTS.md", "module/CLAUDE.md", "docs/plans/a.md", ".tools/sdk/a.txt",
                     "src/bin/data.txt", "src/obj/data.txt", "artifacts/build-info.json",
                     "docs/internal/log.md", ".env", "config/.env.production", "signing.pfx",
                     "keys/private.pem", "native/FrameScope.Hud.dll", "app.exe", "../outside.txt"):
            with self.subTest(name=name):
                self.assertIsNotNone(support.path_error(name))

    def test_allows_product_source_and_nonsecret_env_example(self):
        for name in ("README.md", "docs/ARCHITECTURE.md", "native/hud.cpp", "src/App.cs",
                     ".env.example", "scripts/tests/test_repository_policy.py", "LICENSE"):
            self.assertIsNone(support.path_error(name))

    def test_secret_is_rejected_without_its_value(self):
        secret = "gh" + "p_" + "a" * 36
        with self.assertRaises(ValueError) as raised:
            support.scan_text("config.txt", secret)
        self.assertNotIn(secret, str(raised.exception))
        self.assertIn("config.txt", str(raised.exception))

    def test_private_key_marker_is_rejected(self):
        with self.assertRaises(ValueError):
            support.scan_text("key.txt", "-----BEGIN " + "PRIVATE KEY-----")

    def test_home_path_is_rejected_but_documented_variable_is_allowed(self):
        with self.assertRaises(ValueError):
            support.scan_text("config.txt", "/home/" + "synthetic-user/project")
        support.scan_text("docs.md", "%LOCALAPPDATA%/FrameScope")

    def test_missing_license_and_other_required_files_fail(self):
        with temporary_directory() as directory:
            root = Path(directory)
            (root / "README.md").write_text("test", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Missing required"):
                support.validate_sources(root, ["README.md"], dependencies=False)

    def test_symlink_source_cannot_escape_root(self):
        with temporary_directory() as directory:
            root = Path(directory)
            target = root / "target.txt"
            target.write_text("synthetic", encoding="utf-8")
            link = root / "link.txt"
            try:
                link.symlink_to(target)
            except OSError:
                self.skipTest("Host does not allow unprivileged symlinks")
            with self.assertRaisesRegex(ValueError, "symlink"):
                support.validate_sources(root, ["link.txt"], required=False, dependencies=False)

    def test_binary_disguised_as_text_is_rejected(self):
        with temporary_directory() as directory:
            root = Path(directory)
            (root / "source.txt").write_bytes(b"a\0b")
            with self.assertRaisesRegex(ValueError, "NUL"):
                support.validate_sources(root, ["source.txt"], required=False, dependencies=False)

    def test_reparse_ancestor_is_rejected_even_when_leaf_is_regular(self):
        with temporary_directory() as directory:
            root = Path(directory).resolve()
            (root / "nested").mkdir()
            (root / "nested/source.txt").write_text("fixture")
            with patch.object(support, "is_reparse", side_effect=lambda path: path == root / "nested"):
                with self.assertRaisesRegex(ValueError, "symlink"):
                    support.validate_sources(root, ["nested/source.txt"], required=False, dependencies=False)

    def test_version_file_is_authoritative(self):
        with temporary_directory() as directory:
            root = Path(directory)
            (root / "VERSION").write_text("0.1.0-preview\n")
            self.assertEqual("0.1.0-preview", support.version(root))
            (root / "VERSION").write_text("latest")
            with self.assertRaisesRegex(ValueError, "VERSION"):
                support.version(root)

    def test_git_selection_preserves_unusual_names_and_does_not_execute_them(self):
        with temporary_directory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            names = ["source with spaces.txt", "$(touch SHOULD_NOT_EXIST).txt"]
            for name in names:
                (root / name).write_text("fixture", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "--", *names], check=True)
            (root / "untracked.txt").write_text("fixture", encoding="utf-8")
            self.assertEqual(set(names), set(support.source_paths(root)))
            self.assertIn("untracked.txt", support.source_paths(root, include_untracked=True))
            self.assertFalse((root / "SHOULD_NOT_EXIST").exists())

    def test_dependency_versions_and_project_version_are_checked(self):
        with temporary_directory() as directory:
            root = Path(directory)
            (root / "global.json").write_text(json.dumps({"sdk": {"version": "10.0.100"}}))
            (root / "Directory.Build.props").write_text("<Project><PropertyGroup><Version>0.1.0-preview</Version></PropertyGroup></Project>")
            project = root / "App.csproj"
            project.write_text('<Project><ItemGroup><PackageReference Include="Example" Version="1.2.3" /></ItemGroup></Project>')
            support.audit_dependencies(root, ["App.csproj"])
            project.write_text('<Project><ItemGroup><PackageReference Include="Example" Version="1.*" /></ItemGroup></Project>')
            with self.assertRaisesRegex(ValueError, "explicit package"):
                support.audit_dependencies(root, ["App.csproj"])
            project.write_text('<Project><PropertyGroup><Version>0.1.0</Version></PropertyGroup></Project>')
            with self.assertRaisesRegex(ValueError, "Version differs"):
                support.audit_dependencies(root, ["App.csproj"])

    def test_unpinned_actions_and_mutable_cmake_refs_are_rejected(self):
        with temporary_directory() as directory:
            root = Path(directory)
            (root / "global.json").write_text('{"sdk":{"version":"10.0.100"}}')
            (root / "Directory.Build.props").write_text('<Project><PropertyGroup><Version>0.1.0-preview</Version></PropertyGroup></Project>')
            cmake = root / "CMakeLists.txt"
            cmake.write_text('FetchContent_Declare(example\n GIT_TAG main\n)')
            with self.assertRaisesRegex(ValueError, "Unpinned"):
                support.audit_dependencies(root, ["CMakeLists.txt"])
            workflow = root / ".github/workflows/ci.yml"
            workflow.parent.mkdir(parents=True)
            workflow.write_text('steps:\n  - uses: actions/checkout@v4\n')
            with self.assertRaisesRegex(ValueError, "commit SHA"):
                support.audit_dependencies(root, [".github/workflows/ci.yml"])


if __name__ == "__main__":
    unittest.main()
