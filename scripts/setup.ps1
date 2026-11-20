#Requires -Version 7.0
[CmdletBinding()]
param([switch]$Update, [string]$Ref = 'main')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'dependencies.ps1')
$directory = Join-Path $PSScriptRoot '../RE-UE4SS'
if ([string]::IsNullOrWhiteSpace($Ref) -or $Ref.StartsWith('-')) { throw 'Specify a branch, tag, or commit.' }
$created = -not (Test-Path -LiteralPath $directory)
if (-not (Test-Path -LiteralPath $directory)) {
    Invoke-DependencyGit @('clone', '--no-checkout', '--', 'https://github.com/UE4SS-RE/RE-UE4SS.git', $directory) | Out-Host
} else {
    Assert-DependencyClean $directory
}
if ($created -or $Update) {
    Invoke-DependencyGit @('-C', $directory, 'fetch', 'origin', $Ref) | Out-Host
    $commit = Invoke-DependencyGit @('-C', $directory, 'rev-list', '-n', '1', 'FETCH_HEAD')
} else {
    if ($PSBoundParameters.ContainsKey('Ref')) { throw '-Ref requires -Update for an existing checkout.' }
    $commit = Invoke-DependencyGit @('-C', $directory, 'rev-parse', 'HEAD')
}
Sync-DependencyCommit $directory $commit
Write-Host "UE4SS ready: $commit. After an update, use a new build directory."
