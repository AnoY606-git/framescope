# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$NativeBuildDirectory,
    [string]$NativeOutputDirectory,
    [string[]]$NativeRuntimeSearchPath = @(),
    [string[]]$NativeRuntimeNoticesPath = @(),
    [string[]]$CMakeArguments = @(),
    [switch]$SkipVerify,
    [switch]$PortableOnly,
    [switch]$AllowDirtySource
)
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-WindowsX64
$python = Resolve-Python
$paths = Get-FrameScopePaths -Configuration $Configuration -NativeBuildDirectory $NativeBuildDirectory `
    -NativeOutputDirectory $NativeOutputDirectory -CMakeArguments $CMakeArguments
$Configuration = $paths.Configuration
$publish = $paths.PublishDirectory
$output = $paths.PackageDirectory
Push-Location $script:RepositoryRoot
try {
    if (-not $SkipVerify) {
        & (Join-Path $PSScriptRoot 'verify.ps1') -Configuration $Configuration -NativeBuildDirectory $paths.NativeBuildDirectory `
            -NativeOutputDirectory $paths.NativeOutputDirectory `
            -NativeRuntimeSearchPath $NativeRuntimeSearchPath -NativeRuntimeNoticesPath $NativeRuntimeNoticesPath -CMakeArguments $CMakeArguments
    }
    $verified = Get-Content -LiteralPath $paths.VerificationPath -Raw | ConvertFrom-Json
    $built = Get-Content -LiteralPath (Join-Path $publish 'build-info.json') -Raw | ConvertFrom-Json
    if ($verified.version -ne $built.version -or $verified.source.tree_sha256 -ne $built.source.tree_sha256 -or
        $verified.configuration -ne $Configuration -or $built.configuration -ne $Configuration) {
        throw 'Verification marker does not match this publish. Run verify.ps1 again.'
    }
    Assert-PublishNativeCache -Paths $paths -Info $built
    Invoke-Checked $python @('scripts/package.py', '--publish-dir', $publish, '--configuration', $Configuration,
        '--verification', $paths.VerificationPath, '--check-publish')
    if (($built.source.dirty -or -not $built.source.commit) -and -not $AllowDirtySource) {
        throw 'Packaging needs a clean committed checkout; AllowDirtySource is explicit developer mode only.'
    }
    $version = Get-FrameScopeVersion
    $productVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $publish 'FrameScope.exe')).ProductVersion
    if ($productVersion -ne $version -and -not $productVersion.StartsWith($version + '+')) {
        throw 'FrameScope.exe product version does not match the source version.'
    }
    if (Test-Path -LiteralPath $output) {
        $previous = $output + '-previous-' + [Guid]::NewGuid().ToString('N')
        Move-Item -LiteralPath $output -Destination $previous
        Write-Host "Previous $Configuration packages preserved in a sibling *-previous-* directory."
    }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $arguments = @('scripts/package.py', '--publish-dir', $publish, '--output-dir', $output,
        '--configuration', $Configuration, '--verification', $paths.VerificationPath)
    if ($AllowDirtySource) { $arguments += '--allow-dirty-source' }
    if (-not $PortableOnly) {
        $iscc = Resolve-Iscc
        $compilerVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($iscc)
        if ($compilerVersion.FileMajorPart -ne 6 -or $compilerVersion.FileMinorPart -lt 3) {
            throw 'Use Inno Setup 6.3+ in the 6.x family.'
        }
        $installerDirectory = $paths.InstallerDirectory
        New-Item -ItemType Directory -Path $installerDirectory -Force | Out-Null
        Invoke-Checked $iscc @("/DAppVersion=$version", "/DBuildConfiguration=$Configuration",
            "/DPublishDir=$publish", "/DOutputDir=$installerDirectory", 'packaging/FrameScope.iss')
        $debugSuffix = ''
        if ($Configuration -eq 'Debug') { $debugSuffix = '-debug' }
        $arguments += @('--installer', (Join-Path $installerDirectory "FrameScope-$version$debugSuffix-win-x64-setup.exe"))
    }
    Invoke-Checked $python $arguments
}
finally { Pop-Location }
