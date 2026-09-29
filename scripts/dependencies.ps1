#Requires -Version 7.0
# Shared dependency operations. Never discard local work or advance submodules independently.
function Invoke-DependencyGit {
    param([string[]]$Arguments)
    $git = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $gitRoot = Split-Path (Split-Path $git -Parent) -Parent
    $oldPath = $env:PATH
    $oldLocale = $env:LC_ALL
    try {
        # Git for Windows submodule scripts require its bundled Unix utilities.
        $unixTools = Join-Path $gitRoot 'usr/bin'
        if (Test-Path -LiteralPath $unixTools) { $env:PATH = "$unixTools;$gitRoot/mingw64/bin;$oldPath" }
        $env:LC_ALL = 'C'
        $bash = Join-Path $gitRoot 'bin/bash.exe'
        if (Test-Path -LiteralPath $bash) {
            $output = & $bash --noprofile --norc -c 'export PATH="/mingw64/bin:/usr/bin:$PATH"; exec git "$@"' dependency-git @Arguments
        } else {
            $output = & $git @Arguments
        }
    } finally {
        $env:PATH = $oldPath
        $env:LC_ALL = $oldLocale
    }
    if ($LASTEXITCODE -ne 0) { throw "git failed: $($Arguments -join ' ')" }
    return $output
}
function Assert-DependencyClean {
    param([string]$Directory)
    $status = Invoke-DependencyGit @('-C', $Directory, 'status', '--porcelain', '--untracked-files=all', '--ignore-submodules=none')
    if ($status) { throw 'RE-UE4SS has local changes or changed submodules; preserve them before synchronizing.' }
    Invoke-DependencyGit @('-C', $Directory, 'submodule', 'foreach', '--quiet', '--recursive',
        'test -z "$(git status --porcelain --untracked-files=all)"') | Out-Null
}
function Sync-DependencyCommit {
    param([string]$Directory, [string]$Commit)
    Invoke-DependencyGit @('-C', $Directory, 'checkout', '--detach', $Commit) | Out-Host
    Invoke-DependencyGit @('-C', $Directory, 'submodule', 'sync', '--recursive') | Out-Host
    Invoke-DependencyGit @('-C', $Directory, 'submodule', 'update', '--init', '--recursive') | Out-Host
    $status = Invoke-DependencyGit @('-C', $Directory, 'submodule', 'status', '--recursive')
    if ($status | Where-Object { $_ -match '^[+U-]' }) { throw 'Submodules do not match the selected commit.' }
    Assert-DependencyClean $Directory
}
function Get-DependencyCommit {
    param([string]$Root)
    $directory = Join-Path $Root 'RE-UE4SS'
    $actual = Invoke-DependencyGit @('-C', $directory, 'rev-parse', 'HEAD')
    Assert-DependencyClean $directory
    $status = Invoke-DependencyGit @('-C', $directory, 'submodule', 'status', '--recursive')
    if ($status | Where-Object { $_ -match '^[+U-]' }) { throw 'Initialize matching submodules with setup.ps1.' }
    return $actual.Trim()
}
