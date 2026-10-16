param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [string]$Cxx,
    [switch]$LoaderSmoke,
    [string]$DemoExecutable
)

$ErrorActionPreference = 'Stop'
$owned = Split-Path $PSScriptRoot -Parent
$root = Split-Path (Split-Path $owned -Parent) -Parent
$build = (Resolve-Path $BuildDirectory).Path
$output = Join-Path $owned '.check'
if (-not $Cxx) {
    $entry = Get-Content (Join-Path $build 'CMakeCache.txt') |
        Where-Object { $_ -match '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=' } | Select-Object -First 1
    if (-not $entry) { throw 'Pass -Cxx or use a configured llvm-mingw build directory.' }
    $Cxx = $entry.Substring($entry.IndexOf('=') + 1)
}
if ($Cxx -notmatch 'mingw') { throw 'This local verification script targets llvm-mingw, not an unverified MSVC build.' }
$imgui = Join-Path $build '_deps/imgui-src'
$headers = Join-Path $build '_deps/vulkan_headers-src/include'
$libraries = @('libframescope_common.a', 'libimgui_vulkan.a', 'libimgui.a') |
    ForEach-Object { Join-Path $build $_ }
foreach ($path in @($Cxx, $imgui, $headers) + $libraries) {
    if (-not (Test-Path $path)) { throw "Missing existing build dependency: $path" }
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$flags = @('-std=c++20', '-O3', '-DNDEBUG', '-fstrict-aliasing',
    '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wno-missing-field-initializers',
    '-DUNICODE', '-D_UNICODE', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DVK_NO_PROTOTYPES',
    '-DVK_USE_PLATFORM_WIN32_KHR', '-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES',
    '-I', (Join-Path $root 'native/include'), '-I', $headers, '-I', $imgui,
    '-I', (Join-Path $imgui 'backends'))
$object = Join-Path $output 'layer.obj'
& $Cxx @flags '-c' (Join-Path $owned 'layer.cpp') '-o' $object
if ($LASTEXITCODE) { throw "Layer compilation failed: $LASTEXITCODE" }
$dll = Join-Path $output 'FrameScope.Vulkan.dll'
& $Cxx '-shared' '-static-libstdc++' '-static-libgcc' $object (Join-Path $owned 'FrameScope.Vulkan.def') @libraries '-o' $dll
if ($LASTEXITCODE) { throw "DLL linking failed: $LASTEXITCODE" }
Copy-Item (Join-Path $owned 'FrameScope.Vulkan.json') (Join-Path $output 'FrameScope.Vulkan.json') -Force
$tests = Join-Path $output 'policy_tests.exe'
& $Cxx @flags '-static-libstdc++' '-static-libgcc' (Join-Path $PSScriptRoot 'policy_tests.cc') @libraries '-o' $tests
if ($LASTEXITCODE) { throw "Test compilation failed: $LASTEXITCODE" }
& $tests
if ($LASTEXITCODE) { throw "Mock checks failed: $LASTEXITCODE" }
if ($LoaderSmoke) {
    $smoke = Join-Path $output 'FrameScope.Vulkan.Bootstrap.exe'
    # This source defines its own Windows/Vulkan macros; no duplicate -D warnings.
    & $Cxx '-std=c++20' '-Wall' '-Wextra' '-Werror' '-Wno-missing-field-initializers' '-I' $headers `
        '-static-libstdc++' '-static-libgcc' (Join-Path $PSScriptRoot 'loader_smoke.cc') '-o' $smoke
    if ($LASTEXITCODE) { throw "Loader smoke compilation failed: $LASTEXITCODE" }
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $smoke
    $start.WorkingDirectory = $output
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # Only the NEW test process receives these variables. No global or caller env change.
    $start.EnvironmentVariables['VK_LAYER_PATH'] = $output
    $start.EnvironmentVariables['VK_INSTANCE_LAYERS'] = 'VK_LAYER_FRAMESCOPE_hud'
    $start.EnvironmentVariables['VK_LOADER_DEBUG'] = 'error,warn'
    $process = [System.Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $process.Kill()
        throw 'Only our bootstrap test process was terminated after 30 seconds.'
    }
    Write-Output $stdout.GetAwaiter().GetResult()
    Write-Output $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -eq 77) { Write-Warning 'System loader unavailable; runtime bootstrap skipped.' }
    elseif ($process.ExitCode) { throw "Real loader smoke failed: $($process.ExitCode)" }
    $process.Dispose()
}
if ($DemoExecutable) {
    $demo = (Resolve-Path $DemoExecutable).Path
    $integration = Join-Path $output 'hud_smoke.exe'
    & $Cxx @flags '-static-libstdc++' '-static-libgcc' '-municode' `
        (Join-Path $PSScriptRoot 'hud_smoke.cc') @libraries '-o' $integration
    if ($LASTEXITCODE) { throw "HUD test compilation failed: $LASTEXITCODE" }
    & $integration $demo $output
    if ($LASTEXITCODE) { throw "Real demo HUD integration failed: $LASTEXITCODE" }
    & $integration $demo $output '--stale'
    if ($LASTEXITCODE) { throw "Stale-heartbeat demo integration failed: $LASTEXITCODE" }
}
