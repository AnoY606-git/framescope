# SPDX-License-Identifier: GPL-3.0-only
param([Parameter(Mandatory = $true)][string]$Glslang)
$ErrorActionPreference = 'Stop'
foreach ($stage in @('vert', 'frag')) {
    $name = if ($stage -eq 'vert') { 'kSceneVertexSpv' } else { 'kSceneFragmentSpv' }
    $output = Join-Path $PSScriptRoot "scene_${stage}_spv.hpp"
    & $Glslang -V --target-env vulkan1.0 --vn $name -o $output (Join-Path $PSScriptRoot "scene.$stage")
    if ($LASTEXITCODE -ne 0) { throw "glslang failed for $stage" }
    $text = [IO.File]::ReadAllText($output)
    [IO.File]::WriteAllText($output, "// SPDX-License-Identifier: GPL-3.0-only`n// Generated from scene.$stage by glslang; run regenerate.ps1 to rebuild.`n" + $text, [Text.UTF8Encoding]::new($false))
}
