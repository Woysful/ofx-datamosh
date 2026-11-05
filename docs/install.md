# Install and uninstall

[Back to README](../README.md)

## Install

**One command** (downloads the latest release, checks its SHA-256, installs it):

```sh
# Linux / macOS (asks for your password through sudo)
curl -fsSL https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.sh | sh
```

```powershell
# Windows, in PowerShell started as Administrator
irm https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.ps1 | iex
```

Then restart the host (DaVinci Resolve, Natron, ...). The latest release currently has archives for
Linux x86_64 and Windows x64; on macOS build from source (see below).

**Installer scripts.** `install.sh` (Linux/macOS) and `install.ps1` (Windows)
also ship in the repository and in every release archive. In a terminal they
ask where to take the plugin from:

```
Datamosh OFX installer (linux, Linux-x86-64)
  1) local build:    build-release/linux/Datamosh.ofx.bundle (2026-11-04 22:03)
  2) latest release: v1.6.0 from github.com/Woysful/ofx-datamosh
Choose 1 or 2, q to quit [1]:
```

* The local build is the newest one for the current OS from the unpacked
  archive next to the script, `build-release/`, `build/` or the archives in
  `dist/`. Bundles built for another OS are never picked.
* The release download is checked against the SHA-256 GitHub publishes for
  the file (or a `SHA256SUMS` file in the release) and refused on mismatch.
* Without a terminal (called from another script) they use the local build,
  or download the release when there is none.
* `--local` / `--download` (`-Local` / `-Download`) skip the question, a path
  to a bundle or zip installs exactly that, `--dest DIR` (`-Dest DIR`)
  changes the target directory, `--uninstall` (`-Uninstall`) removes the
  plugin (see [Uninstall](#uninstall)). Help: `./install.sh --help`,
  `Get-Help .\install.ps1 -Detailed`.
* `install.sh` uses sudo only for the copy step when the target directory
  needs it. `install.ps1` must run as Administrator.

**Manual install.** Unpack the archive for your OS from the
[latest release](https://github.com/Woysful/ofx-datamosh/releases/latest) and copy the whole
`Datamosh.ofx.bundle` folder into the system OpenFX plugin directory, then
restart the host.

| OS | Plugin directory |
|---|---|
| Linux | `/usr/OFX/Plugins/` |
| Windows | `C:\Program Files\Common Files\OFX\Plugins\` |
| macOS | `/Library/OFX/Plugins/` |

Create the directory if it does not exist. Copying needs administrator
rights on all three systems.

**Linux**

```sh
sudo mkdir -p /usr/OFX/Plugins
sudo cp -r Datamosh.ofx.bundle /usr/OFX/Plugins/
```

**Windows** (PowerShell as Administrator)

```powershell
New-Item -ItemType Directory -Force "C:\Program Files\Common Files\OFX\Plugins" | Out-Null
Copy-Item -Recurse -Force .\Datamosh.ofx.bundle "C:\Program Files\Common Files\OFX\Plugins\"
```

**macOS**

```sh
sudo mkdir -p /Library/OFX/Plugins
sudo cp -R Datamosh.ofx.bundle /Library/OFX/Plugins/
# the binary is not notarised; clear the quarantine flag so Resolve may load it
sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/Datamosh.ofx.bundle
```

## Uninstall

The installers remove the plugin with `--uninstall` (`-Uninstall` on
Windows). Only `Datamosh.ofx.bundle` and the bundle of the old transition
version (`DatamoshTransition.ofx.bundle`) are deleted; other plugins in the
same directory are left alone. If you installed into a custom directory, pass
the same `--dest DIR` / `-Dest DIR`.

```sh
# Linux / macOS, from the repository or an unpacked release archive
./install.sh --uninstall
# without a local copy of the script
curl -fsSL https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.sh | sh -s -- --uninstall
```

```powershell
# Windows, in PowerShell started as Administrator
.\install.ps1 -Uninstall
# without a local copy of the script
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Woysful/ofx-datamosh/main/install.ps1))) -Uninstall
```

Or delete the bundle by hand:

| OS | Command |
|---|---|
| Linux | `sudo rm -rf /usr/OFX/Plugins/Datamosh.ofx.bundle` |
| macOS | `sudo rm -rf /Library/OFX/Plugins/Datamosh.ofx.bundle` |
| Windows (Administrator) | `Remove-Item -Recurse -Force "C:\Program Files\Common Files\OFX\Plugins\Datamosh.ofx.bundle"` |

Restart the host afterwards.
