#Requires -Version 7.0
# 仅检测环境并提供安装指引，不下载或安装软件。
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw '本脚本只支持 Windows。' }
foreach ($Tool in @('cmake', 'ctest')) {
    if (-not (Get-Command $Tool -ErrorAction SilentlyContinue)) {
        throw "缺少 $Tool；请按 scripts/README.md 手动准备环境。"
    }
}
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path $VsWhere)) { throw '请安装 Visual Studio C++ 桌面开发工具、MSVC v143 及 Windows SDK。' }
$VsPath = & $VsWhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0) { throw '查询 Visual Studio 2022 失败。' }
$env:WAIBUSNAP_VS_GENERATOR = 'Visual Studio 17 2022'
if (-not $VsPath) {
    # 新 runner 使用 VS 2026 宿主，但仍显式选择其安装的 MSVC 2022 v143 工具集。
    $VsPath = & $VsWhere -latest -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $VsPath) { throw '需要 VS 2022 C++，或 VS 2026 加装 MSVC v143 14.44 x64 工具集。' }
    $env:WAIBUSNAP_VS_GENERATOR = 'Visual Studio 18 2026'
    $CmakeVersion = & cmake --version
    if ($LASTEXITCODE -ne 0) { throw '读取 CMake 版本失败。' }
    if ($CmakeVersion[0] -notmatch 'cmake version (\d+\.\d+\.\d+)' -or [version]$Matches[1] -lt [version]'4.2.0') {
        throw 'VS 2026 生成器需要 CMake 4.2 或更高版本。'
    }
}
$env:WAIBUSNAP_VS_INSTANCE = $VsPath.Trim()
Write-Host "构建宿主：$env:WAIBUSNAP_VS_GENERATOR；工具集：v143"
if (-not $env:QT_ROOT_DIR) {
    $QmakeCommand = Get-Command qmake -ErrorAction SilentlyContinue
    if (-not $QmakeCommand) { throw '请安装 Qt 6.11.2 MSVC 2022 x64 组件并设置 QT_ROOT_DIR。' }
    $env:QT_ROOT_DIR = (& $QmakeCommand.Source -query QT_INSTALL_PREFIX).Trim()
    if ($LASTEXITCODE -ne 0) { throw '读取 Qt 路径失败。' }
}
$Qmake = Join-Path $env:QT_ROOT_DIR 'bin/qmake.exe'
if (-not (Test-Path $Qmake)) { throw 'QT_ROOT_DIR 必须指向包含 bin/qmake.exe 的 Qt 目录。' }
$QtVersion = & $Qmake -query QT_VERSION
if ($LASTEXITCODE -ne 0 -or $QtVersion.Trim() -ne '6.11.2') { throw "需要 Qt 6.11.2，实际为 $QtVersion。" }
$QtSpec = & $Qmake -query QMAKE_XSPEC
if ($LASTEXITCODE -ne 0 -or $QtSpec.Trim() -ne 'win32-msvc') { throw '必须使用 MSVC 版 Qt，不能使用 MinGW 版。' }
Write-Host "环境检测通过：Qt $QtVersion，路径 $env:QT_ROOT_DIR"
