# SPDX-License-Identifier: GPL-3.0-only
"""Validate actual HUD pixels in several own-demo frames, not just telemetry.

Uses the root smoke broker unchanged (control flags 669), activates only the
verified own-child HWND, and captures only its foreground client area. Missing
capture permission/foreground is a failure, never a telemetry-only success.
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
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_native import SharedSession, capture_own_foreground  # noqa: E402
from smoke_resize import find_own_window  # noqa: E402


def hud_pixels(path):
    """Count the broker's accent/text glyph colors in its top-left HUD region.

    Linear-filtered, scaled font coverage blends glyph colors with the HUD
    background, so exact source-color matching would incorrectly reject a real
    HUD. Require many green accent AND neutral light text pixels in the first
    four text rows, not merely a dark panel or an isolated scene-triangle pixel.
    """
    png = path.read_bytes()
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Capture is not a PNG")
    width, height, depth, color = struct.unpack_from(">IIBB", png, 16)
    if depth != 8 or color != 6:
        raise ValueError("Expected RGBA capture from the own-window smoke helper")
    packed = bytearray()
    offset = 8
    while offset < len(png):
        length = struct.unpack_from(">I", png, offset)[0]
        if png[offset + 4:offset + 8] == b"IDAT":
            packed.extend(png[offset + 8:offset + 8 + length])
        offset += length + 12
    raw = zlib.decompress(packed)
    stride = 1 + width * 4
    accent = text = 0
    for y in range(28, min(128, height)):
        if raw[y * stride] != 0:
            raise ValueError("Expected unfiltered own-client PNG rows")
        for x in range(30, min(420, width)):
            pixel = raw[y * stride + 1 + x * 4:y * stride + 1 + x * 4 + 3]
            red, green, blue = pixel
            accent += (60 < red < 105 and 160 < green < 240 and 130 < blue < 200 and
                       green - red > 80 and green - blue > 20)
            text += min(pixel) > 160 and max(pixel) - min(pixel) < 20
    return {"accent_pixels": accent, "text_pixels": text,
            "hud_visible": accent >= 100 and text >= 200}


def run(binary_dir, api, output, samples, duration):
    output.mkdir(parents=True, exist_ok=True)
    session = SharedSession()
    process = None
    result = {"api": api, "passed": False, "captures": [], "foreground_rejections": 0}
    try:
        user = ctypes.WinDLL("user32", use_last_error=True)
        user.SetForegroundWindow.argtypes = [wintypes.HWND]
        user.GetForegroundWindow.restype = wintypes.HWND
        try:
            user.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        except (AttributeError, OSError):
            pass
        command = [str(binary_dir / "FrameScope.Launcher.exe"), "--demo",
                   str(binary_dir / "FrameScope.Demo.exe"), "--api", api,
                   "--session", session.session, "--mode", "windowed", "--duration", str(duration),
                   "--stutter-ms", "70"]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", errors="replace")
        deadline = time.monotonic() + duration + 30
        first = next_capture = None
        while process.poll() is None:
            now = time.monotonic()
            if now > deadline:
                raise TimeoutError("Own demo visual validation timed out")
            session.heartbeat()
            frame = session.snapshot()
            if frame and frame["present_count"] >= 30:
                if first is None:
                    first = now
                    next_capture = now + 0.5
                last_count = result["captures"][-1]["present_count"] if result["captures"] else 0
                if (len(result["captures"]) < samples and now >= next_capture and
                        frame["present_count"] >= last_count + 6):
                    # Rate-limit foreground requests even if another window has
                    # focus; never repeatedly capture one paused/stale frame.
                    next_capture = now + 0.173
                    window = find_own_window(frame["process_id"])
                    if window and user.GetForegroundWindow() != window:
                        user.SetForegroundWindow(window)
                    image = output / f"hud-{api}-{len(result['captures']):02d}.png"
                    if capture_own_foreground(frame["process_id"], image):
                        result["captures"].append({"path": str(image),
                            "present_count": frame["present_count"], **hud_pixels(image)})
                    else:
                        result["foreground_rejections"] += 1
            time.sleep(0.01)
        stdout, stderr = process.communicate(timeout=5)
        result.update(exit_code=process.returncode, stdout=stdout.strip(), stderr=stderr.strip())
        frame = session.snapshot()
        summary = json.loads(stdout.strip().splitlines()[-1]) if stdout.strip() else {}
        result["present_count"] = frame["present_count"] if frame else 0
        errors = []
        if process.returncode != 0:
            errors.append("launcher/demo returned nonzero")
        if len(result["captures"]) != samples:
            errors.append(f"Only {len(result['captures'])}/{samples} distinct own-foreground captures available")
        if any(not item["hud_visible"] for item in result["captures"]):
            errors.append("HUD accent/text pixels missing from one or more actual captures")
        if result["present_count"] != summary.get("frame_count", -1):
            errors.append("module/demo present counts differ")
        result["errors"] = errors
        result["passed"] = not errors
        return result
    finally:
        if process and process.poll() is None:
            process.terminate()
            process.communicate(timeout=5)
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, default=ROOT / "artifacts/native/bin")
    parser.add_argument("--api", choices=("dx11", "dx12"), default="dx11")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=12)
    parser.add_argument("--duration", type=int, default=8)
    args = parser.parse_args()
    if sys.platform != "win32" or not 1 <= args.samples <= 20 or not 6 <= args.duration <= 30:
        parser.error("Requires interactive Windows, 1..20 samples and 6..30 second duration")
    result = run(args.bin.resolve(), args.api, args.output.resolve(), args.samples, args.duration)
    (args.output / "visual-smoke.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
