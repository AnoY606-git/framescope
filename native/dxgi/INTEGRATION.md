# DXGI private-preview module

Build `FrameScope.Hud.dll` as a Windows x64 shared library from:

- `dllmain.cpp`
- `hooks.cpp`
- `overlay.cpp`
- `dx11_renderer.cpp`
- `dx12_renderer.cpp`

Link `framescope_common`, `minhook`, `imgui_dx11`, `imgui_dx12`, `d3d11`,
`d3d12`, `dxgi`, `dxguid`, `d3dcompiler`, and `user32`. Use Dear ImGui
**v1.91.9b**, including its unmodified official renderer backends. This version
uses `ImGui_ImplDX12_InitInfo`, explicit descriptor callbacks and `CommandQueue`;
it does not use the newer `IMGUI_HAS_TEXTURES` API. No Win32 ImGui input backend
is used. WRL `ComPtr` works with both the Windows SDK and llvm-mingw headers.

## Lifecycle and scope

- A worker outside loader lock validates the opted-in `FrameScope.Demo.exe` and
  existing shared mapping before installing hooks. The module is pinned for the
  process lifetime; live detours are never unloaded.
- Factory exports and `CreateSwapChain` / `CreateSwapChainForHwnd` are hooked;
  the exact D3D12 direct queue passed to DXGI is retained. New implementation
  addresses are hooked as factories/swapchains are returned. There is no guessed
  or replacement present queue.
- `Present`, `Present1`, `ResizeBuffers` and `ResizeBuffers1` forward their
  arguments and HRESULT unchanged. TEST and recursive presents are excluded;
  failed and occluded presents do not contribute samples.
- The first eligible visible, unowned, top-level window's supported swapchain
  becomes primary. This preview supports one main swapchain for its lifetime,
  not arbitrary multi-window/multi-device recreation, UWP/composition surfaces,
  HDR, multi-queue or linked-node D3D12 configurations. Present1 dirty regions
  are not expanded or modified; the demo performs full-frame presentation.
- D3D11 uses the official save/restore backend plus output-target/UAV and
  HS/DS/CS shader restoration. The owned demo uses the ordinary D3D11.0 slots.
  Its dynamic vertex/index uploads use the official backend's WRITE_DISCARD
  resource renaming. There is deliberately no global GPU-completion query gate:
  gating on the previous frame makes the HUD disappear from normally pipelined
  flip-discard frames. No fence wait or forced Flush is added to the frame path.
- D3D12 owns per-backbuffer allocators, lists, RTVs and fence values, a private
  shader-visible SRV heap, and a shared completion fence. Both actual backbuffer
  and ImGui upload-ring reuse are fenced. Busy frames skip drawing without a
  fence wait. Each submission transitions PRESENT -> RENDER_TARGET -> PRESENT
  on the captured real queue.
- The pinned official D3D12 backend performs one synchronous font upload during
  initial renderer setup (and after resize). Resize can wait up to two seconds
  for this module's submitted work before releasing backbuffers. A timeout
  retains GPU-referenced resources rather than risking use-after-free; the
  original ResizeBuffers call/result is still forwarded unchanged.

The launcher creates `ready`, `start`, `hook`, an internal `loaded` gate and an
additional diagnostic `failed` event using the shared naming helper. The worker
waits for `loaded` before enabling hooks, so MinHook's thread suspension cannot
race the injection thread's loader/TLS-detach work. Hook-ready is signaled only
after all required bootstrap hooks are installed. Initialization errors signal
`failed` when available and write diagnostics to inherited stderr and the debug
output. No metrics are fabricated when drawing is unavailable.

## Visual regression check

Present telemetry alone does **not** prove that HUD pixels are visible. Run
`python native/dxgi/tests/visual_smoke.py --bin artifacts/native/bin --output PATH`
against a built root native directory. The test uses the existing minimal broker,
captures only the verified own demo's foreground client area, and checks multiple
advancing frames for actual accent/text glyph pixels. Foreground denial or
insufficient distinct captures fails validation rather than silently passing on
present counts. Repeat against `artifacts/debug-native/bin` for Debug.

The DX11 renderer emits one submission diagnostic for its first positive
indexed-geometry frame and logs each skip reason at most once, not every frame.
