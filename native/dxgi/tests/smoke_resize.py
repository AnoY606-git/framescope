# SPDX-License-Identifier: GPL-3.0-only
"""Real own-demo injection/resize/disable smoke; no external target is accepted.

Run from the repository: python native/dxgi/tests/smoke_resize.py --bin PATH
Uses the parent's shared-session smoke broker; never writes frame telemetry.
"""

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_native import SharedSession  # noqa: E402


def find_own_window(process_id):
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    found = []

    @callback_type
    def visit(window, unused):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == process_id and user.IsWindowVisible(window):
            found.append(window)
            return False
        return True

    user.EnumWindows(visit, 0)
    return found[0] if found else None


def set_enabled(session, enabled):
    # Only the controller-owned control block is touched, using its seqlock.
    flags = struct.unpack_from("<I", session.buffer, 32)[0]
    session.sequence += 1
    struct.pack_into("<Q", session.buffer, 24, session.sequence)
    struct.pack_into("<I", session.buffer, 32, flags | 1 if enabled else flags & ~1)
    session.sequence += 1
    struct.pack_into("<Q", session.buffer, 24, session.sequence)


def run(binary_dir, api):
    session = SharedSession()
    process = None
    try:
        command = [str(binary_dir / "FrameScope.Launcher.exe"), "--demo",
                   str(binary_dir / "FrameScope.Demo.exe"), "--api", api,
                   "--session", session.session, "--mode", "windowed", "--duration", "5"]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", errors="replace")
        deadline = time.monotonic() + 35
        first_time = None
        first_size = None
        resized = False
        observed_new_size = False
        disabled_first = None
        disabled_last = None
        while process.poll() is None:
            now = time.monotonic()
            if now > deadline:
                raise TimeoutError("Own demo smoke deadline exceeded")
            session.heartbeat()
            frame = session.snapshot()
            if frame and frame["present_count"] > 10:
                if first_time is None:
                    first_time = now
                    first_size = (frame["width"], frame["height"])
                elapsed = now - first_time
                if elapsed > 0.7 and not resized:
                    window = find_own_window(frame["process_id"])
                    if window:
                        user = ctypes.WinDLL("user32", use_last_error=True)
                        user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                                     ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
                        # Only our verified child HWND; do not move, activate or reorder it.
                        if not user.SetWindowPos(window, None, 0, 0, 1000, 620, 0x2 | 0x4 | 0x10):
                            raise ctypes.WinError(ctypes.get_last_error())
                        resized = True
                if resized and (frame["width"], frame["height"]) != first_size:
                    observed_new_size = True
                if 1.5 <= elapsed < 2.5:
                    set_enabled(session, False)
                    if disabled_first is None:
                        disabled_first = frame["present_count"]
                    disabled_last = frame["present_count"]
            time.sleep(0.03)
        stdout, stderr = process.communicate(timeout=5)
        if process.returncode:
            raise RuntimeError(f"Launcher/demo exited {process.returncode}: {stderr}")
        summary = json.loads(stdout.strip().splitlines()[-1])
        frame = session.snapshot()
        assert frame and frame["api_id"] == {"dx11": 1, "dx12": 2}[api], "wrong/missing API"
        assert resized and observed_new_size, "ResizeBuffers did not publish new dimensions"
        assert disabled_first is not None and disabled_last > disabled_first + 5, "HUD disable stopped telemetry"
        assert frame["present_count"] == summary["frame_count"], "recursive/double/missed present count"
        assert frame["present_count"] > 30 and frame["hud_cpu_us"] > 0, "no real frame work observed"
        assert "Selected main" in stderr and "Renderer initialization failed" not in stderr, stderr
        return {"api": api, "passed": True, "present_count": frame["present_count"],
                "resized_from": first_size, "resized_to": (frame["width"], frame["height"]),
                "presents_while_disabled": disabled_last - disabled_first,
                "hud_cpu_us": frame["hud_cpu_us"]}
    finally:
        if process and process.poll() is None:
            # Terminate only the exact test launcher; its own child also has an
            # explicit five-second duration bound. Normal runs exit on their own.
            process.terminate()
            process.communicate(timeout=5)
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, default=ROOT / "artifacts/native/bin")
    parser.add_argument("--api", choices=("dx11", "dx12", "all"), default="all")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Interactive Windows x64 is required")
    for api in ("dx11", "dx12") if args.api == "all" else (args.api,):
        print(json.dumps(run(args.bin.resolve(), api)), flush=True)


if __name__ == "__main__":
    main()
