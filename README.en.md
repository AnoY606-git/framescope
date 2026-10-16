# FrameScope

**Configure your HUD. Test it against real rendering.**

FrameScope is a **Windows 11 x64** HUD editor: a .NET 10 WPF application and
native C++ backends for DirectX 11, DirectX 12 and Vulkan.

**0.1.0-preview is under development**, not a general-purpose game overlay.
The primary target is the project's own three-API `FrameScope.Demo.exe`.
**CS2 is explicitly out of scope.** Compatibility with third-party games or
anti-cheat software is not claimed.

## Scope

- Panel and compact-strip profiles with crisp system fonts, semantic accents,
  graphs and configurable metrics. The own Demo is the real preview target,
  not fabricated game readings inside the editor.
- FPS and frametime describe **Present-call intervals**, not displayed FPS,
  an engine counter, or Frame Generation.
- Windows CPU/RAM data and available GPU counters. Unavailable readings are
  **N/A**, never made-up zeroes. GPU sensors are not promised for every vendor.
- GPU temperature uses WDDM or Intel Sysman with an explicit source. A separate
  live Level Zero Sysman provider exposes supported GPU/VRAM temperatures,
  clocks, voltage, card power and throttling reasons. There are no clock,
  power-limit or fan setters and no fabricated unavailable sensor values.
- No mandatory kernel service, elevation or Windows-security changes.
  Vulkan layer activation is scoped to the launched Demo, not the registry.

See [verification](docs/VERIFICATION.md) for actual evidence and untested cases.
Source code or a passing unit test does not establish hardware compatibility.

## Running

Intended release formats are a **per-user EXE installer** and a **portable ZIP**.
Run `FrameScope.exe` at the package root and keep its `native/` directory and
runtime files. A self-contained package does not require a separate .NET
installation. Vulkan needs a compatible driver and Vulkan runtime.

The portable package contains `portable.txt`; this selects settings beside the
application and requires a writable directory. Installed mode uses per-user
storage. Preview signing is not claimed: SmartScreen may warn about an unsigned
build. Check its origin and SHA-256; do not disable Windows security features.

## Building

Windows x64, the .NET 10 SDK pinned in `global.json`, Python 3.10+, CMake 3.25+,
Ninja and a configured Windows C++ toolchain are required. Inno Setup 6 is
required for the installer; bootstrap can install pinned 6.5.4 per-user.

```powershell
powershell -NoProfile -File scripts/bootstrap.ps1
powershell -NoProfile -File scripts/verify.ps1
powershell -NoProfile -File scripts/package.ps1
```

Scripts support Windows PowerShell 5.1 / PowerShell 7 and do not install tools
globally. [Build instructions](docs/BUILDING.md), [architecture](docs/ARCHITECTURE.md),
[metrics](docs/METRICS.md), [testing](docs/TESTING.md) and
[release instructions](docs/RELEASING.md) contain the detailed contracts.

Development packages use `./scripts/package.ps1 -Configuration Debug`, separate
`artifacts/publish-debug/` / `artifacts/packages-debug/` directories and `-debug`
filenames. Debug is not a Release performance benchmark, a trusted signature,
or an exemption from Smart App Control. See [signing options](docs/SIGNING.md).

## License

Copyright (C) 2026 AnoY606-git and FrameScope contributors.
Own code: **[GPL-3.0-only](LICENSE)**, without warranty. Third-party code retains
its licenses; see [notices](THIRD_PARTY_NOTICES.md) and [attribution](ATTRIBUTION.md).
Repository: `AnoY606-git/framescope` (private).
