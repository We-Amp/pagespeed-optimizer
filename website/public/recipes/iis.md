# IIS: install and verify mod_pagespeed 1.15 (native IIS module)

Source: https://modpagespeed.com/docs/install-iis/ and
https://modpagespeed.com/docs/iis-configuration/. Header: `X-Page-Speed`.
The IIS module ships from the 1.15 packaging channel. All commands run in an
elevated PowerShell on the Windows Server host.

## 1. Prerequisites

Windows Server 2019 or later, IIS 10 or later, 64-bit, the Visual C++
Redistributable 2022, and no IISpeed on the host: both register the same
handler, so uninstall IISpeed first and run `iisreset`.

```powershell
(Get-CimInstance Win32_OperatingSystem).Caption
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\InetStp').MajorVersion   # 10 or later
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue).Installed   # 1
$uninstall = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
Get-ItemProperty $uninstall -ErrorAction SilentlyContinue | Where-Object DisplayName -like 'IISpeed*' | Select-Object DisplayName           # nothing
Get-ItemProperty $uninstall -ErrorAction SilentlyContinue | Where-Object DisplayName -like '*PageSpeed for IIS*' | Select-Object DisplayName   # nothing
Import-Module WebAdministration
Get-Website | Select-Object Name, State, PhysicalPath   # the sites on this host
```

Stop and ask if the module is already installed. On a host with more than one
site, ask the operator which sites to optimize.

## 2. Install

Find the current MSI on https://modpagespeed.com/download/ (section "IIS
(Windows)"), download it and the release's `SHA256SUMS` from the same release
folder, and stop on a checksum mismatch:

```powershell
$ProgressPreference = 'SilentlyContinue'
$page = (Invoke-WebRequest 'https://modpagespeed.com/download/' -UseBasicParsing).Content
$rel = [regex]::Match($page, '/releases/v[0-9][^/"]*/pagespeed-iis-[^/"]+-win-x64\.msi').Value
if (-not $rel) { throw 'IIS MSI link not found on /download/' }
$base = 'https://modpagespeed.com' + $rel.Substring(0, $rel.LastIndexOf('/') + 1)
$name = Split-Path $rel -Leaf
$dir = Join-Path $env:TEMP 'pagespeed-install'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$msi = Join-Path $dir $name
Invoke-WebRequest ($base + $name) -OutFile $msi -UseBasicParsing
Invoke-WebRequest ($base + 'SHA256SUMS') -OutFile (Join-Path $dir 'SHA256SUMS') -UseBasicParsing
$line = Select-String -Path (Join-Path $dir 'SHA256SUMS') -Pattern ('\s\*?' + [regex]::Escape($name) + '$') | Select-Object -First 1
if (-not $line) { throw "$name is not listed in SHA256SUMS" }
$expected = $line.Line.Split(' ')[0]
if ((Get-FileHash -Algorithm SHA256 $msi).Hash -ne $expected) { throw "SHA256 mismatch for $name" }
$msi
```

Install silently and fail on any exit code other than 0 or 3010 (3010: installed,
a reboot is pending; tell the operator):

```powershell
$log = Join-Path $dir 'install.log'
$p = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /qn /norestart /l*v `"$log`"" -Wait -PassThru
if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { throw "msiexec failed with exit code $($p.ExitCode); log: $log" }
iisreset
```

The installer registers the module as a native HTTP module and creates the
default cache directory, `%ProgramData%\We-Amp\PageSpeed\cache`. In releases
that bundle the optimizer it also installs the optimizer worker as the Windows
service `WeAmpPageSpeedOptimizer`, left disabled; the module works without it.

## 3. Minimal configuration

A site is optimized only when its own application root holds a
`pagespeed.config`. The server-level file
`%ProgramData%\We-Amp\PageSpeed\pagespeed.config` (the installer creates it)
sets defaults for every site that has its own `pagespeed.config`; it turns
nothing on by itself. The installer also puts a copy with `pagespeed on` in the
Default Web Site root, so that site is on after install.

For each site the operator chose:

```powershell
Import-Module WebAdministration
$site = '<site-name>'
$root = [Environment]::ExpandEnvironmentVariables((Get-Website | Where-Object Name -eq $site).PhysicalPath)
$cfg = Join-Path $root 'pagespeed.config'
if (Test-Path $cfg) { Get-Content $cfg } else { Set-Content -Path $cfg -Value 'pagespeed on' -Encoding Ascii; "created $cfg" }
Get-WebApplication -Site $site | Select-Object Path, PhysicalPath   # nested applications
```

If the file already exists, do not overwrite it: it must contain
`pagespeed on`; otherwise stop and ask. A nested IIS application is optimized
only with its own `pagespeed.config` in its own physical path; ask before
adding those. The cache path and other defaults come from the server-level
file. Path matching and the directive list:
https://modpagespeed.com/docs/iis-configuration/

## 4. Verify

```powershell
(Invoke-WebRequest '<site-url>/?mps-verify=agent' -UseBasicParsing).Headers['X-Page-Speed']
```

Pass: the module version is printed; the header is on every HTML response the
module handles. A new per-site file is picked up on the next request; if
nothing is printed, run `iisreset` once and verify again. Still nothing:
https://modpagespeed.com/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header

## 5. Rollback

```powershell
# Off for one site: remove its own pagespeed.config. A server-level
# `pagespeed off` does not turn off a site whose own file says `pagespeed on`.
$root = [Environment]::ExpandEnvironmentVariables((Get-Website | Where-Object Name -eq '<site-name>').PhysicalPath)
$cfg = Join-Path $root 'pagespeed.config'
$createdByThisRun = $true   # $false when the file was there before this run
if ($createdByThisRun) { Remove-Item $cfg } else { Rename-Item $cfg 'pagespeed.config.disabled' }

# Remove the module with the same MSI, then restart IIS
$dir = Join-Path $env:TEMP 'pagespeed-install'
$msi = Join-Path $dir 'pagespeed-iis-<version>-win-x64.msi'
if (-not (Test-Path $msi)) { throw 'MSI not found in TEMP; uninstall mod_pagespeed from Apps and features instead' }
$p = Start-Process msiexec.exe -ArgumentList "/x `"$msi`" /qn /norestart /l*v `"$dir\uninstall.log`"" -Wait -PassThru
if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { throw "msiexec failed with exit code $($p.ExitCode)" }
iisreset
```

The cache and configuration under `%ProgramData%\We-Amp\` and the per-site
`pagespeed.config` files stay on disk. Rolling back a release:
https://modpagespeed.com/docs/uninstall/#roll-back-to-the-previous-release
