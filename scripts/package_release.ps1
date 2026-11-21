#requires -Version 7
<#
  发布包安装检查/打包脚本。
  只调用 cmake --install，不额外复制测试程序；安装后显式检查 mvp_* 测试可执行文件，
  一旦发现立即失败，避免“测试程序混进发布包”再次发生。

  用法（在项目根目录）：
    pwsh -File scripts\package_release.ps1
    pwsh -File scripts\package_release.ps1 -Config Release -CreateZip

  说明：脚本不删除旧目录，每次生成带时间戳的 stage，便于保留上一次结果。
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build\Desktop_Qt_6_10_1_MinGW_64_bit_Debug"),
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Debug",
    [string]$OutDir = (Join-Path $PSScriptRoot "..\release"),
    [switch]$CreateZip
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildPath = (Resolve-Path $BuildDir).Path
$outputPath = [System.IO.Path]::GetFullPath($OutDir)
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stage = Join-Path $outputPath "stage-$stamp"

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Write-Host "安装源: $buildPath"
Write-Host "安装到: $stage"

& cmake --install $buildPath --prefix $stage --config $Config
if ($LASTEXITCODE -ne 0) {
    throw "cmake --install 失败，退出码 $LASTEXITCODE"
}

$mainExe = Get-ChildItem -LiteralPath $stage -Recurse -File -Filter "MusicVideoPlayer.exe" |
    Select-Object -First 1
if ($null -eq $mainExe) {
    throw "安装目录里没有 MusicVideoPlayer.exe，发布包不完整"
}

# 测试目标名全部以 mvp_ 开头；主程序只有 MusicVideoPlayer.exe。
$testExe = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Filter "*.exe" |
    Where-Object { $_.BaseName -match "^mvp_" -or $_.Name -match "^(test|tests)\.exe$" })
if ($testExe.Count -gt 0) {
    $names = ($testExe | ForEach-Object { $_.FullName }) -join [Environment]::NewLine
    throw "发布包里发现测试程序：`n$names"
}

$fileCount = (Get-ChildItem -LiteralPath $stage -Recurse -File).Count
Write-Host "安装完成：主程序 $($mainExe.FullName)"
Write-Host "文件数：$fileCount；未发现 mvp_* 测试程序"

if ($CreateZip) {
    $zip = Join-Path $outputPath "MusicVideoPlayer-$stamp.zip"
    Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "发布包：$zip"
} else {
    Write-Host "仅安装检查，未生成 zip。需要 zip 时追加 -CreateZip。"
}
