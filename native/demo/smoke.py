# SPDX-License-Identifier: GPL-3.0-only
"""Windows/GPU integration smoke tests; no third-party Python packages needed."""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


def check_summary(process, api, mode="windowed", positive=True):
    stdout, stderr = process.communicate(timeout=45)
    if process.returncode:
        raise AssertionError(f"{api} exited {process.returncode}: {stdout}\n{stderr}")
    lines = stdout.strip().splitlines()
    assert len(lines) == 1, stdout
    summary = json.loads(lines[0])
    assert summary["exit_status"] == 0 and summary["api"] == api, summary
    assert summary["width"] > 0 and summary["height"] > 0, summary
    assert summary["frame_count"] > 0 if positive else summary["frame_count"] >= 0, summary
    assert summary["fullscreen_requested"] == (mode == "exclusive"), summary
    if mode != "exclusive":
        assert not summary["fullscreen_acquired"], summary
    print(json.dumps(summary))
    if stderr.strip():
        print(stderr.strip(), file=sys.stderr)
    return summary


def configure_win32():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateEventW.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.SetEvent.argtypes = [wintypes.HANDLE]
    kernel.SetEvent.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.EnumWindows.restype = wintypes.BOOL
    user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user.SetWindowPos.restype = wintypes.BOOL
    user.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user.ShowWindow.restype = wintypes.BOOL
    return kernel, user, callback_type


def window_for_process(user, callback_type, pid):
    result = []

    @callback_type
    def callback(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid:
            result.append(window)
        return True

    user.EnumWindows(callback, 0)
    return result[0] if result else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--exclusive", action="store_true", help="also test explicit exclusive requests")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("requires Windows x64 with a graphics-capable desktop")
    executable = str(args.executable.resolve())
    kernel, user, callback_type = configure_win32()
    environment = dict(os.environ)
    environment.pop("FRAMESCOPE_DEMO_GATE", None)
    environment.pop("FRAMESCOPE_SESSION", None)

    def launch(*arguments, env=environment):
        return subprocess.Popen([executable, *arguments], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env=env)

    for api in ("dx11", "dx12", "vulkan"):
        process = launch("--api", api, "--duration", "3", "--vsync", "off",
                         "--cpu-load", "5", "--gpu-load", "3", "--stutter-ms", "30")
        try:
            deadline = time.monotonic() + 10
            window = None
            while process.poll() is None and time.monotonic() < deadline:
                window = window_for_process(user, callback_type, process.pid)
                if window:
                    break
                time.sleep(0.01)
            assert window, f"{api} did not create a window"
            time.sleep(0.25)
            assert user.SetWindowPos(window, None, 0, 0, 960, 640, 0x0002 | 0x0004), ctypes.get_last_error()
            time.sleep(0.25)
            user.ShowWindow(window, 6)  # SW_MINIMIZE
            time.sleep(0.25)
            user.ShowWindow(window, 9)  # SW_RESTORE
            summary = check_summary(process, api)
            assert summary["width"] < 1280 and summary["height"] < 720, summary
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

        # Event names and readiness deliberately match the launcher protocol.
        session = uuid.uuid4().hex
        ready = kernel.CreateEventW(None, True, False, f"Local\\FrameScope-{session}-ready")
        start = kernel.CreateEventW(None, True, False, f"Local\\FrameScope-{session}-start")
        assert ready and start, ctypes.get_last_error()
        gated = dict(environment, FRAMESCOPE_DEMO_GATE="1", FRAMESCOPE_SESSION=session)
        process = launch("--api", api, "--duration", "0.4", env=gated)
        try:
            assert kernel.WaitForSingleObject(ready, 10000) == 0, "ready not signaled"
            time.sleep(0.15)
            assert process.poll() is None, "demo exited while gated"
            assert not window_for_process(user, callback_type, process.pid), "window/graphics started before release"
            assert kernel.SetEvent(start), ctypes.get_last_error()
            check_summary(process, api)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            kernel.CloseHandle(start)
            kernel.CloseHandle(ready)

        for mode in (("borderless", "exclusive") if args.exclusive else ("borderless",)):
            process = launch("--api", api, "--mode", mode, "--duration", "0.4")
            try:
                check_summary(process, api, mode)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    for arguments, env in ((["--gpu-load", "101"], environment),
                           (["--cpu-load", "-1"], environment),
                           (["--duration", "nan"], environment),
                           (["--api", "invalid"], environment),
                           ([], dict(environment, FRAMESCOPE_DEMO_GATE="1", FRAMESCOPE_SESSION="invalid"))):
        process = launch(*arguments, env=env)
        stdout, stderr = process.communicate(timeout=10)
        summary = json.loads(stdout)
        assert process.returncode == 1 and summary["exit_status"] == 1, (stdout, stderr)
        assert summary["frame_count"] == 0 and summary["error"], summary
    print("Demo smoke tests passed.")


if __name__ == "__main__":
    main()
