#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not (Get-Command clang-format -ErrorAction SilentlyContinue)) { throw '请按 scripts/README.md 准备 clang-format 18.1.8。' }
$FormatVersion = & clang-format --version
if ($LASTEXITCODE -ne 0 -or $FormatVersion -notmatch 'version 18\.1\.8(?:\s|$)') { throw '格式工具必须为 clang-format 18.1.8。' }
$RepoRoot = Split-Path $PSScriptRoot -Parent
$SourceRoots = @((Join-Path $RepoRoot 'src'), (Join-Path $RepoRoot 'tests'))
Get-ChildItem -Path $SourceRoots -Recurse -File | Where-Object { $_.Extension -in @('.h', '.cpp', '.mm') } | ForEach-Object {
    & clang-format --dry-run --Werror $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "格式检查失败：$($_.FullName)" }
}
