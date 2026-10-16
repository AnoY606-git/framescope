# SPDX-License-Identifier: GPL-3.0-only
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [switch]$Legacy
)

$ErrorActionPreference = 'Stop'

function Start-Probe([string]$Arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    return $process
}

function Invoke-Probe([string]$Arguments) {
    $process = Start-Probe $Arguments
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errorOutput = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw 'Probe timeout'
        }
        if ($errorOutput.Result.Length -ne 0) { throw 'Unexpected stderr output' }
        $lines = @($output.Result.Split([char]10) | Where-Object { $_.Trim().Length -gt 0 })
        if ($lines.Length -ne 1) { throw 'Expected exactly one JSON line' }
        return [pscustomobject]@{ ExitCode=$process.ExitCode; Data=($lines[0] | ConvertFrom-Json) }
    }
    finally {
        if (-not $process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
}

function Assert-Snapshot($Data) {
    if ($Data.schemaVersion -ne 1 -or $Data.source -ne 'level-zero-sysman') {
        throw 'Invalid schema/source'
    }
    if ([string]::IsNullOrEmpty($Data.timestamp)) { throw 'Missing timestamp' }
    foreach ($device in $Data.devices) {
        if ($device.vendorId -ne 0x8086) { throw 'Non-Intel device mislabeled as Intel' }
        if ([string]::IsNullOrEmpty($device.id)) { throw 'Missing device identity' }
        foreach ($property in @('uuid', 'serialNumber', 'boardNumber')) {
            if ($device.PSObject.Properties.Name -contains $property) { throw 'Unexpected unique hardware identifier' }
        }
        foreach ($reading in $device.readings) {
            if ($reading.status -eq 'available') {
                if ($null -eq $reading.value) { throw 'Available reading without value' }
                $number = [double]$reading.value
                if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw 'Nonfinite reading' }
            }
            elseif ($null -ne $reading.value) { throw 'Unavailable reading leaked numeric value' }
            if ($reading.key -eq 'fan.speed' -and $null -ne $reading.value) {
                throw 'Unsafe Windows Sysman fan path must remain disabled'
            }
        }
    }
}

$oldSysmanEnvironment = [Environment]::GetEnvironmentVariable('ZES_ENABLE_SYSMAN', 'Process')
$mode = if ($Legacy) { ' --legacy' } else { '' }
$once = Invoke-Probe ('--once --interval-ms 500' + $mode)
Assert-Snapshot $once.Data
if (@(0,3) -notcontains $once.ExitCode) { throw 'Unexpected once exit code' }

foreach ($arguments in @('--interval-ms 499', '--interval-ms 5001', '--interval-ms NaN',
                          '--interval-ms', '--once --watch', '--unknown')) {
    $invalid = Invoke-Probe $arguments
    if ($invalid.ExitCode -ne 2 -or $invalid.Data.backend.status -ne 'error') {
        throw "Argument validation failed: $arguments"
    }
    Assert-Snapshot $invalid.Data
}

$watch = Start-Probe ('--watch --interval-ms 500' + $mode)
try {
    for ($index = 1; $index -le 3; $index++) {
        $pending = $watch.StandardOutput.ReadLineAsync()
        if (-not $pending.Wait(30000)) { throw 'Watch line timeout' }
        if ($null -eq $pending.Result) { throw 'Watch ended prematurely' }
        $sample = $pending.Result | ConvertFrom-Json
        Assert-Snapshot $sample
        if ($sample.sequence -ne $index) { throw 'Watch sequence mismatch' }
    }
    # Закрываем только pipe нашего дочернего probe, не процессы пользователя.
    $watch.StandardOutput.Close()
    if (-not $watch.WaitForExit(7000)) { throw 'Watch did not stop after parent closed stdout pipe' }
    if ($watch.ExitCode -ne 0) { throw 'Closed pipe is not a normal exit' }
}
finally {
    if (-not $watch.HasExited) { $watch.Kill() }
    $watch.Dispose()
}

if ([Environment]::GetEnvironmentVariable('ZES_ENABLE_SYSMAN', 'Process') -cne $oldSysmanEnvironment) {
    throw 'Child changed parent process environment'
}

[pscustomobject]@{
    Result = 'PASS'
    Mode = $once.Data.backend.mode
    Backend = $once.Data.backend.status
    Devices = @($once.Data.devices).Count
    Checks = 'once JSON; six invalid arguments; watch JSON/sequence; closed pipe; parent environment; null/finite/privacy/fan invariants'
} | ConvertTo-Json
