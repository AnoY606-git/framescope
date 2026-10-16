# Checks performed (2026-10-16)

These are local Windows x64 hardware smoke checks, not performance benchmarks
or claims of arbitrary-game compatibility.

## DX11 visual-regression follow-up (root Release and Debug builds)

The earlier single-capture check was insufficient: the global D3D11 EVENT query
gate caused a flickering HUD, although present telemetry continued correctly.
Root Release reproduction captured **only 1 visible HUD in 12 distinct frames**;
other images showed the bare scene. One-time diagnostics showed `S_FALSE` from
the previous HUD's completion query, not corrupt broker controls. The official
backend uses dynamic WRITE_DISCARD upload buffers, which support asynchronous
resource renaming without waiting for that previous draw to finish on the GPU.

Removed the inappropriate global query gate without adding a wait or forced
Flush. Kept the official backend and supplemental state restoration. Added
one-time positive-indexed-geometry submission and per-reason skip diagnostics;
`overlay.cpp` now consumes the renderer's result instead of ignoring it.

Rebuilt **only FrameScope.Hud** with the existing root Release and Debug Ninja
builds. `tests/visual_smoke.py` uses the unchanged root smoke broker (flags 669,
scale 1.5, opacity 0.86, 24-pixel margins), the same 70 ms demo stutter, and actual
foreground own-client captures. It checks both accent and text glyph pixels,
requires advancing/distinct present counts, and fails if captures are missing.

- Final root Release: **20/20 distinct captured frames have visible HUD pixels**.
- Final root Debug: **12/12 distinct captured frames have visible HUD pixels**;
  module and demo both reported **1126 accepted presents** over eight seconds.
- Final root Release resize/disable regression passed: 1280x720 -> 984x581,
  **713 / 713** presents matched and 140 more presents were observed while hidden.
- Representative Release and Debug images were also inspected directly.
- An intermediate extended run lost foreground access; it was reported as an
  insufficient-capture failure, not counted as a visual success. The test also
  rejects repeatedly capturing one paused frame.

Follow-up reports and images are under
`%LOCALAPPDATA%/Temp/opencode/framescope-dxgi-check/`:

- `root-dx11-query-regression/` (before fix, includes bare-scene captures)
- `root-release-dx11-visual-final/` (20 passing captures)
- `root-debug-dx11-visual-confirm/` (12 passing captures)

No launcher, shared code, CMake source, UI/App source or packaged App copy was
changed for this fix. Existing root native Release/Debug HUD DLLs were rebuilt.

## Initial implementation checks

- Compiled and linked all DXGI sources into `FrameScope.Hud.dll`, and the
  launcher plus shared bridge/statistics into `FrameScope.Launcher.exe`, using
  the existing llvm-mingw Clang 23.1.3 toolchain, C++20, `-Wall -Wextra
  -Wpedantic`, and the root build's pinned ImGui/MinHook archives. Final owned
  sources compiled without warnings. No tools were installed by this component.
- `native/launcher/tests/test_cli.py`: all **5 tests passed**, including external
  target/PID rejection, session validation, numeric limits and duplicate options.
- Real standard LoadLibrary injection into the separate demo passed for DX11
  and DX12. Final DX11 capture smoke published **567 presents**, exactly matching
  the demo's **567**; a DX12 capture run matched **571 / 571**. Both captured
  foreground demo client images were inspected and show the actual HUD over the
  independently rendered scene, including updated FPS, graph and unavailable
  sensors shown as N/A.
- `native/dxgi/tests/smoke_resize.py`: the final DX11/DX12 runs both passed.
  Dimensions changed from **1280x720 to 984x581** after resizing the verified
  own-child window. Present totals matched the demo exactly (**723 / 723** for
  DX11, **722 / 722** for DX12), and each observed **140** more presents during
  the HUD-disabled interval. Drawing resumed without rebuilding the game scene.
- Launcher Vulkan integration with the Vulkan agent's built explicit layer
  passed (**570 / 570** presents). A second run with pre-existing
  `VK_LAYER_PATH`, `VK_ADD_LAYER_PATH` and `VK_INSTANCE_LAYERS` also passed
  (**434 / 434**); the test process's original environment was unchanged by the
  launcher. This validates launcher scoping/integration, not ownership of the
  Vulkan implementation.

Scratch binaries, JSON reports and client-area captures were kept under
`%LOCALAPPDATA%/Temp/opencode/framescope-dxgi-check`, not added to the source tree.

## Findings and remaining checks

An initial startup test exposed a MinHook/load-thread sequencing deadlock. The
internal `loaded` handshake now waits for completed remote-thread exit before
patching, avoiding overlap with loader/TLS-detach work. Subsequent injection,
resize and capture runs passed. One earlier
post-relink module-verification failure was also observed; its precise cause
was not established. Verification now retries documented Toolhelp snapshot
races, compares file identity rather than spelling, and includes the load
thread's low DWORD return in failure diagnostics. It does not retry injection
or fabricate a success/frame sample.

MSVC was not available for an actual compile here. Present1, TEST exclusion and
ResizeBuffers1 were compiled/reviewed, but the demo's ordinary Present and
ResizeBuffers paths—not direct Present1/ResizeBuffers1 stress cases—were exercised.
Exclusive/HDR/multi-window/linked-node/device-loss scenarios were not validated
by these component checks. Supported preview scope and the pinned backend's
one-time font-upload/resize waits are documented in `INTEGRATION.md`.
