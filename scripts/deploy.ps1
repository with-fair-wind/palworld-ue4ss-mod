#Requires -Version 7.0
# Update an existing installation; no assets, settings, loader, or other mods are copied.
[CmdletBinding()]
param(
    [string]$BuildDirectory = "$PSScriptRoot/../build/msvc",
    [string]$InstallDirectory = $env:PALWORLD_INSTALL_DIR,
    [string]$RuntimeDirectory
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name 'Palworld*', 'PalServer*' -ErrorAction SilentlyContinue) {
    throw 'Exit Palworld and PalServer completely before deployment.'
}
if (-not $RuntimeDirectory) {
    if (-not $InstallDirectory) { throw 'Pass -InstallDirectory or the actual -RuntimeDirectory.' }
    $win64 = Join-Path $InstallDirectory 'Pal/Binaries/Win64'
    if (Test-Path -LiteralPath (Join-Path $win64 'override.txt')) {
        throw 'override.txt redirects UE4SS. Pass the actual -RuntimeDirectory explicitly.'
    }
    $RuntimeDirectory = Join-Path $win64 'ue4ss'
}
$RuntimeDirectory = [IO.Path]::GetFullPath($RuntimeDirectory)
if (-not (Test-Path -LiteralPath (Join-Path $RuntimeDirectory 'UE4SS.dll') -PathType Leaf)) {
    throw 'An existing UE4SS installation is required. Install the compatible runtime and game adaptation first.'
}
$info = Get-Content -LiteralPath (Join-Path $BuildDirectory 'deployment-build.json') -Raw | ConvertFrom-Json
$sources = @($info.runtimeDll, $info.modDll)
$destinations = @((Join-Path $RuntimeDirectory 'UE4SS.dll'),
    (Join-Path $RuntimeDirectory 'Mods/PalworldEditor/dlls/main.dll'))
# Open both inputs and destinations before changing either DLL, so missing/locked files fail early.
$inputs = @()
$outputs = @()
try {
    foreach ($source in $sources) {
        if (-not $source -or -not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Build both DLLs first: $source" }
        if ($destinations -contains [IO.Path]::GetFullPath($source)) { throw 'Build outputs must be separate from the installed DLLs.' }
        $inputs += [IO.File]::Open($source, 'Open', 'Read', 'Read')
        if ($inputs[-1].Length -eq 0) { throw "Empty build output: $source" }
    }
    foreach ($destination in $destinations) {
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        $outputs += [IO.File]::Open($destination, 'OpenOrCreate', 'Write', 'None')
    }
    for ($i = 0; $i -lt 2; $i++) {
        $outputs[$i].SetLength(0)
        $inputs[$i].CopyTo($outputs[$i])
        $outputs[$i].Flush($true)
    }
} catch {
    throw "Deployment failed. Keep the game closed; fix the error and redeploy both DLLs before launching. Cause: $_"
} finally {
    foreach ($stream in $outputs + $inputs) { $stream.Dispose() }
}
Write-Host "Updated UE4SS.dll and Mods/PalworldEditor/dlls/main.dll in $RuntimeDirectory"
Write-Host 'For a first mod install, enable PalworldEditor in your existing mod configuration. Cold-start the game and verify the UE4SS log and GUI.'
