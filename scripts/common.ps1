# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
# Process-local helpers shared by build, verification and packaging.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:RepositoryRoot = Split-Path -Parent $PSScriptRoot
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'

function Assert-WindowsX64 {
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitProcess) {
        throw 'Use a 64-bit PowerShell process on Windows.'
    }
}

function Resolve-Tool {
    param([Parameter(Mandatory = $true)][string]$Name, [string]$Override)
    if ($Override) {
        if (-not (Test-Path -LiteralPath $Override -PathType Leaf)) {
            throw "Configured tool does not exist: $Name"
        }
        return (Resolve-Path -LiteralPath $Override).Path
    }
    $commands = @(Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue)
    if ($commands.Count -eq 0) { throw "Required tool not found: $Name. See docs/BUILDING.md." }
    return $commands[0].Source
}

function Resolve-DotNet {
    $override = $env:FRAMESCOPE_DOTNET
    if (-not $override) {
        $local = Join-Path $script:RepositoryRoot '.tools/dotnet/dotnet.exe'
        if (Test-Path -LiteralPath $local) { $override = $local }
    }
    return Resolve-Tool -Name 'dotnet.exe' -Override $override
}

function Resolve-Python {
    return Resolve-Tool -Name 'python.exe' -Override $env:FRAMESCOPE_PYTHON
}

function Get-ArtifactHash {
    param([string]$LiteralPath, [ValidateSet('SHA256', 'SHA512')][string]$Algorithm = 'SHA256')
    if ($Algorithm -eq 'SHA512') { $hasher = [Security.Cryptography.SHA512]::Create() }
    else { $hasher = [Security.Cryptography.SHA256]::Create() }
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $LiteralPath).Path)
        return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        if ($stream) { $stream.Dispose() }
        $hasher.Dispose()
    }
}

function Get-InnoSetupToolchain {
    return [pscustomobject]@{
        Version = '6.5.4'
        Url = 'https://github.com/jrsoftware/issrc/releases/download/is-6_5_4/innosetup-6.5.4.exe'
        Sha256 = 'fa73bf47a4da250d185d07561c2bfda387e5e20db77e4570004cf6a133cc10b1'
    }
}

function Resolve-Iscc {
    $override = $env:FSC_ISCC
    if (-not $override) { $override = $env:FRAMESCOPE_ISCC }
    if (-not $override) {
        $pin = Get-InnoSetupToolchain
        $local = Join-Path $script:RepositoryRoot ('.tools/inno-' + $pin.Version + '/ISCC.exe')
        if (Test-Path -LiteralPath $local -PathType Leaf) { $override = $local }
    }
    if (-not $override -and ${env:ProgramFiles(x86)}) {
        $installed = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'
        if (Test-Path -LiteralPath $installed -PathType Leaf) { $override = $installed }
    }
    return Resolve-Tool -Name 'ISCC.exe' -Override $override
}

function ConvertFrom-InnoVersionProbe {
    param([string]$Output, [int]$ExitCode)
    if ($ExitCode -ne 0) { throw "ISCC version probe failed with exit code $ExitCode. $Output" }
    $markers = [regex]::Matches($Output, '(?m)(?<![A-Za-z0-9_])FRAMESCOPE_ISCC_VERSION=([0-9]+)[ \t\r]*$')
    [uint32]$encoded = 0
    if ($markers.Count -ne 1 -or -not [uint32]::TryParse($markers[0].Groups[1].Value, [ref]$encoded)) {
        throw 'ISCC did not provide exactly one valid PREPROCVER marker; compiler version is unknown.'
    }
    $major = ($encoded -shr 24) -band 255
    $minor = ($encoded -shr 16) -band 255
    $revision = ($encoded -shr 8) -band 255
    $build = $encoded -band 255
    if ($major -eq 0) { throw 'ISCC reported an invalid zero major version.' }
    $version = '{0}.{1}.{2}' -f $major, $minor, $revision
    if ($build -ne 0) { $version += '.' + $build }
    return $version
}

