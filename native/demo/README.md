# FrameScope standalone demo

GPL-3.0-only. An original animated cube, orbiting triangles, and triangle-based
grid are rendered by genuine DX11, DX12, or Vulkan pipelines. There is **no HUD,
ImGui, HUD DLL loading, telemetry fabrication, or FrameScope core dependency**.
The scene geometry and animation are shared across all APIs.

The root CMake target `FrameScope.Demo` already discovers this directory's seven
`.cpp` files. It needs Windows x64, C++20, the Windows SDK, and the pinned Vulkan
headers. Vulkan functions are dynamically dispatched from the **System32**
`vulkan-1.dll`; no Vulkan loader import library or bundled runtime is needed.
HLSL is compiled with System32 `d3dcompiler_47.dll` at startup. Both a console
entry point and the root target's Unicode GUI entry point are supported.

```powershell
FrameScope.Demo.exe --api dx12 --mode windowed --duration 10 --vsync off
FrameScope.Demo.exe --api vulkan --mode borderless --gpu-load 20 --cpu-load 30 --stutter-ms 80
python native/demo/smoke.py artifacts/native/bin/FrameScope.Demo.exe
```

Options:

| Option | Values / meaning |
| --- | --- |
| `--api` | `dx11` (default), `dx12`, `vulkan` |
| `--mode` | `windowed` (default), `borderless`, `exclusive` |
| `--duration` | 0–86400 seconds; 0 is interactive (default) |
| `--cpu-load` | Integer 0–100; deliberate busy work for `N / 6` milliseconds per frame, **not measured CPU utilization** |
| `--gpu-load` | Integer 1–100; that many complete scene draw calls per frame |
| `--stutter-ms` | Integer 0–5000; deliberate pause every two seconds; 0 disables it |
| `--vsync` | `on` (default), `off`; Vulkan falls back to FIFO if neither immediate nor mailbox is supported |

Esc quits, Space toggles stutter (100 ms if configured as zero), and +/- changes
the GPU draw-call count. Windowed mode is resizable. Minimized windows do not
render, and a finite duration still expires while minimized. Long stutters skip
missed stutter deadlines rather than trying to catch up with repeated pauses.

`borderless` never calls DXGI `SetFullscreenState(TRUE)`. `exclusive` explicitly
requests it; the reported state is obtained from `GetFullscreenState`, not the
window's style. Vulkan exclusive mode requires the enumerated extension and its
dependencies, the surface's advertised capability, an application-controlled
swapchain policy, and a successful `vkAcquireFullScreenExclusiveModeEXT`. A
failure/unsupported configuration is diagnosed on stderr and continues without
claiming exclusive access. Observed exclusive-mode-loss results clear the Vulkan
acquired flag. SDR swapchains are used; no HDR metadata or color space is forced.

## Launcher gate

With `FRAMESCOPE_DEMO_GATE=1`, `FRAMESCOPE_SESSION` must be exactly 32 hex digits.
Before window creation, device creation, loading the Vulkan runtime/compiler, or
any other graphics initialization, the process signals
`Local\FrameScope-<session>-ready` and waits up to 30 seconds for
`Local\FrameScope-<session>-start`. These are the protocol's inline `EventName`
helpers; no out-of-line protocol/core function is referenced. Timeout is an error,
not a fallback to uninstrumented rendering. The launcher may inject its DXGI
module or arrange the Vulkan explicit layer, then release `start`.

## Exit contract

Diagnostics are stderr only. Every run other than `--help` writes one stdout JSON
object containing `api`, `frame_count`, `width`, `height`,
`fullscreen_requested`, `fullscreen_acquired`, `exit_status`, `duration`, and
`error` on failure. Exit status 0 means a clean run; 1 means an error. No error
message box is shown. `duration` is actual render-loop wall time in seconds,
excluding successful initialization and shutdown (on startup failure it is time
since process entry). Dimensions describe the actual last API framebuffer.
`frame_count` counts **accepted presents**, not displayed frames or FPS:
DXGI `S_OK`; Vulkan `VK_SUCCESS`/`VK_SUBOPTIMAL_KHR`. Occluded, timed-out,
out-of-date, or exclusive-mode-lost presentations are not counted.

DX12 uses a real DIRECT queue in `CreateSwapChainForHwnd`, per-buffer allocators
and upload buffers, PRESENT/RENDER_TARGET transitions, and fences covering queue
work submitted through Present hooks. Vulkan uses per-frame acquire semaphores,
per-image present-wait semaphores, per-frame fences, and per-image in-flight fence
tracking. Resize waits for device work, rebuilds dependent resources, and handles
out-of-date acquisition without leaving a reset, unsignaled fence behind.

## Shaders

`shaders/scene.hlsl`, `scene.vert`, and `scene.frag` are original source. The
checked-in SPIR-V headers were generated with glslang 16.6.0, targeting Vulkan 1.0:

```powershell
native/demo/shaders/regenerate.ps1 -Glslang C:/path/to/glslang.exe
```

The runtime uses those arrays directly, so no shader compiler is needed for the
Vulkan demo. The GLSL vertex shader reverses Y to match the DXGI viewport.

`smoke.py` uses only Python's standard library. It exercises timed rendering,
resize/minimize/restore, the launcher gate, invalid input, and optionally explicit
exclusive mode. It needs an interactive Windows desktop and a functioning GPU;
this is not a software/headless rendering substitute.
