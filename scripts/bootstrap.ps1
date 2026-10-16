# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
# No machine-level changes. An optional SDK ZIP is verified against Microsoft metadata.
[CmdletBinding()]
param([switch]$InstallDotNet, [switch]$InstallInno)
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-WindowsX64
$sdk = (Get-Content -LiteralPath (Join-Path $script:RepositoryRoot 'global.json') -Raw | ConvertFrom-Json).sdk.version
if ($sdk -notmatch '^10\.\d+\.\d+$') { throw 'global.json must pin a stable .NET 10 SDK version.' }
if ($InstallDotNet) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $metadataUrl = 'https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json'
    $metadata = Invoke-RestMethod -Uri $metadataUrl
    $matches = @($metadata.releases | ForEach-Object {
        foreach ($item in $_.sdks) { if ($item.version -eq $sdk) { $item } }
    })
    $files = @($matches | ForEach-Object { $_.files } | Where-Object { $_.rid -eq 'win-x64' -and $_.name -match '\.zip$' })
    $unique = @($files | Sort-Object -Property url -Unique)
    if ($unique.Count -ne 1 -or $unique[0].hash -notmatch '^[0-9A-Fa-f]{128}$' -or
        $unique[0].url -notmatch '^https://(?:builds\.dotnet\.microsoft\.com|download\.visualstudio\.microsoft\.com)/') {
        throw 'Unable to select one verified Microsoft SDK archive.'
    }
    $tools = Join-Path $script:RepositoryRoot '.tools'
    New-Item -ItemType Directory -Path $tools -Force | Out-Null
    $archive = Join-Path $tools "dotnet-sdk-$sdk-win-x64.zip"
    if (-not (Test-Path -LiteralPath $archive) -or
        (Get-ArtifactHash -LiteralPath $archive -Algorithm SHA512) -ne $unique[0].hash) {
        Invoke-WebRequest -Uri $unique[0].url -OutFile $archive -UseBasicParsing
    }
    if ((Get-ArtifactHash -LiteralPath $archive -Algorithm SHA512) -ne $unique[0].hash) {
        throw 'SDK SHA-512 mismatch; archive was not extracted.'
    }
    $destination = Join-Path $tools 'dotnet'
    if (Test-Path -LiteralPath $destination) {
        throw 'Local .tools/dotnet already exists; inspect it rather than overwriting an existing toolchain.'
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $destination
    Write-Host "Installed SDK $sdk under .tools/dotnet only. Global PATH/settings were not changed."
}
if ($InstallInno) {
    Install-LocalInnoSetup -ToolsRoot (Join-Path $script:RepositoryRoot '.tools') | Out-Null
}
Push-Location $script:RepositoryRoot
try {
    $dotnet = Resolve-DotNet
    Invoke-Checked $dotnet @('--version')
    $python = Resolve-Python
    Invoke-Checked $python @('--version')
    Write-Host 'Configure CMake, Ninja and an x64 Windows compiler; build.ps1 can initialize installed VS C++ tools in-process.'
    Write-Host 'Use -InstallInno for pinned per-user Inno Setup 6.5.4; no global PATH changes. See docs/BUILDING.md.'
}
finally { Pop-Location }
