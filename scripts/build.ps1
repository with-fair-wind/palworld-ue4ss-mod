#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string[]]$Target = @('UE4SS', 'PalworldEditor'),
    [string[]]$CMakeArguments = @(),
    [switch]$ConfigureOnly,
    [switch]$PreflightOnly,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
function Initialize-BuildToolchain {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is missing.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Install Visual Studio Desktop development with C++.' }
    & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    foreach ($name in @('cmake', 'ninja', 'ml64', 'link', 'rc', 'mt', 'cargo', 'rustc')) {
        if (-not (Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)) { throw "Missing required tool: $name" }
    }
    $compiler = 'cl'
    if (-not (Get-Command $compiler -CommandType Application -ErrorAction SilentlyContinue)) { throw "Missing compiler: $compiler" }
    $cmakeVersion = & cmake --version
    if ($LASTEXITCODE -ne 0 -or $cmakeVersion[0] -notmatch 'cmake version (\d+\.\d+\.\d+)' -or [version]$Matches[1] -lt [version]'3.26.0') {
        throw 'CMake 3.26 or newer is required.'
    }
    if (-not $env:WindowsSdkDir -or -not $env:WindowsSDKVersion -or
        -not (Test-Path (Join-Path $env:WindowsSdkDir "Include/$($env:WindowsSDKVersion.TrimEnd('\'))/um/Windows.h"))) {
        throw 'Windows SDK headers are unavailable.'
    }
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Visual Studio did not activate the x64 target environment.' }
    Write-Host 'Toolchain ready: MSVC x64'
}

function Assert-BuildCache {
    param([string]$BuildDirectory)
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath)) { return }
    $cache = Get-Content -LiteralPath $cachePath -Raw
    $compiler = 'cl.exe'
    foreach ($language in @('C', 'CXX')) {
        $pattern = '(?m)^CMAKE_' + $language + '_COMPILER:[^=]+=(.+)\r?$'
        if ($cache -notmatch $pattern -or
            [IO.Path]::GetFileName($Matches[1].Trim()) -ine $compiler) {
            throw "Existing cache uses a different or unknown $language compiler. Choose a new BuildDirectory."
        }
    }
}

function Invoke-BuildCommand {
    param([Parameter(Mandatory)][string]$Command, [string[]]$Arguments = @())
    $git = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $gitRoot = Split-Path (Split-Path $git -Parent) -Parent
    $bash = Join-Path $gitRoot 'bin/bash.exe'
    if (-not (Test-Path -LiteralPath $bash)) { throw 'Git for Windows Bash is required for CMake dependency operations.' }
    $link = (Get-Command link.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    if ($link -notmatch '[\\/]VC[\\/]Tools[\\/]MSVC[\\/]') {
        throw 'Activate the Visual Studio x64 toolchain before invoking build commands.'
    }
    # Preserve every VS/SDK tool directory and its ordering when crossing into MSYS.
    & $bash --noprofile --norc -c 'build_path=$(/usr/bin/cygpath -p -u "$1") || exit; shift; export PATH="$build_path:/mingw64/bin:/usr/bin"; export LC_ALL=C; exec "$@"' build-command $env:PATH $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE." }
}
. (Join-Path $PSScriptRoot 'dependencies.ps1')
$root = Split-Path $PSScriptRoot -Parent
Initialize-BuildToolchain
$commit = Get-DependencyCommit -Root $root
Write-Host "Building against UE4SS $commit"
if ($PreflightOnly) { return }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build/msvc' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
Assert-BuildCache -BuildDirectory $BuildDirectory
Push-Location $root
try {
    Invoke-BuildCommand -Command 'cmake' -Arguments (@('--preset', 'ninja-msvc-x64', '-B', $BuildDirectory) + $CMakeArguments)
    if ($ConfigureOnly) { return }
    if ($Test) {
        $Target = @($Target) + @('format-check', 'PalworldEditorTests', 'PalworldEditorCommonTests',
            'PalworldEditorModLifecycleTests', 'PalworldEditorBaseResourceSharingTests',
            'PalworldEditorRemotePalboxTests', 'PalworldEditorCaptureOverrideTests',
            'PalworldEditorReviveTimerTests', 'PalworldEditorFishingBoostTests', 'PalworldEditorWaypointTeleportTests')
    }
    Invoke-BuildCommand -Command 'cmake' -Arguments (@('--build', $BuildDirectory, '--target') + $Target)
    if ($Test) {
        Invoke-BuildCommand -Command 'ctest' -Arguments @('--test-dir', $BuildDirectory, '--output-on-failure')
    }
} finally { Pop-Location }
