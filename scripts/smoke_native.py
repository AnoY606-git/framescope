#!/usr/bin/env python3
"""Exercise the real native module/layer against the separate three-API demo.

This is a hardware smoke test, not a benchmark or proof of arbitrary-game support.
It never launches a third-party game or modifies global Vulkan configuration.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import math
from pathlib import Path
import platform
import struct
import subprocess
import sys
import time
import uuid
import zlib

MAGIC = 0x46534350
CONTROL = struct.Struct("<IIffffIIII10dQ")
FRAME = struct.Struct("<QQ5d6I120f")
API_IDS = {"dx11": 1, "dx12": 2, "vulkan": 3}


def windows_functions():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileMappingW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR]
    kernel.CreateFileMappingW.restype = wintypes.HANDLE
    kernel.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                    wintypes.DWORD, ctypes.c_size_t]
    kernel.MapViewOfFile.restype = ctypes.c_void_p
    kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.QueryPerformanceCounter.argtypes = [ctypes.POINTER(ctypes.c_longlong)]
    kernel.QueryPerformanceFrequency.argtypes = [ctypes.POINTER(ctypes.c_longlong)]
    return kernel


class SharedSession:
    def __init__(self, layout: str = "panel", scale: float = 1.5):
        self.kernel = windows_functions()
        self.session = uuid.uuid4().hex
        self.handle = self.kernel.CreateFileMappingW(
            wintypes.HANDLE(-1), None, 4, 0, 4096, f"Local\\FrameScope-{self.session}")
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        self.address = self.kernel.MapViewOfFile(self.handle, 0xF001F, 0, 0, 4096)
        if not self.address:
            self.kernel.CloseHandle(self.handle)
            raise ctypes.WinError(ctypes.get_last_error())
        self.buffer = (ctypes.c_ubyte * 4096).from_address(self.address)
        frequency = ctypes.c_longlong()
        self.kernel.QueryPerformanceFrequency(ctypes.byref(frequency))
        self.frequency = frequency.value
        struct.pack_into("<IIIIQ", self.buffer, 0, MAGIC, 1, 4096, 0, self.frequency)
        self.sequence = 0
        self.layout = layout
        self.scale = scale
        self.heartbeat()

    def heartbeat(self):
        now = ctypes.c_longlong()
        self.kernel.QueryPerformanceCounter(ctypes.byref(now))
        # Только реальные RAM-данные. Остальные датчики намеренно недоступны в smoke broker.
        class MemoryStatus(ctypes.Structure):
            _fields_ = [("length", wintypes.DWORD), ("load", wintypes.DWORD)] + [
                (name, ctypes.c_ulonglong) for name in
                ("total", "available", "total_page", "available_page", "total_virtual",
                 "available_virtual", "available_extended")]
        memory = MemoryStatus()
        memory.length = ctypes.sizeof(memory)
        ram_ok = bool(self.kernel.GlobalMemoryStatusEx(ctypes.byref(memory)))
        # Этот минимальный broker проверяет кадры и RAM, а не подделывает остальные датчики.
        flags = 1 | 4 | 8 | 16 | 128 | 512
        if self.layout == "strip":
            flags |= 2
        payload = CONTROL.pack(flags, 0, self.scale, 0.86, 24, 24,
                               0xFFF4F7FC, 0xFF55DEB0, 0xFF101820, 8 if ram_ok else 0,
                               0, 0, 0, (memory.total - memory.available) / 2**30,
                               memory.total / 2**30, 0, 0, 0, 0, 0, now.value)
        self.sequence += 1
        struct.pack_into("<Q", self.buffer, 24, self.sequence)
        ctypes.memmove(self.address + 32, payload, len(payload))
        self.sequence += 1
        struct.pack_into("<Q", self.buffer, 24, self.sequence)

    def snapshot(self):
        for _ in range(16):
            before = struct.unpack_from("<Q", self.buffer, 256)[0]
            if before & 1:
                continue
            raw = bytes(self.buffer[264:824])
            after = struct.unpack_from("<Q", self.buffer, 256)[0]
            if before == after:
                values = FRAME.unpack(raw)
                return {
                    "present_count": values[0], "timestamp_qpc": values[1],
                    "fps": values[2], "frametime_ms": values[3],
                    "low_1": values[4], "low_01": values[5], "hud_cpu_us": values[6],
                    "api_id": values[7], "flags": values[8], "graph_count": values[9],
                    "width": values[10], "height": values[11], "process_id": values[12],
                    "graph": list(values[13:13 + min(values[9], 120)]),
                }
        return None

    def close(self):
        self.buffer = None
        self.kernel.UnmapViewOfFile(self.address)
        self.kernel.CloseHandle(self.handle)


def capture_own_foreground(process_id: int, output: Path) -> bool:
    """Only capture our verified foreground demo's client area, never the desktop."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    gdi = ctypes.WinDLL("gdi32", use_last_error=True)
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    user.GetDC.argtypes = [wintypes.HWND]
    user.GetDC.restype = wintypes.HDC
    user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    hwnd = user.GetForegroundWindow()
    pid = wintypes.DWORD()
    user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value != process_id:
        return False
    rect = wintypes.RECT()
    origin = wintypes.POINT(0, 0)
    if not user.GetClientRect(hwnd, ctypes.byref(rect)) or not user.ClientToScreen(hwnd, ctypes.byref(origin)):
        return False
    width, height = rect.right, rect.bottom
    if not 0 < width <= 7680 or not 0 < height <= 4320:
        return False

    class BitmapInfo(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("width", wintypes.LONG), ("height", wintypes.LONG),
                    ("planes", wintypes.WORD), ("bits", wintypes.WORD), ("compression", wintypes.DWORD),
                    ("image_size", wintypes.DWORD), ("xppm", wintypes.LONG), ("yppm", wintypes.LONG),
                    ("used", wintypes.DWORD), ("important", wintypes.DWORD)]
    gdi.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi.CreateCompatibleDC.restype = wintypes.HDC
    gdi.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
                                    ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
    gdi.CreateDIBSection.restype = wintypes.HBITMAP
    gdi.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]
    gdi.SelectObject.restype = wintypes.HANDLE
    gdi.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                          wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    gdi.DeleteObject.argtypes = [wintypes.HANDLE]
    gdi.DeleteDC.argtypes = [wintypes.HDC]
    source = user.GetDC(None)
    destination = gdi.CreateCompatibleDC(source)
    bits = ctypes.c_void_p()
    info = BitmapInfo(40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
    bitmap = gdi.CreateDIBSection(source, ctypes.byref(info), 0, ctypes.byref(bits), None, 0)
    if not bitmap:
        gdi.DeleteDC(destination)
        user.ReleaseDC(None, source)
        return False
    previous = gdi.SelectObject(destination, bitmap)
    try:
        if not gdi.BitBlt(destination, 0, 0, width, height, source, origin.x, origin.y, 0x40CC0020):
            return False
        raw = ctypes.string_at(bits, width * height * 4)
        rgba = bytearray(len(raw))
        rgba[0::4], rgba[1::4], rgba[2::4] = raw[2::4], raw[1::4], raw[0::4]
        rgba[3::4] = b"\xff" * (width * height)
        rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(png)
        return True
    finally:
        gdi.SelectObject(destination, previous)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(destination)
        user.ReleaseDC(None, source)


def run_one(binary_dir: Path, api: str, duration: int, mode: str, capture: bool, output: Path,
            layout: str = "panel", scale: float = 1.5):
    session = SharedSession(layout, scale)
    process = None
    result = {"api": api, "mode": mode, "layout": layout, "passed": False, "samples": 0, "max_observed_interval_ms": 0}
    try:
        command = [str(binary_dir / "FrameScope.Launcher.exe"), "--demo",
                   str(binary_dir / "FrameScope.Demo.exe"), "--api", api,
                   "--session", session.session, "--mode", mode, "--duration", str(duration),
                   "--stutter-ms", "70"]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", errors="replace")
        started = time.monotonic()
        captured = False
        last = None
        while process.poll() is None:
            if time.monotonic() - started > duration + 45:
                raise TimeoutError("own demo/launcher did not complete within the smoke deadline")
            session.heartbeat()
            frame = session.snapshot()
            if frame and frame["present_count"] > 0:
                last = frame
                result["samples"] += 1
                if frame["graph"]:
                    result["max_observed_interval_ms"] = max(result["max_observed_interval_ms"], max(frame["graph"]))
                if capture and not captured and time.monotonic() - started >= 3:
                    path = output.parent / f"hud-{api}-{mode}.png"
                    captured = capture_own_foreground(frame["process_id"], path)
                    if captured:
                        result["screenshot"] = str(path)
            time.sleep(0.1)
        stdout, stderr = process.communicate(timeout=5)
        result.update(exit_code=process.returncode, stdout=stdout.strip(), stderr=stderr.strip())
        last = session.snapshot() or last
        if last:
            result["frame"] = {key: value for key, value in last.items() if key != "graph"}
        errors = []
        if process.returncode != 0:
            errors.append("launcher/demo returned nonzero")
        if not last or last["present_count"] < 10:
            errors.append("real module/layer did not publish enough presents")
        elif last["api_id"] != API_IDS[api]:
            errors.append("module reported wrong API")
        elif not (last["flags"] & 1) or not math.isfinite(last["fps"]) or last["fps"] <= 0:
            errors.append("present FPS is unavailable/invalid")
        elif not 0 < last["graph_count"] <= 120:
            errors.append("bounded frametime graph missing")
        result["errors"] = errors
        result["passed"] = not errors
        return result
    finally:
        if process and process.poll() is None:
            # Only the exact launcher we created; the demo has its own bounded duration.
            process.terminate()
            process.communicate(timeout=5)
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, default=Path("artifacts/native/bin"))
    parser.add_argument("--api", choices=["all", *API_IDS], default="all")
    parser.add_argument("--mode", choices=["windowed", "borderless", "exclusive"], default="windowed")
    parser.add_argument("--duration", type=int, default=7)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--layout", choices=["panel", "strip"], default="panel")
    parser.add_argument("--scale", type=float, default=1.5)
    parser.add_argument("--output", type=Path, default=Path("artifacts/verification/native-smoke.json"))
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("native hardware smoke requires an interactive Windows x64 session")
    if not 3 <= args.duration <= 120:
        parser.error("duration must be 3..120 seconds")
    if not math.isfinite(args.scale) or not 0.6 <= args.scale <= 2.5:
        parser.error("scale must be 0.6..2.5")
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except (AttributeError, OSError):
        pass
    api_list = list(API_IDS) if args.api == "all" else [args.api]
    report = {"kind": "hardware-smoke-not-benchmark", "platform": platform.platform(),
              "date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "results": []}
    for api in api_list:
        try:
            result = run_one(args.bin.resolve(), api, args.duration, args.mode, args.capture,
                             args.output, args.layout, args.scale)
        except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
            result = {"api": api, "mode": args.mode, "passed": False, "errors": [str(error)]}
        report["results"].append(result)
        print(json.dumps(result, ensure_ascii=False), flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0 if all(result["passed"] for result in report["results"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
