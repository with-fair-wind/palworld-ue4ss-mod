#Requires -Version 5.1
<#
.SYNOPSIS
    构建并把 MyPalMod 部署到 Palworld 游戏目录（执行 'deploy' target）。
.DESCRIPTION
    需要在配置时已设置 PALWORLD_INSTALL_DIR
    （通过 -DPALWORLD_INSTALL_DIR=<路径> 或 PALWORLD_INSTALL_DIR 环境变量）。
#>

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new() } catch {}
$OutputEncoding = [System.Text.UTF8Encoding]::new()

Set-Location -Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Host "错误：PATH 中找不到 'cl.exe'（MSVC）。" -ForegroundColor Red
    Write-Host "请在「x64 Native Tools Command Prompt for VS 2022」或 VS Developer PowerShell 中运行。" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path build)) {
    Write-Host "未找到 build/ —— 先进行配置（cmake --preset ninja-msvc-x64）..." -ForegroundColor Cyan
    cmake --preset ninja-msvc-x64
    if ($LASTEXITCODE -ne 0) { throw "配置失败。" }
}

Write-Host "正在部署（cmake --build --preset ninja-msvc-x64 --target deploy）..." -ForegroundColor Cyan
cmake --build --preset ninja-msvc-x64 --target deploy
if ($LASTEXITCODE -ne 0) { throw "部署失败。" }

Write-Host "部署完成。" -ForegroundColor Green
