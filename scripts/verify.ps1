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
    [switch]$SkipBuild
)
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-WindowsX64
$paths = Get-FrameScopePaths -Configuration $Configuration -NativeBuildDirectory $NativeBuildDirectory `
    -NativeOutputDirectory $NativeOutputDirectory -CMakeArguments $CMakeArguments
$Configuration = $paths.Configuration
$NativeBuildDirectory = $paths.NativeBuildDirectory
$dotnet = Resolve-DotNet
$python = Resolve-Python
Push-Location $script:RepositoryRoot
try {
    if (Test-Path -LiteralPath $paths.VerificationPath) {
        Move-Item -LiteralPath $paths.VerificationPath -Destination ($paths.VerificationPath + '.previous-' + [Guid]::NewGuid().ToString('N'))
    }
    Invoke-Checked $python @('scripts/run_python_tests.py', '--output', (Join-Path $paths.TestResultsDirectory 'python/unittest.xml'))
    Invoke-Checked $python @('scripts/check_repository.py', '--include-untracked')
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -NativeBuildDirectory $NativeBuildDirectory `
            -NativeOutputDirectory $paths.NativeOutputDirectory `
            -NativeRuntimeSearchPath $NativeRuntimeSearchPath -NativeRuntimeNoticesPath $NativeRuntimeNoticesPath -CMakeArguments $CMakeArguments
    }
    $infoPath = Join-Path $paths.PublishDirectory 'build-info.json'
    $info = Get-Content -LiteralPath $infoPath -Raw | ConvertFrom-Json
    $buildInfoHash = Get-ArtifactHash -LiteralPath $infoPath
    Assert-PublishNativeCache -Paths $paths -Info $info
    Invoke-Checked $python @('scripts/package.py', '--publish-dir', $paths.PublishDirectory,
        '--configuration', $Configuration, '--check-publish')
    $managedReports = Join-Path $paths.TestResultsDirectory 'managed'
    $nativeReports = Join-Path $paths.TestResultsDirectory 'native'
    New-Item -ItemType Directory -Path $managedReports, $nativeReports -Force | Out-Null
    $projects = @(Get-ChildItem -Path (Join-Path $script:RepositoryRoot 'tests') -Recurse -Filter '*.Tests.csproj')
    if ($projects.Count -eq 0) { throw 'No managed test projects found.' }
    foreach ($project in $projects) {
        [xml]$xml = Get-Content -LiteralPath $project.FullName -Raw
        $isTestProject = $xml.SelectSingleNode('//IsTestProject[text()="true"]')
        if ($isTestProject) {
            Invoke-Checked $dotnet @('test', $project.FullName, '--configuration', $Configuration,
                '--no-build', '--no-restore', '--logger', 'trx', '--results-directory', $managedReports)
        }
        elseif ($xml.SelectSingleNode('//OutputType[text()="Exe"]')) {
            Invoke-Checked $dotnet @('run', '--project', $project.FullName, '--configuration', $Configuration,
                '--no-build', '--no-restore') | Tee-Object -FilePath (Join-Path $managedReports ($project.BaseName + '.log'))
        }
        else { throw "Unknown test execution contract: $($project.Name)" }
    }
    $ctest = Resolve-Tool -Name 'ctest.exe' -Override $env:FRAMESCOPE_CTEST
    Invoke-Checked $ctest @('--test-dir', $NativeBuildDirectory, '-C', $Configuration,
        '--output-on-failure', '--no-tests=error', '--output-junit', (Join-Path $nativeReports 'ctest.xml'))
    Assert-PublishNativeCache -Paths $paths -Info $info
    Invoke-Checked $python @('scripts/package.py', '--publish-dir', $paths.PublishDirectory,
        '--configuration', $Configuration, '--check-publish')
    if ((Get-ArtifactHash -LiteralPath $infoPath) -ne $buildInfoHash) {
        throw 'Publish metadata changed during verification; rerun with a stable build tree.'
    }
    $verification = [ordered]@{
        version = $info.version
        source = $info.source
        configuration = $Configuration
        native_cache = $info.native_cache
        build_info_sha256 = $buildInfoHash
        completed_utc = [DateTime]::UtcNow.ToString('o')
        checks = @('python-behavioral-tests', 'repository-policy', 'managed-tests', 'native-ctest', 'build-publish')
        hardware_validation = 'not-run-by-this-script'
    }
    $verification | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $paths.VerificationPath -Encoding UTF8
    Write-Host "Automated $Configuration verification completed: $($paths.VerificationPath). No hardware/trust claim is implied."
}
finally { Pop-Location }
