# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$NativeBuildDirectory,
    [string]$NativeOutputDirectory,
    [string[]]$NativeRuntimeSearchPath = @(),
    [string[]]$NativeRuntimeNoticesPath = @(),
    [string[]]$CMakeArguments = @()
)
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-WindowsX64
$paths = Get-FrameScopePaths -Configuration $Configuration -NativeBuildDirectory $NativeBuildDirectory `
    -NativeOutputDirectory $NativeOutputDirectory -CMakeArguments $CMakeArguments
$Configuration = $paths.Configuration
$NativeBuildDirectory = $paths.NativeBuildDirectory
$publish = $paths.PublishDirectory
$dotnet = Resolve-DotNet
$python = Resolve-Python
Push-Location $script:RepositoryRoot
try {
    Enable-CompilerEnvironment
    $cmake = Resolve-Tool -Name 'cmake.exe' -Override $env:FRAMESCOPE_CMAKE
    $ninja = Resolve-Tool -Name 'ninja.exe' -Override $env:FRAMESCOPE_NINJA
    $version = Get-FrameScopeVersion
    $sdk = & $dotnet --version
    if ($LASTEXITCODE -ne 0 -or $sdk -notmatch '^10\.') { throw 'A working .NET 10 SDK is required.' }
    $configure = @('-S', $script:RepositoryRoot, '-B', $NativeBuildDirectory, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_MAKE_PROGRAM=$ninja", '-DBUILD_TESTING=ON',
        "-DFRAMESCOPE_NATIVE_OUTPUT_DIRECTORY:PATH=$($paths.NativeOutputDirectory)")
    if ($Configuration -eq 'Release') { $configure += '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' }
    else { $configure += '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug' }
    Invoke-Checked $cmake ($configure + $CMakeArguments)
    Assert-NativeConfiguration $paths
    Invoke-Checked $cmake @('--build', $NativeBuildDirectory, '--config', $Configuration, '--parallel', '2')

    $application = Join-Path $script:RepositoryRoot 'src/FrameScope.App/FrameScope.App.csproj'
    Invoke-Checked $dotnet @('build', $application, '--configuration', $Configuration)
    $tests = @(Get-ChildItem -Path (Join-Path $script:RepositoryRoot 'tests') -Recurse -Filter '*.Tests.csproj')
    if ($tests.Count -eq 0) { throw 'No managed test projects found.' }
    foreach ($test in $tests) { Invoke-Checked $dotnet @('build', $test.FullName, '--configuration', $Configuration) }

    # Use a new exact output directory so stale helpers cannot be republished.
    if (Test-Path -LiteralPath $publish) {
        $previous = $publish + '-previous-' + [Guid]::NewGuid().ToString('N')
        Move-Item -LiteralPath $publish -Destination $previous
        Write-Host "Previous $Configuration publish output preserved in a sibling *-previous-* directory."
    }
    Invoke-Checked $dotnet @('publish', $application, '--configuration', $Configuration,
        '--runtime', 'win-x64', '--self-contained', 'true', '--output', $publish,
        '-p:PublishSingleFile=false', '-p:PublishTrimmed=false', "-p:Version=$version")

    $runtimeArguments = @((Join-Path $PSScriptRoot 'native_runtime.py'), '--source',
        $paths.NativeOutputDirectory, '--destination', (Join-Path $publish 'native'))
    foreach ($directory in (Get-NativeRuntimeDirectories -Additional $NativeRuntimeSearchPath)) {
        $runtimeArguments += @('--search', $directory)
    }
    Invoke-Checked $python $runtimeArguments
    foreach ($name in @('LICENSE', 'NOTICE', 'AUTHORS.md', 'ATTRIBUTION.md', 'THIRD_PARTY_NOTICES.md', 'README.md', 'README.en.md', 'SECURITY.md')) {
        Copy-Item -LiteralPath (Join-Path $script:RepositoryRoot $name) -Destination $publish
    }
    Copy-Item -LiteralPath (Join-Path $script:RepositoryRoot 'docs') -Destination (Join-Path $publish 'docs') -Recurse
    New-Item -ItemType Directory -Path (Join-Path $publish 'licenses') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $script:RepositoryRoot 'docs/licenses') -Destination (Join-Path $publish 'licenses/third-party') -Recurse
    Copy-DotNetRuntimeNotices -PublishDirectory $publish -DotNet $dotnet
    Copy-NativeRuntimeNotices -PublishDirectory $publish -NoticeDirectories $NativeRuntimeNoticesPath
    Invoke-Checked $python @((Join-Path $PSScriptRoot 'build_info.py'), '--publish-dir', $publish,
        '--native-build', $NativeBuildDirectory, '--native-output', $paths.NativeOutputDirectory,
        '--configuration', $Configuration, '--sdk-version', $sdk)
    Write-Host "Built FrameScope $version ($Configuration, win-x64): $publish"
}
finally { Pop-Location }
