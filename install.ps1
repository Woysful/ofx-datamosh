<#
.SYNOPSIS
Install Datamosh.ofx.bundle into the system-wide OpenFX plugin directory (Windows).

.DESCRIPTION
Copies Datamosh.ofx.bundle into the OpenFX plugin directory and removes older
versions of the plugin. Without -Path it uses the newest Windows build found in:
  .\Datamosh.ofx.bundle            (unpacked release archive)
  build-release\windows\           (build.sh)
  build\                           (plain cmake build)
  dist\ofx-datamosh-*-windows-*.zip (build.sh archives, unpacked on the fly)
Run from an elevated PowerShell (Run as Administrator).

.PARAMETER Path
A bundle directory or a release .zip to install instead of searching.

.PARAMETER Dest
Target directory. Default: $env:OFX_INSTALL_DIR, else
C:\Program Files\Common Files\OFX\Plugins.

.EXAMPLE
.\install.ps1
.EXAMPLE
.\install.ps1 -Path .\dist\ofx-datamosh-v1.4-windows-x64.zip
#>
param(
    [string]$Path,
    [string]$Dest
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bundleName = "Datamosh.ofx.bundle"
$tmp = $null

function Get-Binary([string]$bundle) { Join-Path $bundle "Contents\Win64\Datamosh.ofx" }
function Test-Usable([string]$bundle) { Test-Path -LiteralPath (Get-Binary $bundle) -PathType Leaf }
function Expand-Bundle([string]$zip) {
    $script:tmp = Join-Path ([IO.Path]::GetTempPath()) ("ofx-datamosh-" + [guid]::NewGuid())
    Expand-Archive -LiteralPath $zip -DestinationPath $script:tmp
    $b = Join-Path $script:tmp $bundleName
    if (-not (Test-Path -LiteralPath $b)) { throw "$zip does not contain $bundleName" }
    return $b
}

try {
    $bundle = $null
    if ($Path) {
        if (-not (Test-Path -LiteralPath $Path)) { throw "No such file or directory: $Path" }
        if ($Path -like "*.zip") {
            $bundle = Expand-Bundle $Path
        } elseif (Test-Path -LiteralPath (Join-Path $Path "Contents")) {
            $bundle = $Path
        } else {
            $bundle = Join-Path $Path $bundleName
        }
        if (-not (Test-Usable $bundle)) { throw "$Path has no Win64 binary, it was not built for Windows" }
    } else {
        $candidates = @(
            (Join-Path $here $bundleName),
            (Join-Path $here "build-release\windows\$bundleName"),
            (Join-Path $here "build\$bundleName")
        ) | Where-Object { Test-Usable $_ }
        if ($candidates) {
            $bundle = $candidates |
                Sort-Object { (Get-Item -LiteralPath (Get-Binary $_)).LastWriteTime } -Descending |
                Select-Object -First 1
        } else {
            $zip = Get-ChildItem -Path (Join-Path $here "dist") -Filter "ofx-datamosh-*-windows-*.zip" -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($zip) {
                $bundle = Expand-Bundle $zip.FullName
                if (-not (Test-Usable $bundle)) { throw "$($zip.FullName) has no Win64 binary" }
            }
        }
    }
    if (-not $bundle) {
        throw "No $bundleName built for Windows was found. Build it first (in Git Bash: ./build.sh windows, or ./build.sh --install), or: cmake -S . -B build -A x64; cmake --build build --config Release"
    }

    if (-not $Dest) {
        $Dest = if ($env:OFX_INSTALL_DIR) { $env:OFX_INSTALL_DIR } else { "C:\Program Files\Common Files\OFX\Plugins" }
    }
    Write-Host "Installing $bundle"
    New-Item -ItemType Directory -Force $Dest | Out-Null
    foreach ($old in @($bundleName, "DatamoshTransition.ofx.bundle")) {
        $t = Join-Path $Dest $old
        if (Test-Path -LiteralPath $t) { Remove-Item -LiteralPath $t -Recurse -Force }
    }
    Copy-Item -LiteralPath $bundle -Destination $Dest -Recurse -Force
    Write-Host "Installed to $(Join-Path $Dest $bundleName)"
    Write-Host "Restart DaVinci Resolve; find it under Effects Library > OpenFX > Filters > Datamosh."
} catch {
    Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    if ($tmp -and (Test-Path -LiteralPath $tmp)) { Remove-Item -LiteralPath $tmp -Recurse -Force }
}
