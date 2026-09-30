#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
& (Join-Path $PSScriptRoot 'prepare-environment.ps1')
$RepoRoot = Split-Path $PSScriptRoot -Parent
$BuildDir = if ($env:WAIBUSNAP_BUILD_DIR) { $env:WAIBUSNAP_BUILD_DIR } else { Join-Path $RepoRoot 'build/windows-x64' }
$BuildType = if ($env:WAIBUSNAP_BUILD_TYPE) { $env:WAIBUSNAP_BUILD_TYPE } else { 'Release' }
if ($BuildType -notin @('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')) { throw '不支持的构建配置。' }
& cmake -S $RepoRoot -B $BuildDir -G $env:WAIBUSNAP_VS_GENERATOR -A x64 -T v143 "-DCMAKE_GENERATOR_INSTANCE=$env:WAIBUSNAP_VS_INSTANCE" "-DCMAKE_PREFIX_PATH=$env:QT_ROOT_DIR" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }
& cmake --build $BuildDir --config $BuildType --parallel
if ($LASTEXITCODE -ne 0) { throw '编译失败。' }
