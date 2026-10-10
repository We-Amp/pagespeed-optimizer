# IIS: install and verify mod_pagespeed (native IIS module)

Source: https://modpagespeed.com/docs/install-iis/ and
https://modpagespeed.com/docs/iis-configuration/. Header: `X-Page-Speed`.
All commands run in an elevated PowerShell on the Windows Server host.

## 1. Prerequisites

Windows Server 2019 or later, IIS 10 or later, 64-bit, the Visual C++
Redistributable 2022, and no IISpeed on the host: both register the same
handler, so uninstall IISpeed first and run `iisreset`.

```powershell
(Get-CimInstance Win32_OperatingSystem).Caption
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\InetStp').MajorVersion   # 10 or later
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue).Installed   # 1
Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*' | Where-Object DisplayName -like 'IISpeed*' | Select-Object DisplayName   # nothing
```

## 2. Install

Download the IIS MSI and its `.asc` signature from
https://modpagespeed.com/download/ (section "IIS (Windows)"; the file is named
`pagespeed-iis-<version>-win-x64.msi`), then:

```powershell
Start-Process msiexec.exe -ArgumentList '/i', '.\pagespeed-iis-<version>-win-x64.msi', '/qn', '/norestart' -Wait
iisreset
```

The installer registers the module as a native HTTP module and creates the
default cache directory. It also installs the optimizer worker as the Windows
service `WeAmpPageSpeedOptimizer`, left disabled; the module works without it.

## 3. Minimal configuration

The module optimizes every site that has a `pagespeed.config`. Create the
server-level file if it does not exist; the module reloads it within a second,
no `iisreset` needed:

```powershell
$cfg = "$env:ProgramData\We-Amp\PageSpeed\pagespeed.config"
if (-not (Test-Path $cfg)) {
  New-Item -ItemType Directory -Force -Path (Split-Path $cfg) | Out-Null
  Set-Content -Path $cfg -Value "pagespeed on`r`npagespeed FileCachePath %ProgramData%\We-Amp\IISWebSpeed\Cache"
}
Get-Content $cfg
```

Per-site files, path matching and the directive list:
https://modpagespeed.com/docs/iis-configuration/

## 4. Verify

```powershell
(Invoke-WebRequest '<site-url>/?mps-verify=agent' -Method Head -UseBasicParsing).Headers['X-Page-Speed']
```

Pass: the module version is printed; the header is on every HTML response the
module handles. Nothing printed:
https://modpagespeed.com/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header

## 5. Rollback

```powershell
# Off without removing: append `pagespeed off` to the config
Add-Content -Path "$env:ProgramData\We-Amp\PageSpeed\pagespeed.config" -Value 'pagespeed off'

# Remove the module with the same MSI, then restart IIS
Start-Process msiexec.exe -ArgumentList '/x', '.\pagespeed-iis-<version>-win-x64.msi', '/qn', '/norestart' -Wait
iisreset
```

The cache and configuration under `%ProgramData%\We-Amp\` stay on disk.
Rolling back a release: https://modpagespeed.com/docs/uninstall/#iis
