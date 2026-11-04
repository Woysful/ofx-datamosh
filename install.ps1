# Install the built bundle into the system-wide OpenFX plugin directory (Windows).
# Run from an elevated PowerShell: .\install.ps1
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bundle = Join-Path $here "build\Datamosh.ofx.bundle"
if (-not (Test-Path $bundle)) { $bundle = Join-Path $here "Datamosh.ofx.bundle" }   # release archive layout
if (-not (Test-Path $bundle)) {
    Write-Error "Bundle not found. Build first: cmake -S . -B build -A x64; cmake --build build --config Release"
}
$dest = if ($env:OFX_INSTALL_DIR) { $env:OFX_INSTALL_DIR } else { "C:\Program Files\Common Files\OFX\Plugins" }
New-Item -ItemType Directory -Force $dest | Out-Null
$target = Join-Path $dest "Datamosh.ofx.bundle"
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
Copy-Item -Recurse -Force $bundle $dest
Write-Host "Installed to $target"
Write-Host "Restart DaVinci Resolve; find it under Effects Library > OpenFX > Filters > Datamosh."
