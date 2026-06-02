param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [switch]$CleanFirst,
    [string]$InstallXPlanePluginDir = "J:\SteamLibrary\steamapps\common\X-Plane 12\Resources\plugins\Plugin-XTE-DCDU\64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$pluginDir = Split-Path -Parent $scriptDir

$builtXpl = Join-Path $pluginDir "$BuildDir\$Config\win.xpl"
$installedXpl = Join-Path $InstallXPlanePluginDir "win.xpl"

Write-Host "[deploy] plugin dir: $pluginDir"
Write-Host "[deploy] build dir : $BuildDir"
Write-Host "[deploy] config    : $Config"
Write-Host "[deploy] target    : $installedXpl"

Push-Location $pluginDir
try {
    $buildArgs = @("--build", $BuildDir, "--config", $Config)
    if ($CleanFirst) {
        $buildArgs += "--clean-first"
    }

    Write-Host "[deploy] building..."
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-Path $builtXpl)) {
        throw "built plugin not found: $builtXpl"
    }

    $targetDir = Split-Path -Parent $installedXpl
    if (-not (Test-Path $targetDir)) {
        throw "target directory does not exist: $targetDir"
    }

    Write-Host "[deploy] copying win.xpl..."
    Copy-Item $builtXpl $installedXpl -Force

    if (-not (Test-Path $installedXpl)) {
        throw "installed plugin not found after copy: $installedXpl"
    }

    $builtHash = (Get-FileHash $builtXpl -Algorithm SHA256).Hash
    $instHash = (Get-FileHash $installedXpl -Algorithm SHA256).Hash

    if ($builtHash -ne $instHash) {
        throw "hash mismatch after deploy`n built: $builtHash`n inst : $instHash"
    }

    $builtInfo = Get-Item $builtXpl
    $instInfo = Get-Item $installedXpl

    Write-Host "[deploy] success"
    Write-Host "[deploy] built    : $($builtInfo.FullName)"
    Write-Host "[deploy] installed: $($instInfo.FullName)"
    Write-Host "[deploy] size     : $($instInfo.Length) bytes"
    Write-Host "[deploy] built ts : $($builtInfo.LastWriteTime)"
    Write-Host "[deploy] inst ts  : $($instInfo.LastWriteTime)"
    Write-Host "[deploy] sha256   : $builtHash"
}
finally {
    Pop-Location
}
