#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
& (Join-Path $PSScriptRoot 'prepare-environment.ps1')
$RepoRoot = Split-Path $PSScriptRoot -Parent
$BuildDir = if ($env:WAIBUSNAP_BUILD_DIR) { $env:WAIBUSNAP_BUILD_DIR } else { Join-Path $RepoRoot 'build/windows-x64' }
$BuildType = if ($env:WAIBUSNAP_BUILD_TYPE) { $env:WAIBUSNAP_BUILD_TYPE } else { 'Release' }
$Application = Join-Path $BuildDir "bin/$BuildType/WaibuSnap.exe"
if (-not (Test-Path $Application)) { throw '请先执行 build-project.ps1。' }
$env:PATH = (Join-Path $env:QT_ROOT_DIR 'bin') + [IO.Path]::PathSeparator + $env:PATH
& $Application @args
if ($LASTEXITCODE -ne 0) { throw "程序异常退出：$LASTEXITCODE" }
