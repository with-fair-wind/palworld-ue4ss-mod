#Requires -Version 5.1
<#
.SYNOPSIS
    首次初始化：克隆 RE-UE4SS 并初始化其子模块。
.DESCRIPTION
    可重复执行。在仓库根目录运行：  pwsh scripts/setup.ps1
.PARAMETER Branch
    要克隆的 RE-UE4SS 分支/tag，默认 'main'。
#>
[CmdletBinding()]
param(
    [string]$RepoUrl = "https://github.com/UE4SS-RE/RE-UE4SS.git",
    [string]$Branch  = "main"
)

$ErrorActionPreference = "Stop"
# 让中文输出在控制台正确显示。
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new() } catch {}
$OutputEncoding = [System.Text.UTF8Encoding]::new()

# 始终基于本脚本所在位置定位仓库根目录（上两级）。
Set-Location -Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$root = Get-Location

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "PATH 中找不到 git。请先安装 Git（https://git-scm.com）后重试。"
}

$depsDir = Join-Path $root "RE-UE4SS"
if (Test-Path (Join-Path $depsDir ".git")) {
    Write-Host "RE-UE4SS 已克隆到 $depsDir —— 跳过克隆。" -ForegroundColor DarkGray
}
else {
    Write-Host "正在克隆 RE-UE4SS（$Branch）-> $depsDir" -ForegroundColor Cyan
    git clone --branch $Branch -- $RepoUrl $depsDir
    if ($LASTEXITCODE -ne 0) { throw "git clone 失败。" }
}

Write-Host "正在初始化 RE-UE4SS 子模块（递归）..." -ForegroundColor Cyan
git -C $depsDir submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw "git submodule update 失败。" }

Write-Host ""
Write-Host "初始化完成。" -ForegroundColor Green
Write-Host "后续步骤：" -ForegroundColor Green
Write-Host "  1. 打开「x64 Native Tools Command Prompt for VS 2022」（或 VS Developer PowerShell），" -ForegroundColor Green
Write-Host "     以确保 cl.exe 和 ninja 在 PATH 中。" -ForegroundColor Green
Write-Host "  2.（可选）设置游戏路径：  `$env:PALWORLD_INSTALL_DIR = 'C:\Path\To\Palworld'" -ForegroundColor Green
Write-Host "  3. 配置：  cmake --preset ninja-msvc-x64" -ForegroundColor Green
Write-Host "  4. 构建：  cmake --build --preset ninja-msvc-x64" -ForegroundColor Green
Write-Host "  5. 部署：  cmake --build --preset ninja-msvc-x64 --target deploy" -ForegroundColor Green
