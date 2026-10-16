# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise actual helper functions inline without changing host execution policy."""
import base64
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

from test_support import existing_directory_alias, temporary_directory


@unittest.skipUnless(os.name == "nt", "Windows PowerShell contract tests need Windows")
class PowerShellContracts(unittest.TestCase):
    def trusted_inno(self):
        # Only an explicitly supplied verified tool or bootstrap's pinned local tool.
        # Never download/discover arbitrary executables for an integration test.
        selected = os.environ.get("FRAMESCOPE_TEST_ISCC") or os.environ.get("FSC_ISCC")
        if not selected:
            local = Path(__file__).resolve().parents[2] / ".tools/inno-6.5.4/ISCC.exe"
            if local.is_file():
                selected = str(local)
        if not selected:
            self.skipTest("Set FRAMESCOPE_TEST_ISCC to an already verified Inno 6.5.4 compiler")
        compiler = Path(selected)
        self.assertTrue(compiler.is_file(), "Selected integration compiler is missing")
        return compiler.resolve()

    def invoke(self, body, executable=None):
        executable = executable or shutil.which("powershell.exe")
        self.assertIsNotNone(executable)
        common = Path(__file__).resolve().parents[1] / "common.ps1"
        prefix = """
$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath '%s' -Raw
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { throw 'Common helper syntax error.' }
$definitions = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)
foreach ($definition in $definitions) { . ([ScriptBlock]::Create($definition.Extent.Text)) }
""" % str(common).replace("'", "''")
        encoded = base64.b64encode((prefix + body).encode("utf-16-le")).decode("ascii")
        result = subprocess.run([executable, "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                                capture_output=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr.decode(errors="replace"))
        return result.stdout.decode(errors="replace")

    def test_resolves_one_tool_and_preserves_argument_boundaries(self):
        python = sys.executable.replace("'", "''")
        output = self.invoke("""
$tool = Resolve-Tool -Name python.exe -Override '%s'
if ($tool -isnot [string]) { throw 'Tool resolution must return one path.' }
Invoke-Checked $tool @('-c', 'import sys; assert sys.argv[1:] == ["a b", "c"]', 'a b', 'c')
Write-Output 'arguments-ok'
""" % python)
        self.assertIn("arguments-ok", output)

    def test_nonzero_native_exit_is_not_reported_as_success(self):
        python = sys.executable.replace("'", "''")
        output = self.invoke("""
$caught = $false
try { Invoke-Checked '%s' @('-c', 'raise SystemExit(7)') }
catch { $caught = $true }
if (-not $caught) { throw 'Nonzero native exit was ignored.' }
Write-Output 'exit-ok'
""" % python)
        self.assertIn("exit-ok", output)

    def test_reads_version_from_checkout_with_spaces(self):
        with temporary_directory() as directory:
            root = Path(directory) / "checkout with spaces"
            root.mkdir()
            (root / "VERSION").write_text("0.1.0-preview\n")
            output = self.invoke("""
$script:RepositoryRoot = '%s'
if ((Get-FrameScopeVersion) -ne '0.1.0-preview') { throw 'Wrong version.' }
Write-Output 'version-ok'
""" % str(root).replace("'", "''"))
            self.assertIn("version-ok", output)

    def test_explicit_cxx_or_cc_is_never_replaced(self):
        output = self.invoke("""
function Get-VisualStudioInstallation { throw 'Explicit toolchain must bypass discovery.' }
function Get-Command { throw 'Explicit toolchain must bypass PATH discovery.' }
foreach ($name in @('CXX', 'CC')) {
    $env:CXX = $null
    $env:CC = $null
    [Environment]::SetEnvironmentVariable($name, 'X:\\explicit-toolchain\\compiler.exe', 'Process')
    Enable-CompilerEnvironment
    if ([Environment]::GetEnvironmentVariable($name, 'Process') -ne 'X:\\explicit-toolchain\\compiler.exe') {
        throw 'Explicit compiler was replaced.'
    }
}
Write-Output 'explicit-ok'
""")
        self.assertIn("explicit-ok", output)

    def test_installed_msvc_is_preferred_over_gpp_on_path(self):
        output = self.invoke("""
$env:CXX = $null
$env:CC = $null
$script:initialized = $false
function Get-Command {
    param($Name, $CommandType, $ErrorAction)
    if ($Name -eq 'g++.exe') { return [pscustomobject]@{ Source = 'X:\\mingw\\g++.exe' } }
}
function Get-VisualStudioInstallation { return 'X:\\VS with spaces' }
function Initialize-MsvcEnvironment {
    param($Installation)
    if ($Installation -ne 'X:\\VS with spaces') { throw 'Wrong VS instance.' }
    $script:initialized = $true
}
Enable-CompilerEnvironment
if (-not $script:initialized -or $env:CXX) { throw 'MSVC did not take precedence over g++.' }
Write-Output 'msvc-ok'
""")
        self.assertIn("msvc-ok", output)

    def test_existing_cl_environment_does_not_rediscover_vs(self):
        output = self.invoke("""
$env:CXX = $null
$env:CC = $null
function Get-Command {
    param($Name, $CommandType, $ErrorAction)
    if ($Name -eq 'cl.exe') { return [pscustomobject]@{ Source = 'X:\\VS\\cl.exe' } }
}
function Get-VisualStudioInstallation { throw 'Existing developer environment must be preserved.' }
Enable-CompilerEnvironment
Write-Output 'developer-shell-ok'
""")
        self.assertIn("developer-shell-ok", output)

    def test_gpp_fallback_only_when_vs_is_absent(self):
        output = self.invoke("""
$env:CXX = $null
$env:CC = $null
function Get-VisualStudioInstallation { return $null }
function Initialize-MsvcEnvironment { throw 'No VS was discovered.' }
function Get-Command {
    param($Name, $CommandType, $ErrorAction)
    if ($Name -in @('g++.exe', 'gcc.exe')) { return [pscustomobject]@{ Source = 'X:\\mingw\\' + $Name } }
}
Enable-CompilerEnvironment
if ($env:CXX -ne 'X:\\mingw\\g++.exe' -or $env:CC -ne 'X:\\mingw\\gcc.exe') { throw 'Wrong fallback compiler pair.' }
Write-Output 'fallback-ok'
""")
        self.assertIn("fallback-ok", output)

    def test_broken_vs_does_not_silently_fall_back_to_gpp(self):
        output = self.invoke("""
$env:CXX = $null
$env:CC = $null
function Get-Command {
    param($Name, $CommandType, $ErrorAction)
    if ($Name -eq 'g++.exe') { return [pscustomobject]@{ Source = 'X:\\mingw\\g++.exe' } }
}
function Get-VisualStudioInstallation { return 'X:\\VS' }
function Initialize-MsvcEnvironment { param($Installation) throw 'Synthetic initialization failure.' }
$caught = $false
try { Enable-CompilerEnvironment } catch { $caught = $true }
if (-not $caught -or $env:CXX) { throw 'Broken VS was silently replaced.' }
Write-Output 'broken-vs-ok'
""")
        self.assertIn("broken-vs-ok", output)

    def test_real_developer_batch_with_spaces_and_metacharacters_imports_process_environment(self):
        hosts = [shutil.which("powershell.exe")]
        if shutil.which("pwsh.exe"):
            hosts.append(shutil.which("pwsh.exe"))
        with temporary_directory() as directory:
            installation = Path(directory) / "VS fixture with spaces & (tools)"
            developer = installation / "Common7/Tools/VsDevCmd.bat"
            developer.parent.mkdir(parents=True)
            developer.write_text(
                '@echo off\n'
                'if not "%~1"=="-no_logo" exit /b 11\n'
                'if not "%*"=="-no_logo -arch=amd64 -host_arch=amd64" exit /b 12\n'
                'set "FRAMESCOPE_VS_FIXTURE=spaces & equals=preserved"\n'
                'exit /b 0\n', encoding="ascii", newline="\r\n")
            for host in hosts:
                with self.subTest(host=host):
                    output = self.invoke("""
$userBefore = [Environment]::GetEnvironmentVariable('FRAMESCOPE_VS_FIXTURE', 'User')
$machineBefore = [Environment]::GetEnvironmentVariable('FRAMESCOPE_VS_FIXTURE', 'Machine')
$env:FRAMESCOPE_VS_FIXTURE = 'parent-process-value'
Initialize-MsvcEnvironment -Installation '%s'
if ($env:FRAMESCOPE_VS_FIXTURE -ne 'spaces & equals=preserved') { throw 'Child environment was not imported intact.' }
if ([Environment]::GetEnvironmentVariable('FRAMESCOPE_VS_FIXTURE', 'User') -ne $userBefore -or
    [Environment]::GetEnvironmentVariable('FRAMESCOPE_VS_FIXTURE', 'Machine') -ne $machineBefore) {
    throw 'Persistent environment was modified.'
}
Write-Output ('real-developer-env-ok:' + $PSVersionTable.PSVersion.Major)
""" % str(installation).replace("'", "''"), executable=host)
                    self.assertIn("real-developer-env-ok:", output)

    def test_real_failed_developer_batch_preserves_parent_value_and_reports_exit_stderr(self):
        hosts = [shutil.which("powershell.exe")]
        if shutil.which("pwsh.exe"):
            hosts.append(shutil.which("pwsh.exe"))
        with temporary_directory() as directory:
            installation = Path(directory) / "VS failure fixture with spaces"
            developer = installation / "Common7/Tools/VsDevCmd.bat"
            developer.parent.mkdir(parents=True)
            developer.write_text('@echo off\nset "FRAMESCOPE_VS_FIXTURE=must-not-import"\n'
                                 'echo synthetic-vs-error 1>&2\nexit /b 23\n', encoding="ascii", newline="\r\n")
            for host in hosts:
                with self.subTest(host=host):
                    output = self.invoke("""
$env:FRAMESCOPE_VS_FIXTURE = 'unchanged-parent-value'
$caught = $false
try { Initialize-MsvcEnvironment -Installation '%s' }
catch { $caught = $_.Exception.Message -like '*23*synthetic-vs-error*' }
if (-not $caught) { throw 'Actual cmd failure did not retain its exit code and diagnostic.' }
if ($env:FRAMESCOPE_VS_FIXTURE -ne 'unchanged-parent-value') { throw 'Failed child environment was imported.' }
Write-Output ('real-developer-failure-ok:' + $PSVersionTable.PSVersion.Major)
""" % str(installation).replace("'", "''"), executable=host)
                    self.assertIn("real-developer-failure-ok:", output)

    def test_official_inno_pin_and_explicit_iscc_precedence(self):
        python = sys.executable.replace("'", "''")
        output = self.invoke("""
$pin = Get-InnoSetupToolchain
if ($pin.Version -ne '6.5.4' -or
    $pin.Url -ne 'https://github.com/jrsoftware/issrc/releases/download/is-6_5_4/innosetup-6.5.4.exe' -or
    $pin.Sha256 -ne 'fa73bf47a4da250d185d07561c2bfda387e5e20db77e4570004cf6a133cc10b1') { throw 'Inno pin changed.' }
$env:FSC_ISCC = '%s'
$env:FRAMESCOPE_ISCC = 'X:\\not-the-selected-compiler.exe'
if ((Resolve-Iscc) -ne $env:FSC_ISCC) { throw 'FSC_ISCC must take precedence.' }
Write-Output 'inno-pin-ok'
""" % python)
        self.assertIn("inno-pin-ok", output)

    def test_inno_probe_decodes_vendor_version_and_rejects_help_failure_or_ambiguity(self):
        output = self.invoke("""
$encoded = (6 -shl 24) -bor (4 -shl 16) -bor (2 -shl 8)
$text = "   Line 4: FRAMESCOPE_ISCC_VERSION=$encoded`r`r`n"
if ((ConvertFrom-InnoVersionProbe -Output $text -ExitCode 0) -ne '6.4.2') { throw 'Vendor version was not decoded.' }
$cases = @(
    @{ Text = 'Inno Setup 6 Command-Line Compiler'; Code = 1 },
    @{ Text = 'Inno Setup 6 Command-Line Compiler'; Code = 0 },
    @{ Text = 'FRAMESCOPE_ISCC_VERSION=0'; Code = 0 },
    @{ Text = 'FRAMESCOPE_ISCC_VERSION=4294967296'; Code = 0 },
    @{ Text = $text + $text; Code = 0 },
    @{ Text = $text; Code = 2 }
)
foreach ($case in $cases) {
    $caught = $false
    try { ConvertFrom-InnoVersionProbe -Output $case.Text -ExitCode $case.Code } catch { $caught = $true }
    if (-not $caught) { throw 'Invalid/failed probe was accepted as a version.' }
}
Write-Output 'version-parser-ok'
""")
        self.assertIn("version-parser-ok", output)

    def test_real_pinned_compiler_reports_version_despite_zero_pe_metadata_and_help_exit_one(self):
        compiler = self.trusted_inno()
        help_result = subprocess.run([str(compiler), "/?"], capture_output=True, timeout=30)
        self.assertEqual(1, help_result.returncode)
        repository = Path(__file__).resolve().parents[2]
        output = self.invoke("""
$script:RepositoryRoot = '%s'
$compiler = '%s'
$metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($compiler)
$peVersion = '{0}.{1}.{2}.{3}' -f $metadata.FileMajorPart, $metadata.FileMinorPart, $metadata.FileBuildPart, $metadata.FilePrivatePart
$actual = Get-InnoCompilerVersion -Compiler $compiler
if ($actual -ne '6.5.4') { throw 'Real pinned compiler did not report its vendor version.' }
$packageVersion = [version]$actual
if ($packageVersion.Major -ne 6 -or $packageVersion.Minor -lt 3) { throw 'Package compatibility check rejected the real compiler.' }
Write-Output ("metadata=$peVersion;builtin=$actual")
""" % (str(repository).replace("'", "''"), str(compiler).replace("'", "''")))
        self.assertIn("metadata=0.0.0.0;builtin=6.5.4", output)

    def test_real_compiler_parses_release_and_debug_installer_without_generating_an_exe(self):
        compiler = self.trusted_inno()
        repository = Path(__file__).resolve().parents[2]
        with temporary_directory() as directory:
            root = Path(directory)
            publish = root / "synthetic publish with spaces"
            publish.mkdir()
            (publish / "LICENSE").write_bytes((repository / "LICENSE").read_bytes())
            (publish / "FrameScope.exe").write_text("Synthetic payload; never executable or launched.")
            for configuration in ("Release", "Debug"):
                with self.subTest(configuration=configuration):
                    output = root / ("output-" + configuration)
                    result = subprocess.run([
                        str(compiler), "/O-", "/DAppVersion=0.1.0-preview",
                        "/DBuildConfiguration=" + configuration, "/DPublishDir=" + str(publish),
                        "/DOutputDir=" + str(output), str(repository / "packaging/FrameScope.iss"),
                    ], capture_output=True, cwd=root, timeout=60)
                    diagnostics = (result.stdout + result.stderr).decode("utf-8", errors="replace")
                    self.assertEqual(0, result.returncode, diagnostics)
                    self.assertIn("Output was disabled", diagnostics)
            self.assertEqual([publish / "FrameScope.exe"], list(root.rglob("*.exe")))

    def test_inno_existing_directory_is_preserved_without_download(self):
        with temporary_directory() as directory:
            root = Path(directory)
            destination = root / "inno-6.5.4"
            destination.mkdir()
            marker = destination / "keep.txt"
            marker.write_text("existing toolchain")
            output = self.invoke("""
function Invoke-WebRequest { throw 'Must not download over an existing toolchain.' }
function Invoke-Checked { throw 'Must not run installer over an existing toolchain.' }
$caught = $false
try { Install-LocalInnoSetup -ToolsRoot '%s' } catch { $caught = $_.Exception.Message -like '*already exists*' }
if (-not $caught) { throw 'Existing toolchain was not rejected.' }
Write-Output 'existing-inno-ok'
""" % str(root).replace("'", "''"))
            self.assertIn("existing-inno-ok", output)
            self.assertEqual("existing toolchain", marker.read_text())

    def test_wrong_inno_hash_never_executes_or_overwrites_cached_installer(self):
        with temporary_directory() as directory:
            root = Path(directory)
            archive = root / "innosetup-6.5.4.exe"
            archive.write_bytes(b"wrong synthetic installer")
            output = self.invoke("""
function Invoke-WebRequest { throw 'Existing mismatched cache must be inspected, not overwritten.' }
function Invoke-Checked { throw 'Unverified installer must never execute.' }
$caught = $false
try { Install-LocalInnoSetup -ToolsRoot '%s' } catch { $caught = $_.Exception.Message -like '*different SHA-256*' }
if (-not $caught) { throw 'Bad installer digest was not rejected.' }
Write-Output 'bad-inno-ok'
""" % str(root).replace("'", "''"))
            self.assertIn("bad-inno-ok", output)
            self.assertEqual(b"wrong synthetic installer", archive.read_bytes())
            self.assertFalse((root / "inno-6.5.4").exists())

    def test_bad_fresh_inno_download_is_not_executed_or_promoted_to_cache(self):
        with temporary_directory() as directory:
            root = Path(directory)
            output = self.invoke("""
function Invoke-WebRequest {
    param($Uri, $OutFile, [switch]$UseBasicParsing)
    [IO.File]::WriteAllText($OutFile, 'synthetic incorrect download')
}
function Invoke-Checked { throw 'Unverified download must never execute.' }
$caught = $false
try { Install-LocalInnoSetup -ToolsRoot '%s' } catch { $caught = $_.Exception.Message -like '*SHA-256 mismatch*' }
if (-not $caught) { throw 'Bad fresh download was not rejected.' }
Write-Output 'download-rejected'
""" % str(root).replace("'", "''"))
            self.assertIn("download-rejected", output)
            self.assertFalse((root / "innosetup-6.5.4.exe").exists())
            self.assertFalse((root / "inno-6.5.4").exists())

    def test_artifact_sha256_and_sha512_match_standard_library(self):
        with temporary_directory() as directory:
            path = Path(directory) / "artifact with spaces.bin"
            path.write_bytes(b"synthetic artifact fixture\x00\xff")
            output = self.invoke("""
$path = '%s'
if ((Get-ArtifactHash -LiteralPath $path -Algorithm SHA256) -ne '%s') { throw 'SHA256 mismatch.' }
if ((Get-ArtifactHash -LiteralPath $path -Algorithm SHA512) -ne '%s') { throw 'SHA512 mismatch.' }
Write-Output 'hashes-ok'
""" % (str(path).replace("'", "''"), hashlib.sha256(path.read_bytes()).hexdigest(),
       hashlib.sha512(path.read_bytes()).hexdigest()))
            self.assertIn("hashes-ok", output)

    def test_verified_inno_install_is_current_user_and_exports_only_selected_compiler(self):
        with temporary_directory() as directory:
            root = Path(directory)
            archive = root / "innosetup-6.5.4.exe"
            archive.write_bytes(b"synthetic installer fixture")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            environment_file = root / "github-env.txt"
            output = self.invoke("""
function Get-InnoSetupToolchain {
    return [pscustomobject]@{ Version = '6.5.4'; Url = 'https://example.invalid/not-downloaded'; Sha256 = '%s' }
}
function Invoke-WebRequest { throw 'Verified cache should be reused without downloading.' }
function Get-InnoCompilerVersion { param($Compiler) return '6.5.4' }
$script:installerCalls = 0
function Invoke-Checked {
    param($Executable, [string[]]$Arguments)
    $script:installerCalls++
    foreach ($required in @('/CURRENTUSER', '/VERYSILENT', '/NOICONS', '/TASKS=')) {
        if ($Arguments -notcontains $required) { throw 'Missing per-user/no-shortcut installer option.' }
    }
    if ($Arguments -contains '/ALLUSERS') { throw 'Machine installation is forbidden.' }
    $directoryArgument = @($Arguments | Where-Object { $_.StartsWith('/DIR=') })
    if ($directoryArgument.Count -ne 1) { throw 'Expected one explicit tools directory.' }
    $target = $directoryArgument[0].Substring(5)
    New-Item -ItemType Directory -Path $target | Out-Null
    [IO.File]::WriteAllText((Join-Path $target 'ISCC.exe'), 'synthetic compiler fixture, never executed')
}
$env:GITHUB_ENV = '%s'
$selected = Install-LocalInnoSetup -ToolsRoot '%s'
if ($script:installerCalls -ne 1 -or $env:FSC_ISCC -ne $selected) { throw 'Wrong selected compiler.' }
if ([IO.File]::ReadAllText($env:GITHUB_ENV) -ne "FSC_ISCC=$selected`n") { throw 'Wrong workflow environment export.' }
Write-Output 'local-inno-ok'
""" % (digest, str(environment_file).replace("'", "''"), str(root).replace("'", "''")))
            self.assertIn("local-inno-ok", output)

    def test_debug_and_release_layouts_are_separate(self):
        with temporary_directory() as directory:
            output = self.invoke("""
$script:RepositoryRoot = '%s'
$release = Get-FrameScopePaths -Configuration Release
$debug = Get-FrameScopePaths -Configuration Debug
if ((Get-FrameScopePaths -Configuration debug).Configuration -cne 'Debug') { throw 'Configuration casing was not normalized.' }
foreach ($property in @('NativeBuildDirectory', 'NativeOutputDirectory', 'PublishDirectory', 'VerificationPath',
                        'PackageDirectory', 'InstallerDirectory', 'TestResultsDirectory')) {
    if ($release.$property -eq $debug.$property) { throw 'Configurations share an artifact path.' }
}
if ($release.NativeOutputDirectory -ne (Join-Path $script:RepositoryRoot 'artifacts/native/bin') -or
    $debug.NativeOutputDirectory -ne (Join-Path $script:RepositoryRoot 'artifacts/native-debug/bin') -or
    $debug.VerificationPath -ne (Join-Path $script:RepositoryRoot 'artifacts/verification-debug.json')) {
    throw 'Unexpected default layout.'
}
Write-Output 'layouts-ok'
""" % str(Path(directory)).replace("'", "''"))
            self.assertIn("layouts-ok", output)

    def test_custom_native_cache_output_is_used_and_conflicting_arguments_are_rejected(self):
        with temporary_directory() as directory:
            root = Path(directory)
            build = root / "artifacts/build-debug"
            build.mkdir(parents=True)
            native = root / "artifacts/debug-native/bin"
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_BUILD_TYPE:STRING=Debug\nCMAKE_GENERATOR:INTERNAL=Ninja\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={root.as_posix()}\n"
                f"FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY:PATH={native.as_posix()}\n", encoding="utf-8")
            output = self.invoke("""
$script:RepositoryRoot = '%s'
$paths = Get-FrameScopePaths -Configuration Debug -NativeBuildDirectory 'artifacts/build-debug' -CMakeArguments @('-DUNRELATED=kept')
if ($paths.NativeOutputDirectory -ne (Join-Path $script:RepositoryRoot 'artifacts/debug-native/bin')) { throw 'Cached output was ignored.' }
Assert-NativeConfiguration $paths
$caught = 0
try { Get-FrameScopePaths -Configuration Release -NativeBuildDirectory 'artifacts/build-debug' } catch { $caught++ }
try { Get-FrameScopePaths -Configuration Debug -NativeBuildDirectory 'artifacts/build-debug' -NativeOutputDirectory 'wrong/bin' } catch { $caught++ }
try { Get-FrameScopePaths -Configuration Debug -NativeBuildDirectory 'artifacts/build-debug' -CMakeArguments @('-DCMAKE_BUILD_TYPE=Release') } catch { $caught++ }
try { Get-FrameScopePaths -Configuration Debug -CMakeArguments @('-DFRAMESCOPE_NATIVE_OUTPUT_DIRECTORY=artifacts/native/bin') } catch { $caught++ }
try { Get-FrameScopePaths -Configuration Debug -CMakeArguments @('-B', 'other/tree') } catch { $caught++ }
if ($caught -ne 5) { throw 'A configuration/output override was silently accepted.' }
$matching = '-DFRAMESCOPE_NATIVE_OUTPUT_DIRECTORY:PATH=' + $paths.NativeOutputDirectory
$same = Get-FrameScopePaths -Configuration Debug -NativeBuildDirectory 'artifacts/build-debug' -CMakeArguments @($matching)
if ($same.NativeOutputDirectory -ne $paths.NativeOutputDirectory) { throw 'Matching explicit output should be allowed.' }
Write-Output 'cache-layout-ok'
""" % str(root).replace("'", "''"))
            self.assertIn("cache-layout-ok", output)

    def test_publish_native_cache_proof_cannot_be_reused_after_options_change(self):
        with temporary_directory() as directory:
            root = Path(directory)
            build = root / "artifacts/native-debug"
            build.mkdir(parents=True)
            cache = build / "CMakeCache.txt"
            cache.write_text("CMAKE_BUILD_TYPE:STRING=Debug\nCMAKE_GENERATOR:INTERNAL=Ninja\n"
                             f"FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY:PATH={(build / 'bin').as_posix()}\n", encoding="utf-8")
            output = self.invoke("""
$script:RepositoryRoot = '%s'
$paths = Get-FrameScopePaths -Configuration Debug
$cachePath = Join-Path $paths.NativeBuildDirectory 'CMakeCache.txt'
$info = [pscustomobject]@{ configuration = 'Debug'; native_cache = [pscustomobject]@{
    configuration = 'Debug'; cache_sha256 = (Get-ArtifactHash -LiteralPath $cachePath) } }
Assert-PublishNativeCache -Paths $paths -Info $info
[IO.File]::AppendAllText($cachePath, "OPTION:BOOL=ON`n")
$caught = $false
try { Assert-PublishNativeCache -Paths $paths -Info $info } catch { $caught = $true }
if (-not $caught) { throw 'Changed native cache was accepted as already verified.' }
Write-Output 'cache-proof-ok'
""" % str(root).replace("'", "''"))
            self.assertIn("cache-proof-ok", output)

    def test_equivalent_root_alias_preserves_cached_output_and_cannot_bypass_configuration_isolation(self):
        with temporary_directory() as directory:
            root = Path(directory)
            alias = existing_directory_alias(root)
            self.assertTrue(alias.samefile(root))
            native = root / "artifacts/debug-native/bin"
            native.mkdir(parents=True)
            release_native = root / "artifacts/native/bin"
            release_native.mkdir(parents=True)
            release_alias = existing_directory_alias(release_native)
            self.assertTrue(release_alias.samefile(release_native))
            build = root / "artifacts/build-debug"
            build.mkdir(parents=True)
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_BUILD_TYPE:STRING=Debug\nCMAKE_GENERATOR:INTERNAL=Ninja\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={root.as_posix()}\n"
                f"FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY:PATH={native.as_posix()}\n", encoding="utf-8")
            output = self.invoke("""
$script:RepositoryRoot = '%s'
$paths = Get-FrameScopePaths -Configuration Debug -NativeBuildDirectory 'artifacts/build-debug'
if ($paths.NativeOutputDirectory -ne '%s') { throw 'Equivalent root alias changed the selected output.' }
Assert-NativeConfiguration $paths
$isolated = $false
try {
    Get-FrameScopePaths -Configuration Debug -NativeBuildDirectory 'artifacts/fresh-debug' -NativeOutputDirectory '%s'
}
catch { $isolated = $_.Exception.Message -like '*must not share*' }
if (-not $isolated) { throw 'Release output alias bypassed Debug/Release isolation.' }
Write-Output 'alias-isolation-ok'
""" % tuple(str(path).replace("'", "''") for path in (alias, native, release_alias)))
            self.assertIn("alias-isolation-ok", output)


if __name__ == "__main__":
    unittest.main()