function Get-InnoCompilerVersion {
    param([Parameter(Mandatory = $true)][string]$Compiler)
    # Official 6.5.4 binaries can have PE version 0.0.0.0. PREPROCVER is the vendor's
    # documented version: https://jrsoftware.org/ispphelp/topic_predefinedvars.htm
    # /? can exit 1 and is not a version API. Compile only our output-disabled stdin probe.
    $compilerPath = Resolve-Tool -Name 'ISCC.exe' -Override $Compiler
    $probe = [IO.File]::ReadAllText((Join-Path $script:RepositoryRoot 'packaging/version-probe.iss'))
    $temporaryBase = [IO.Path]::GetTempPath()
    $approved = Join-Path $temporaryBase 'opencode'
    if ([IO.Directory]::Exists($approved)) { $temporaryBase = $approved }
    $temporary = Join-Path $temporaryBase ('framescope-inno-version-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($temporary) | Out-Null
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $compilerPath
    $start.Arguments = '/O- -'
    $start.WorkingDirectory = $temporary
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'ISCC version probe did not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.Write($probe)
        $process.StandardInput.Close()
        $process.WaitForExit()
        return ConvertFrom-InnoVersionProbe -Output ($stdout.Result + "`n" + $stderr.Result) -ExitCode $process.ExitCode
    }
    finally {
        $process.Dispose()
        # Never recursively delete an unexpected compiler output; preserve it for inspection.
        try { [IO.Directory]::Delete($temporary, $false) }
        catch { Write-Verbose 'Nonempty version-probe temporary directory preserved for inspection.' }
    }
}

function Install-LocalInnoSetup {
    param([Parameter(Mandatory = $true)][string]$ToolsRoot)
    $pin = Get-InnoSetupToolchain
    $destination = [IO.Path]::GetFullPath((Join-Path $ToolsRoot ('inno-' + $pin.Version)))
    if (Test-Path -LiteralPath $destination) {
        throw 'Local Inno directory already exists; inspect it rather than overwriting an existing toolchain.'
    }
    New-Item -ItemType Directory -Path $ToolsRoot -Force | Out-Null
    $archive = Join-Path $ToolsRoot ('innosetup-' + $pin.Version + '.exe')
    if (Test-Path -LiteralPath $archive) {
        if ((Get-ArtifactHash -LiteralPath $archive -Algorithm SHA256) -ne $pin.Sha256) {
            throw 'Existing Inno installer has a different SHA-256; inspect it before retrying.'
        }
    }
    else {
        $partial = $archive + '.' + [Guid]::NewGuid().ToString('N') + '.partial'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $pin.Url -OutFile $partial -UseBasicParsing
        if ((Get-ArtifactHash -LiteralPath $partial -Algorithm SHA256) -ne $pin.Sha256) {
            throw 'Inno SHA-256 mismatch; installer was not executed. Inspect the partial download.'
        }
        Move-Item -LiteralPath $partial -Destination $archive
    }
    Invoke-Checked $archive @('/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
        '/NOICONS', '/TASKS=', ('/DIR=' + $destination))
    $compiler = Join-Path $destination 'ISCC.exe'
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'Installed Inno tool directory is missing ISCC.exe.' }
    $actualVersion = Get-InnoCompilerVersion -Compiler $compiler
    if ($actualVersion -ne $pin.Version) {
        throw "Installed ISCC version $actualVersion does not match pinned $($pin.Version); inspect the local tool directory."
    }
    if ($compiler -match '[\r\n]') { throw 'Tool path cannot contain a line break.' }
    $env:FSC_ISCC = $compiler
    if ($env:GITHUB_ENV) {
        [IO.File]::AppendAllText($env:GITHUB_ENV, "FSC_ISCC=$compiler`n", (New-Object Text.UTF8Encoding($false)))
    }
    Write-Host "Inno Setup $($pin.Version) installed per-user under .tools; PATH was not changed."
    return $compiler
}

