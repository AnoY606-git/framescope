# Restricted demo launcher

Build `main.cpp`, `native/common/bridge.cpp` and
`native/common/statistics.cpp` as `FrameScope.Launcher.exe` (console subsystem,
Windows x64). Add `native/include` to includes and link **`advapi32` and
`user32`** in addition to the normal Windows runtime libraries. The entry point
is `wmain`; llvm-mingw needs `-municode`. No ImGui or DXGI renderer dependency is
needed for the launcher.

Accepted arguments:

```text
--demo PATH --api dx11|dx12|vulkan --session 32hex
[--mode windowed|borderless|exclusive] [--duration 0..86400]
[--cpu-load 0..100] [--gpu-load 1..100] [--stutter-ms 0..5000]
[--vsync on|off]
```

`--demo` must resolve to `FrameScope.Demo.exe` beside this launcher. Native
images must be x64 PE files. The launcher has no PID, external DLL, arbitrary
target, admin-elevation, registry or global-environment options. A valid existing
UI session mapping is mandatory; the launcher never creates control data or
writes frame metrics. The UI must choose a fresh random session ID per run.

DX11/DX12 sequence: create local manual-reset events with default DACLs, launch
the gated demo, await ready, use ordinary `LoadLibraryW` remote-thread loading of
the adjacent `FrameScope.Hud.dll` in that child only, signal internal `loaded`
after the load thread has completely exited, await actual hook-ready,
then release start. System module addresses are resolved by module-relative
offset, including forwarded `LoadLibraryW` exports; 64-bit HMODULE success is
not inferred from a truncated thread exit code.

Vulkan sequence: require a non-elevated launcher, append the adjacent manifest
directory to child-only `VK_ADD_LAYER_PATH`, also append it to an already-set
`VK_LAYER_PATH` (which otherwise overrides ADD), and append
`VK_LAYER_FRAMESCOPE_hud` to `VK_INSTANCE_LAYERS`. Existing values and all other
environment entries are preserved. Launch the demo and release its start gate
without waiting for a DXGI hook-ready event. The Vulkan loader negotiates the
layer during graphics initialization; actual frame telemetry, not a fabricated
launcher success metric, confirms rendering in the UI/smoke broker. No injection
is performed. Conflicting preserved Vulkan loader filters can still disable
layers; diagnostics explain
this instead of changing user settings.

The demo inherits only duplicated stdin/stdout/stderr handles, preserving JSON
stdout for smoke tests. After launch the launcher waits and propagates the
child's actual exit code. Failure/cancellation releases the gate and requests
WM_CLOSE/WM_QUIT; only that just-created child is terminated as a last resort if
it does not exit within the grace period. A timed-out remote load keeps its path
allocation alive until child exit rather than freeing memory a running thread
could still read.
