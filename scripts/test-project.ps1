#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
& (Join-Path $PSScriptRoot 'prepare-environment.ps1')
$RepoRoot = Split-Path $PSScriptRoot -Parent
$BuildDir = if ($env:WAIBUSNAP_BUILD_DIR) { $env:WAIBUSNAP_BUILD_DIR } else { Join-Path $RepoRoot 'build/windows-x64' }
$BuildType = if ($env:WAIBUSNAP_BUILD_TYPE) { $env:WAIBUSNAP_BUILD_TYPE } else { 'Release' }
if (-not (Test-Path (Join-Path $BuildDir 'CTestTestfile.cmake'))) { throw '请先执行 build-project.ps1。' }
# 测试进程及其子进程从同一 Qt 安装加载 DLL。
$env:PATH = (Join-Path $env:QT_ROOT_DIR 'bin') + [IO.Path]::PathSeparator + $env:PATH
& ctest --test-dir $BuildDir -C $BuildType --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { throw 'CTest 失败。' }
