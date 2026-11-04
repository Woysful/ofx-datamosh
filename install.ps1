<#
.SYNOPSIS
Install Datamosh.ofx.bundle into the system-wide OpenFX plugin directory (Windows),
from a local build or from the latest GitHub release.

.DESCRIPTION
Installs Datamosh.ofx.bundle into the OpenFX plugin directory and removes older
versions of the plugin. The plugin comes either from a local build or from the
latest release on https://github.com/Woysful/ofx-datamosh/releases.

In an interactive console you are asked which one to use. Without one the local
build is used if there is one, otherwise the latest release is downloaded.

Local builds are searched for (newest Windows build wins) in:
  .\Datamosh.ofx.bundle             (unpacked release archive)
  build-release\windows\            (build.sh)
  build\                            (plain cmake build)
  dist\ofx-datamosh-*-windows-*.zip (build.sh archives)

Downloads are checked against the SHA-256 published with the release.
Run from an elevated PowerShell (Run as Administrator).

One-line install (elevated PowerShell):
  irm https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.ps1 | iex

.PARAMETER Path
A bundle directory or a release .zip to install instead of searching.

.PARAMETER Local
Use the local build, do not ask.

.PARAMETER Download
Download the latest release, do not ask.

.PARAMETER Uninstall
Remove the plugin (and the old DatamoshTransition bundle) from the plugin
directory instead of installing. Uninstall with one line:
  & ([scriptblock]::Create((irm https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.ps1))) -Uninstall

.PARAMETER Dest
Directory to install into / uninstall from. Default: $env:OFX_INSTALL_DIR, else
C:\Program Files\Common Files\OFX\Plugins.

.EXAMPLE
.\install.ps1
.EXAMPLE
.\install.ps1 -Download
.EXAMPLE
.\install.ps1 -Path .\dist\ofx-datamosh-v1.4-windows-x64.zip
.EXAMPLE
.\install.ps1 -Uninstall
#>
param(
    [string]$Path,
    [switch]$Local,
    [switch]$Download,
    [switch]$Uninstall,
    [string]$Dest
)

