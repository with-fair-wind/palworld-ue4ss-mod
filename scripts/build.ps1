#Requires -Version 5.1
<#
.SYNOPSIS
    使用 Ninja + MSVC preset 完成「配置（如需要）+ 构建」。
.DESCRIPTION
    必须在 MSVC 环境中运行（PATH 中有 cl.exe），例如
    「x64 Native Tools Command Prompt for VS 2022」或 VS Developer PowerShell。
.PARAMETER Clean
    先删除 build/ 目录再重新配置（完全重新构建）。
#>
[CmdletBinding()]
param([switch]$Clean)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new() } catch {}
$OutputEncoding = [System.Text.UTF8Encoding]::new()

Set-Location -Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

function Assert-MSVC {
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Host "错误：PATH 中找不到 'cl.exe'（MSVC）。" -ForegroundColor Red
        Write-Host "请在「x64 Native Tools Command Prompt for VS 2022」或 VS Developer PowerShell 中运行。" -ForegroundColor Red
        exit 1
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        Write-Host "错误：PATH 中找不到 'ninja'。" -ForegroundColor Red
        Write-Host "请安装 Visual Studio「使用 C++ 的桌面开发」工作负载（自带 Ninja），或单独安装 Ninja。" -ForegroundColor Red
        exit 1
    }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        Write-Host "错误：PATH 中找不到 'cmake'。" -ForegroundColor Red
        Write-Host "请安装 CMake >= 3.22（https://cmake.org/download）。" -ForegroundColor Red
        exit 1
    }
}

Assert-MSVC

if ($Clean -and (Test-Path build)) {
    Write-Host "正在删除 build/（clean）" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force build
}

Write-Host "正在配置（cmake --preset ninja-msvc-x64）..." -ForegroundColor Cyan
cmake --preset ninja-msvc-x64
if ($LASTEXITCODE -ne 0) { throw "配置失败。" }

Write-Host "正在构建（cmake --build --preset ninja-msvc-x64）..." -ForegroundColor Cyan
cmake --build --preset ninja-msvc-x64
if ($LASTEXITCODE -ne 0) { throw "构建失败。" }

Write-Host "构建完成。" -ForegroundColor Green
