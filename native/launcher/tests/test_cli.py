# SPDX-License-Identifier: GPL-3.0-only
"""Non-graphical policy checks against the built restricted launcher."""

import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[3]
BINARY_DIR = Path(os.environ.get("FRAMESCOPE_TEST_BIN", ROOT / "artifacts/native/bin"))
LAUNCHER = BINARY_DIR / "FrameScope.Launcher.exe"


@unittest.skipUnless(sys.platform == "win32" and LAUNCHER.is_file(), "built Windows launcher required")
class LauncherPolicyTests(unittest.TestCase):
    def run_launcher(self, *arguments):
        return subprocess.run([str(LAUNCHER), *arguments], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=5)

    def test_help_explains_restricted_preview(self):
        result = self.run_launcher("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("ONLY FrameScope.Demo.exe", result.stdout)
        self.assertIn("non-elevated", result.stdout)

    def test_external_executable_rejected_before_launch(self):
        result = self.run_launcher("--demo", r"C:\Windows\System32\notepad.exe",
                                   "--api", "dx11", "--session", "a" * 32)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("basename", result.stderr)

    def test_invalid_session_rejected(self):
        result = self.run_launcher("--demo", "FrameScope.Demo.exe", "--api", "dx12",
                                   "--session", "not-a-session")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("32-hex", result.stderr)

    def test_pid_injection_option_does_not_exist(self):
        result = self.run_launcher("--pid", "1234")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown option", result.stderr)

    def test_numeric_and_duplicate_options_rejected(self):
        for arguments in (("--duration", "nan"), ("--cpu-load", "101"),
                          ("--gpu-load", "0"), ("--stutter-ms", "-1"),
                          ("--api", "dx11", "--api", "dx12")):
            with self.subTest(arguments=arguments):
                result = self.run_launcher(*arguments)
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