function ConvertTo-NativeArgument {
    param([AllowEmptyString()][string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * ($slashes * 2 + 1)))
            [void]$builder.Append('"')
        }
        else {
            [void]$builder.Append(('\' * $slashes))
            [void]$builder.Append($character)
        }
        $slashes = 0
    }
    [void]$builder.Append(('\' * ($slashes * 2)))
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-Checked {
    param([Parameter(Mandatory = $true)][string]$Executable, [string[]]$Arguments = @())
    # Windows PowerShell 5.1 otherwise loses embedded quotes in native arguments.
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Executable
    $start.Arguments = (@($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Native process did not start.' }
        $standardOutput = $process.StandardOutput.ReadToEndAsync()
        $standardError = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $output = $standardOutput.Result
        $errors = $standardError.Result
        if ($output.Length -gt 0) { Write-Output $output.TrimEnd("`r", "`n") }
        if ($errors.Length -gt 0) { [Console]::Error.Write($errors) }
        if ($process.ExitCode -ne 0) {
            throw "$(Split-Path -Leaf $Executable) failed with exit code $($process.ExitCode)."
        }
    }
    finally { $process.Dispose() }
}

function Get-FrameScopeVersion {
    $versionFile = Join-Path $script:RepositoryRoot 'VERSION'
    if (Test-Path -LiteralPath $versionFile -PathType Leaf) {
        $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
        if ($version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') { throw 'VERSION must contain one semantic version.' }
        return $version
    }
    [xml]$properties = Get-Content -LiteralPath (Join-Path $script:RepositoryRoot 'Directory.Build.props') -Raw
    $nodes = @($properties.SelectNodes('//Version'))
    if ($nodes.Count -ne 1 -or $nodes[0].InnerText -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') {
        throw 'Directory.Build.props must define one explicit semantic Version.'
    }
    return $nodes[0].InnerText
}

function ConvertTo-RepositoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $script:RepositoryRoot $Path }
    return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Read-CMakeCache {
    param([Parameter(Mandatory = $true)][string]$NativeBuildDirectory)
    $cache = @{}
    $path = Join-Path $NativeBuildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $path -Encoding UTF8) {
            if ($line -match '^([^#/:=]+):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
        }
    }
    return $cache
}

function Get-CMakeDefinitions {
    param([string[]]$Arguments = @())
    $definitions = @{}
    for ($index = 0; $index -lt $Arguments.Count; $index++) {
        $argument = $Arguments[$index]
        if ($argument -match '^-[SBGH]' -or $argument -match '^--preset(?:=|$)') {
            throw 'CMakeArguments cannot override source/build/generator; use NativeBuildDirectory and the supported Ninja layout.'
        }
        if (-not $argument.StartsWith('-D')) { continue }
        $definition = $argument.Substring(2)
        if ($argument -eq '-D') {
            $index++
            if ($index -ge $Arguments.Count) { throw 'CMake -D requires a definition.' }
            $definition = $Arguments[$index]
        }
        if ($definition -notmatch '^([^:=\s]+)(?::[^=\s]+)?=(.*)$') { throw 'Invalid CMake -D definition.' }
        $name, $value = $Matches[1], $Matches[2]
        if ($name -in @('CMAKE_BUILD_TYPE', 'FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY') -and
            $definitions.ContainsKey($name) -and $definitions[$name] -cne $value) {
            throw "Conflicting CMake definitions: $name"
        }
        $definitions[$name] = $value
    }
    return $definitions
}

function Get-FrameScopePaths {
    param(
        [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
        [string]$NativeBuildDirectory,
        [string]$NativeOutputDirectory,
        [string[]]$CMakeArguments = @()
    )
    if ($Configuration -eq 'Debug') { $Configuration = 'Debug' }
    else { $Configuration = 'Release' }
    $suffix = ''
    $otherSuffix = '-debug'
    if ($Configuration -eq 'Debug') { $suffix = '-debug'; $otherSuffix = '' }
    $artifacts = Join-Path $script:RepositoryRoot 'artifacts'
    if (-not $NativeBuildDirectory) { $NativeBuildDirectory = Join-Path $artifacts ('native' + $suffix) }
    $NativeBuildDirectory = ConvertTo-RepositoryPath $NativeBuildDirectory
    $cache = Read-CMakeCache $NativeBuildDirectory
    $definitions = Get-CMakeDefinitions $CMakeArguments
    foreach ($settings in @($cache, $definitions)) {
        if ($settings.ContainsKey('CMAKE_BUILD_TYPE') -and $settings['CMAKE_BUILD_TYPE'] -cne $Configuration) {
            throw "Native configuration does not match $Configuration; use a separate build tree."
        }
    }
    if ($cache.ContainsKey('CMAKE_CONFIGURATION_TYPES') -and $cache['CMAKE_CONFIGURATION_TYPES']) {
        throw 'Use separate single-configuration Ninja build trees for Debug and Release.'
    }
    if ($cache.ContainsKey('CMAKE_GENERATOR') -and $cache['CMAKE_GENERATOR'] -ne 'Ninja') {
        throw 'This script requires a Ninja build tree; do not reuse another generator cache.'
    }
    if ($cache.ContainsKey('CMAKE_HOME_DIRECTORY') -and
        (ConvertTo-RepositoryPath $cache['CMAKE_HOME_DIRECTORY']) -ne (ConvertTo-RepositoryPath $script:RepositoryRoot)) {
        throw 'Native cache belongs to a different source checkout.'
    }
    $requested = $NativeOutputDirectory
    if ($definitions.ContainsKey('FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY')) {
        $fromArguments = $definitions['FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY']
        if (-not $fromArguments) { throw 'Native output directory must be an explicit nonempty path.' }
        if ($requested -and (ConvertTo-RepositoryPath $requested) -ne (ConvertTo-RepositoryPath $fromArguments)) {
            throw 'NativeOutputDirectory conflicts with CMakeArguments.'
        }
        $requested = $fromArguments
    }
    $cachedOutput = $cache['FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY']
    if ($requested -and $cachedOutput -and
        (ConvertTo-RepositoryPath $requested) -ne (ConvertTo-RepositoryPath $cachedOutput)) {
        throw 'Requested native output differs from the existing cache; inspect it or choose a fresh build tree.'
    }
    if ($requested) { $NativeOutputDirectory = $requested }
    elseif ($cachedOutput) { $NativeOutputDirectory = $cachedOutput }
    else { $NativeOutputDirectory = Join-Path $NativeBuildDirectory 'bin' }
    if ($NativeOutputDirectory -match '\$[<{]') { throw 'Native output must be a literal directory, not a generator expression.' }
    $NativeOutputDirectory = ConvertTo-RepositoryPath $NativeOutputDirectory
    $reserved = ConvertTo-RepositoryPath (Join-Path $artifacts ('native' + $otherSuffix))
    foreach ($directory in @($NativeBuildDirectory, $NativeOutputDirectory)) {
        if ($directory -eq $reserved -or $directory.StartsWith($reserved + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Debug and Release must not share the other configuration default native tree/output.'
        }
    }
    return [pscustomobject]@{
        Configuration = $Configuration
        NativeBuildDirectory = $NativeBuildDirectory
        NativeOutputDirectory = $NativeOutputDirectory
        PublishDirectory = Join-Path $artifacts ('publish' + $suffix)
        VerificationPath = Join-Path $artifacts ('verification' + $suffix + '.json')
        PackageDirectory = Join-Path $artifacts ('packages' + $suffix)
        InstallerDirectory = Join-Path $artifacts ('installer' + $suffix)
        TestResultsDirectory = Join-Path $artifacts ('test-results' + $suffix)
    }
}

function Assert-NativeConfiguration {
    param([Parameter(Mandatory = $true)]$Paths)
    $cache = Read-CMakeCache $Paths.NativeBuildDirectory
    if ($cache['CMAKE_BUILD_TYPE'] -cne $Paths.Configuration -or
        -not $cache['FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY']) {
        throw 'Native cache must record the requested configuration and FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY.'
    }
    if ((ConvertTo-RepositoryPath $cache['FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY']) -ne $Paths.NativeOutputDirectory) {
        throw 'Configured native output differs from the selected artifact directory; no helpers will be copied.'
    }
}

function Assert-PublishNativeCache {
    param([Parameter(Mandatory = $true)]$Paths, [Parameter(Mandatory = $true)]$Info)
    Assert-NativeConfiguration $Paths
    $nativeProperty = $Info.PSObject.Properties['native_cache']
    if ($Info.configuration -cne $Paths.Configuration -or -not $nativeProperty -or
        $nativeProperty.Value.configuration -cne $Paths.Configuration) {
        throw 'Publish metadata does not match the selected native configuration; rebuild this configuration.'
    }
    $cacheHash = Get-ArtifactHash -LiteralPath (Join-Path $Paths.NativeBuildDirectory 'CMakeCache.txt')
    if ($nativeProperty.Value.cache_sha256 -ne $cacheHash) {
        throw 'Publish was produced from a different native cache/options; rebuild before testing or packaging.'
    }
}

function Get-VisualStudioInstallation {
    if (-not ${env:ProgramFiles(x86)}) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { return }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio discovery failed; inspect vswhere rather than silently changing compiler.' }
    if ($installation) { return ([string](@($installation)[0])).Trim() }
}

function Initialize-MsvcEnvironment {
    param([Parameter(Mandatory = $true)][string]$Installation)
    $developer = Join-Path $Installation 'Common7/Tools/VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $developer -PathType Leaf)) { throw 'Installed Visual Studio is missing VsDevCmd.bat.' }
    if ($developer -match '["\r\n%!]') { throw 'Developer script path contains unsafe cmd quoting/expansion characters.' }
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    # Raw cmd quoting: PowerShell 7 must not reinterpret the nested /s /c quotes.
    $start.Arguments = '/d /s /c ""{0}" -no_logo -arch=amd64 -host_arch=amd64 >nul && set"' -f $developer
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'MSVC developer command process did not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $output = $stdout.Result
        $diagnostic = $stderr.Result.Trim()
        if ($process.ExitCode -ne 0) {
            throw "Unable to initialize process-local MSVC environment (cmd exit $($process.ExitCode)). $diagnostic"
        }
        # Never print `set` output: it contains inherited CI environment values.
        foreach ($line in ($output -split '\r?\n')) {
            $separator = $line.IndexOf('=')
            if ($separator -gt 0) {
                [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
            }
        }
    }
    finally { $process.Dispose() }
}

function Enable-CompilerEnvironment {
    if ($env:CXX -or $env:CC) { return }
    if (Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue) { return }
    $installation = Get-VisualStudioInstallation
    if ($installation) {
        Initialize-MsvcEnvironment -Installation $installation
        return
    }
    foreach ($pair in @(@('g++.exe', 'gcc.exe'), @('clang++.exe', 'clang.exe'))) {
        $cpp = Get-Command $pair[0] -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cpp) {
            $c = Get-Command $pair[1] -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $c) { throw 'Fallback C++ compiler has no matching C compiler; configure CC/CXX explicitly.' }
            $env:CXX = $cpp.Source
            $env:CC = $c.Source
            return
        }
    }
    throw 'No Windows C++ toolchain found. Configure CC/CXX or install VS C++ build tools separately.'
}

function Get-NativeRuntimeDirectories {
    param([string[]]$Additional = @())
    $directories = @($Additional)
    if ($env:FRAMESCOPE_NATIVE_RUNTIME_DIRS) { $directories += $env:FRAMESCOPE_NATIVE_RUNTIME_DIRS.Split(';') }
    if ($env:VCToolsRedistDir) {
        $redist = Join-Path $env:VCToolsRedistDir 'x64'
        if (Test-Path -LiteralPath $redist) {
            $directories += @(Get-ChildItem -LiteralPath $redist -Directory -Filter 'Microsoft.VC*.CRT' | ForEach-Object { $_.FullName })
        }
    }
    foreach ($name in @('clang++.exe', 'g++.exe')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { $directories += Split-Path -Parent $command.Source }
    }
    if ($env:CXX -and (Test-Path -LiteralPath $env:CXX -PathType Leaf)) {
        $directories += Split-Path -Parent $env:CXX
    }
    return @($directories | Where-Object { $_ } | Select-Object -Unique)
}

function Copy-DotNetRuntimeNotices {
    param([string]$PublishDirectory, [string]$DotNet)
    $configPath = Join-Path $PublishDirectory 'FrameScope.runtimeconfig.json'
    $runtimeConfig = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $frameworksProperty = $runtimeConfig.runtimeOptions.PSObject.Properties['includedFrameworks']
    if (-not $frameworksProperty) { throw 'Expected self-contained runtimeconfig with includedFrameworks.' }
    $nugetOutput = & $DotNet nuget locals global-packages --list --force-english-output
    if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve NuGet global packages directory.' }
    $match = [regex]::Match(($nugetOutput -join "`n"), '(?m)^global-packages:\s*(.+)$')
    if (-not $match.Success) { throw 'Unable to parse NuGet global packages directory.' }
    $packagesRoot = $match.Groups[1].Value.Trim()
    $required = @{
        'Microsoft.NETCore.App' = 'microsoft.netcore.app.runtime.win-x64'
        'Microsoft.WindowsDesktop.App' = 'microsoft.windowsdesktop.app.runtime.win-x64'
    }
    foreach ($frameworkName in $required.Keys) {
        $framework = @($frameworksProperty.Value | Where-Object { $_.name -eq $frameworkName })
        if ($framework.Count -ne 1) { throw "Self-contained runtime version not found: $frameworkName" }
        $packageName = $required[$frameworkName]
        $package = Join-Path (Join-Path $packagesRoot $packageName) $framework[0].version
        $destination = Join-Path (Join-Path $PublishDirectory 'licenses/dotnet') $packageName
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        $licenses = @(Get-ChildItem -LiteralPath $package -File | Where-Object {
            $_.Name -match '^(LICENSE|THIRD.PARTY.NOTICES)(\..*)?$'
        })
        if ($licenses.Count -eq 0) { throw "Runtime package license/notices not found: $packageName" }
        foreach ($license in $licenses) { Copy-Item -LiteralPath $license.FullName -Destination $destination }
    }
}

function Copy-NativeRuntimeNotices {
    param([string]$PublishDirectory, [string[]]$NoticeDirectories = @())
    $manifest = Get-Content -LiteralPath (Join-Path $PublishDirectory 'native/native-runtime.json') -Raw | ConvertFrom-Json
    if (@($manifest.files | Where-Object { $_.runtime }).Count -eq 0) { return }
    $directories = @($NoticeDirectories)
    if ($env:FRAMESCOPE_NATIVE_NOTICES_DIR) { $directories += $env:FRAMESCOPE_NATIVE_NOTICES_DIR.Split(';') }
    if ($directories.Count -eq 0) {
        throw 'Dynamic native runtime found. Supply -NativeRuntimeNoticesPath with its redistribution licenses/notices.'
    }
    $destination = Join-Path $PublishDirectory 'licenses/native-runtime'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $index = 0
    foreach ($directory in $directories) {
        $index++
        $resolved = (Resolve-Path -LiteralPath $directory).Path.TrimEnd('\', '/')
        $files = @(Get-ChildItem -LiteralPath $resolved -Recurse -File | Where-Object {
            $_.Name -match '(?i)(license|copying|copyright|notice)' -and $_.Extension -in @('', '.txt', '.md', '.rst')
        })
        if ($files.Count -eq 0) { throw 'Native runtime notice directory contains no recognized license files.' }
        foreach ($file in $files) {
            if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Runtime license symlinks are not accepted.' }
            $relative = $file.FullName.Substring($resolved.Length).TrimStart('\', '/')
            $target = Join-Path (Join-Path $destination ([string]$index)) $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
    }
}