function Install-Datamosh {
    $ErrorActionPreference = "Stop"
    # Global, because module cmdlets such as Expand-Archive ignore a local value; the
    # progress bar also makes Invoke-WebRequest very slow on PowerShell 5.
    $savedProgress = $global:ProgressPreference
    $global:ProgressPreference = "SilentlyContinue"
    $repo = if ($env:OFX_DATAMOSH_REPO) { $env:OFX_DATAMOSH_REPO } else { "Woysful/ofx-datamosh" }
    $apiUrl = if ($env:OFX_DATAMOSH_API) { $env:OFX_DATAMOSH_API } else { "https://api.github.com/repos/$repo/releases/latest" }
    $bundleName = "Datamosh.ofx.bundle"
    # Empty when run through "irm ... | iex": there is no local checkout then.
    $here = if ($PSCommandPath) { Split-Path -Parent $PSCommandPath } else { $null }
    $script:tmp = $null

    function Get-Binary([string]$bundle) { Join-Path $bundle "Contents\Win64\Datamosh.ofx" }
    function Test-Usable([string]$bundle) { Test-Path -LiteralPath (Get-Binary $bundle) -PathType Leaf }
    function Get-Tmp {
        if (-not $script:tmp) {
            $script:tmp = Join-Path ([IO.Path]::GetTempPath()) ("ofx-datamosh-" + [guid]::NewGuid())
            New-Item -ItemType Directory $script:tmp | Out-Null
        }
        $script:tmp
    }
    function Expand-Bundle([string]$zip) {
        $out = Join-Path (Get-Tmp) "unpacked"
        if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
        Expand-Archive -LiteralPath $zip -DestinationPath $out
        $b = Join-Path $out $bundleName
        if (-not (Test-Path -LiteralPath $b)) { throw "$zip does not contain $bundleName" }
        if (-not (Test-Usable $b)) { throw "$zip has no Win64 binary, it was not built for Windows" }
        $b
    }
    function Resolve-LocalBundle([string]$p) {
        if (-not (Test-Path -LiteralPath $p)) { throw "No such file or directory: $p" }
        if ($p -like "*.zip") { return Expand-Bundle $p }
        $b = if (Test-Path -LiteralPath (Join-Path $p "Contents")) { $p } else { Join-Path $p $bundleName }
        if (-not (Test-Usable $b)) { throw "$p has no Win64 binary, it was not built for Windows" }
        $b
    }
    function Find-Local {
        if (-not $here) { return $null }
        $bundles = @(
            (Join-Path $here $bundleName),
            (Join-Path $here "build-release\windows\$bundleName"),
            (Join-Path $here "build\$bundleName")
        ) | Where-Object { Test-Usable $_ }
        if ($bundles) {
            return $bundles | Sort-Object { (Get-Item -LiteralPath (Get-Binary $_)).LastWriteTime } -Descending |
                Select-Object -First 1
        }
        $zip = Get-ChildItem -Path (Join-Path $here "dist") -Filter "ofx-datamosh-*-windows-x64.zip" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($zip) { return $zip.FullName }
        $null
    }
    function Get-Release {
        $info = [pscustomobject]@{ State = "offline"; Tag = $null; Asset = $null; Sums = $null }
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            $rel = Invoke-RestMethod -Uri $apiUrl -Headers @{ "User-Agent" = "ofx-datamosh-installer" }
        } catch { return $info }
        $info.Tag = $rel.tag_name
        $info.State = "noasset"
        $info.Sums = $rel.assets | Where-Object { $_.name -eq "SHA256SUMS" } | Select-Object -First 1
        $info.Asset = $rel.assets | Where-Object { $_.name -like "*-windows-x64.zip" } | Select-Object -First 1
        if ($info.Asset) { $info.State = "ok" }
        $info
    }
    function Get-WebFile([string]$url, [string]$out) {
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -Headers @{ "User-Agent" = "ofx-datamosh-installer" }
    }
    function Install-FromRelease($rel) {
        if (-not $rel) { $rel = Get-Release }
        if ($rel.State -eq "offline") { throw "Could not get release information from $apiUrl" }
        if ($rel.State -eq "noasset") { throw "Release $($rel.Tag) has no Windows archive; build from source (see README)" }
        $name = $rel.Asset.name
        Write-Host "Downloading $name ($($rel.Tag))"
        $zip = Join-Path (Get-Tmp) $name
        Get-WebFile $rel.Asset.browser_download_url $zip

        $expected = $null
        if ($rel.Asset.digest -and $rel.Asset.digest -like "sha256:*") { $expected = $rel.Asset.digest.Substring(7) }
        if (-not $expected -and $rel.Sums) {
            $sumsFile = Join-Path (Get-Tmp) "SHA256SUMS"
            Get-WebFile $rel.Sums.browser_download_url $sumsFile
            foreach ($line in Get-Content -LiteralPath $sumsFile) {
                $parts = $line -split '\s+', 2
                if ($parts.Count -eq 2 -and $parts[1].TrimStart('*') -eq $name) { $expected = $parts[0]; break }
            }
        }
        if (-not $expected) {
            Write-Warning "The release publishes no SHA-256 for $name, skipping verification"
        } else {
            $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $expected.ToLowerInvariant()) {
                throw "SHA-256 mismatch for $name (expected $expected, got $actual); not installing"
            }
            Write-Host "SHA-256 OK"
        }
        Expand-Bundle $zip
    }
    function Write-NoLocal {
        Write-Host "No local Windows build was found. Build one (in Git Bash: ./build.sh windows, or ./build.sh --install),"
        Write-Host "or with CMake: cmake -S . -B build -A x64; cmake --build build --config Release"
    }

    try {
        if (-not $Dest) {
            $Dest = if ($env:OFX_INSTALL_DIR) { $env:OFX_INSTALL_DIR } else { "C:\Program Files\Common Files\OFX\Plugins" }
        }
        if ($Uninstall) {
            if ($Local -or $Download -or $Path) { throw "-Uninstall cannot be combined with -Local, -Download or -Path" }
            $targets = @($bundleName, "DatamoshTransition.ofx.bundle") |
                ForEach-Object { Join-Path $Dest $_ } | Where-Object { Test-Path -LiteralPath $_ }
            if (-not $targets) { Write-Host "Datamosh is not installed in $Dest, nothing to remove."; return 0 }
            foreach ($t in $targets) {
                Write-Host "Removing $t"
                try { Remove-Item -LiteralPath $t -Recurse -Force }
                catch [System.UnauthorizedAccessException] { throw "No write access to $Dest. Run PowerShell as Administrator and try again." }
            }
            Write-Host "Uninstalled. Restart DaVinci Resolve to unload the plugin."
            return 0
        }
        if ($Local -and $Download) { throw "-Local and -Download cannot be used together" }
        if ($Path -and $Download) { throw "-Download cannot be combined with -Path" }
        $interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected

        $bundle = $null
        if ($Path) {
            $bundle = Resolve-LocalBundle $Path
        } elseif ($Local) {
            $found = Find-Local
            if (-not $found) { Write-NoLocal; return 1 }
            $bundle = Resolve-LocalBundle $found
        } elseif ($Download) {
            $bundle = Install-FromRelease $null
        } else {
            $found = Find-Local
            if (-not $interactive) {
                $bundle = if ($found) { Resolve-LocalBundle $found } else { Install-FromRelease $null }
            } else {
                $rel = Get-Release
                Write-Host "Datamosh OFX installer (Windows, Win64)"
                $choices = @()
                if ($found) {
                    $shown = if ($here -and $found.StartsWith($here)) { $found.Substring($here.Length).TrimStart('\', '/') } else { $found }
                    $date = (Get-Item -LiteralPath $found).LastWriteTime.ToString("yyyy-MM-dd HH:mm")
                    Write-Host "  1) local build:    $shown ($date)"
                    $choices += "1"
                } else {
                    Write-Host "     (no local build for Windows)"
                }
                switch ($rel.State) {
                    "ok"      { Write-Host "  2) latest release: $($rel.Tag) from github.com/$repo"; $choices += "2" }
                    "noasset" { Write-Host "     (latest release $($rel.Tag) has no Windows archive)" }
                    "offline" { Write-Host "     (could not reach GitHub for the latest release)" }
                }
                if ($choices.Count -eq 0) { Write-NoLocal; return 1 }
                if ($choices.Count -eq 1) {
                    $c = $choices[0]
                    $q = if ($c -eq "1") { "Install the local build? [Y/n]" } else { "Download and install $($rel.Tag)? [Y/n]" }
                    $a = Read-Host $q
                    if ($a -and $a -notmatch '^(y|yes)$') { Write-Host "Cancelled."; return 1 }
                } else {
                    while ($true) {
                        $a = Read-Host "Choose 1 or 2, q to quit [1]"
                        if (-not $a -or $a -eq "1") { $c = "1"; break }
                        if ($a -eq "2") { $c = "2"; break }
                        if ($a -match '^q$') { Write-Host "Cancelled."; return 1 }
                    }
                }
                $bundle = if ($c -eq "1") { Resolve-LocalBundle $found } else { Install-FromRelease $rel }
            }
        }

        Write-Host "Installing $bundle"
        try {
            New-Item -ItemType Directory -Force $Dest | Out-Null
            foreach ($old in @($bundleName, "DatamoshTransition.ofx.bundle")) {
                $t = Join-Path $Dest $old
                if (Test-Path -LiteralPath $t) { Remove-Item -LiteralPath $t -Recurse -Force }
            }
            Copy-Item -LiteralPath $bundle -Destination $Dest -Recurse -Force
        } catch [System.UnauthorizedAccessException] {
            throw "No write access to $Dest. Run PowerShell as Administrator and try again."
        }
        Write-Host "Installed to $(Join-Path $Dest $bundleName)"
        Write-Host "Restart DaVinci Resolve; find it under Effects Library > OpenFX > Filters > Datamosh."
        return 0
    } catch {
        Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
        return 1
    } finally {
        $global:ProgressPreference = $savedProgress
        if ($script:tmp -and (Test-Path -LiteralPath $script:tmp)) { Remove-Item -LiteralPath $script:tmp -Recurse -Force }
    }
}

$code = Install-Datamosh
# Under "irm | iex" an exit would close the user's PowerShell window.
if ($PSCommandPath) { exit $code }
