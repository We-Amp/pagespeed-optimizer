---
title: 'cPanel / EasyApache 4'
description: 'Install, uninstall, and maintain mod_pagespeed on cPanel / EasyApache 4: the signed EA4 RPM, enabling it in WHM Customize, the cpanel/elevate OS-upgrade runbook, and how the Apache Module Magic Number keeps routine ea-apache24 updates from breaking the module.'
order: 12
group: 'Install'
datePublished: 2026-05-20
lastUpdated: 2026-09-18
---

mod_pagespeed ships as a signed EasyApache 4 RPM, `ea-apache24-mod_pagespeed`, served from `packages.modpagespeed.com`. This is the package name Google's archived [apache/incubator-pagespeed-cpanel](https://github.com/apache/incubator-pagespeed-cpanel) repo used before it went dormant; We-Amp maintains the EA4 build today.

EasyApache 4 ships only on RHEL-family hosts. The EA4 RPM is built for **EL8 (AlmaLinux 8, Rocky 8, CloudLinux 8), EL9 (AlmaLinux 9, Rocky 9, RHEL 9, CloudLinux 9), and EL10, x86_64**. Each EL major has its own RPM (the module is built against that major's glibc); pick the tree matching your host's release. arm64 is not built. The module pairs with the `pagespeed-optimizer` worker on EL9 and EL10; on EL8, the module runs without the optimizer worker.

On CloudLinux 8/9, mod_pagespeed is an Apache output filter and runs on the standard EA4 Apache (including hosts using `mod_lsapi` as the PHP handler — `mod_lsapi` is an Apache handler, not LiteSpeed Web Server). It does not apply to LiteSpeed Web Server (LSWS), which is a separate, non-Apache server. The EL8 build is validated on AlmaLinux 8 / Rocky 8 stock EA4; if you run CloudLinux's `cl-ea4`-patched Apache and hit anything unexpected, [let us know](/contact/).

## Install

### 1. Add the EA4 yum repository

```bash
sudo tee /etc/yum.repos.d/modpagespeed-ea4.repo >/dev/null <<'EOF'
[ea4]
name=mod_pagespeed for EasyApache 4
baseurl=https://packages.modpagespeed.com/yum/ea4/el$releasever/x86_64/
enabled=0
repo_gpgcheck=1
gpgcheck=1
gpgkey=https://packages.modpagespeed.com/pubkey.gpg
EOF
```

`$releasever` resolves to `8` on EL8 hosts, `9` on EL9, and `10` on EL10, so dnf picks the matching tree automatically — the same `.repo` file works on both. `enabled=0` keeps the EA4 tree out of routine `dnf update` runs; you opt in per command with `--enablerepo=ea4` below. The RPMs and the repository metadata are both signed with key `rsa4096/F50D6054F10712A0` — the same key as the rest of `packages.modpagespeed.com`, which is why both `repo_gpgcheck=1` and `gpgcheck=1` are set.

### 2. Install the module

```bash
sudo dnf install --enablerepo=ea4 ea-apache24-mod_pagespeed
```

### 3. Turn it on in WHM

The RPM drops the module file but does not flip it on. Apache loads `mod_pagespeed` only after you tick the box in WHM and rebuild:

1. WHM → _Software_ → _EasyApache 4_ → _Customize_ on the current profile.
2. Open the _Apache Modules_ step. `ea-apache24-mod_pagespeed` appears alongside the other EA4 modules.
3. Tick the checkbox, then _Review_ → _Provision_. EasyApache 4 rebuilds Apache.
4. When the provision finishes the module is loaded and the filter runs on every vhost served by `ea-apache24-httpd`.

The package installs its configuration to `/etc/apache2/conf.d/pagespeed.conf` (marked `%config(noreplace)`, so your edits survive a reinstall or upgrade). Tune it the same way you would on any RHEL Apache host — see [Getting Started](/docs/getting-started/) for the directive reference.

### 4. Open the admin console

The global admin console lives at:

```
https://your-host.example.com/pagespeed_global_admin
```

Open `/pagespeed_global_admin` on any vhost — the admin route is global to the server, not scoped to one vhost (`/pagespeed_admin/` is the per-vhost statistics endpoint). The module installs and fully optimizes out of the box: mod_pagespeed is licensed under the Apache License 2.0, free in development and in production. See [Downloads & Licensing](/download/).

There is nothing to license per site or per server. cPanel hosts running many sites can join the [Hoster partner program](/hosting-partners/) for backing across the fleet, set up through the [contact form](/contact/). Plans: [pricing](/pricing/). Support terms: [we-amp.com/licensing](https://we-amp.com/licensing/).

## Uninstall

```bash
sudo dnf remove ea-apache24-mod_pagespeed
```

The package's uninstall script reloads EA4 Apache (`/scripts/restartsrv_httpd`), so the module is dropped immediately — no provision needed. Your `/etc/apache2/conf.d/pagespeed.conf` stays on disk, so a later reinstall picks it up unchanged. To remove the configuration as well, delete `/etc/apache2/conf.d/pagespeed.conf` (and `pagespeed_libraries.conf`) after the package is gone. To stop the repo from being offered at all:

```bash
sudo rm -f /etc/yum.repos.d/modpagespeed-ea4.repo
```

## Before an OS upgrade (cpanel/elevate)

Any third-party EA4 module — `ea-apache24-mod_pagespeed` included — blocks `cpanel/elevate` OS-major upgrades (for example EL8 → EL9 or EL9 → EL10), the same way `mod_passenger` does. cPanel's elevate preflight stops on the installed package; the upgrade does not start.

Because mod_pagespeed is built for **both EL8 and EL9**, an EL8 → EL9 upgrade has a target-OS package waiting — you reinstall the EL9 build after elevate finishes (an EL8-only module would leave you with nothing to reinstall). The remove-before / reinstall-after flow below is still required, because elevate's preflight halts on any third-party module regardless of whether a target-OS build exists.

Remove the module before the upgrade window and reinstall after:

1. **Remove the module.**

   ```bash
   sudo dnf remove ea-apache24-mod_pagespeed
   ```

   EA4 Apache reloads without it. `/etc/apache2/conf.d/pagespeed.conf` stays on disk.

2. **Run the OS upgrade.**

   ```bash
   sudo /scripts/elevate-cpanel
   ```

   EasyApache 4 migrates to the new EL release as part of elevate.

3. **Reinstall on the new EL release.** The `$releasever`-based `baseurl` picks up the new release's tree (`el9` or `el10`) automatically, then:

   ```bash
   sudo dnf install --enablerepo=ea4 ea-apache24-mod_pagespeed
   ```

   Re-tick the box in WHM → _EasyApache 4_ → _Customize_ if the post-elevate provision dropped it. The existing config is picked up unchanged. The post-install script also writes a one-line reminder to `/etc/motd` so it surfaces at the next SSH session before the upgrade window.

## Current limits

- **arm64.** EA4 RPMs ship x86_64 only. arm64 isn't packaged yet; [contact us](/contact/) if it blocks you.
- **Ubuntu EA4.** cPanel's Ubuntu EA4 support is partial / preview. We ship RHEL-family only — EL8, EL9, and EL10 (AlmaLinux, Rocky, RHEL, CloudLinux).
- **WHM plugin UI.** Config still lives in `pagespeed.conf` and per-vhost includes. There's no WHM plugin for point-and-click config yet.

## MMN / Apache ABI FAQ

The module is built against EasyApache 4's Apache ABI, identified by the Apache **Module Magic Number (MMN)**. cPanel hardcodes the MMN at `20120211` and the package pins it with `Requires: ea-apache24-mmn`.

### Does a routine `ea-apache24` update break the module?

No. The MMN `20120211` has been stable across the entire Apache 2.4 series — cPanel sets `%define mmn 20120211` in its `ea-apache24` spec, and a minor `ea-apache24` bump does not change it. Routine `ea-apache24` updates keep the same MMN, so a `yum update` does not disturb the installed module; it keeps loading on the new Apache build.

### What is `Requires: ea-apache24-mmn` for, then?

It is the safety pin for the one case that would matter: a true Apache ABI break (a future Apache 2.6 / 3.0, which would move the MMN). The package requires the MMN it was built against. If Apache's ABI ever changed, that requirement makes `yum update` **hold** the Apache upgrade with a clear dependency message instead of loading an incompatible module — your site keeps serving the existing Apache and module. `dnf`/`yum` default to skip-broken; `--allowerasing` is not the default, so the update is blocked, not silently resolved by removing the module.

### Will a `yum update` ever silently unload the module?

No. Either the MMN is unchanged (the normal case) and the module keeps loading, or a hypothetical ABI break trips the `ea-apache24-mmn` requirement and `yum`/`dnf` holds the Apache upgrade with a dependency message. Neither path removes a loaded module out from under a running `httpd`. As a further fail-safe the loader fragment is `<IfFile>`-guarded, so if the module file is ever absent, Apache starts as plain Apache rather than failing to start.

### Do you keep up with EA4?

Yes. We track `ea-apache24` releases and publish a matching signed rebuild when an ABI change requires one — through the same signed repo, so `dnf upgrade --enablerepo=ea4 ea-apache24-mod_pagespeed` picks up the current build.

### Where do I check which Apache the module is built for?

```bash
rpm -q --requires ea-apache24-mod_pagespeed   # lists the ea-apache24-mmn requirement
rpm -q ea-apache24-mod_pagespeed              # installed package version
httpd -M 2>/dev/null | grep pagespeed         # confirm the module is loaded
```

If it does not appear, re-check the _Apache Modules_ step in WHM Customize and re-provision. See [Troubleshooting](/docs/troubleshooting/) for the general "module not running" checklist.

### `rpm -q` shows a leading `2:` (e.g. `2:1.15.0-...`) — is that version 2?

No. The leading `2:` is the RPM **epoch**, not a major version — the installed package's version string currently reads **1.15.0**. The epoch is a packaging ordering hint that sits above the version number; it ensures `dnf`/WHM consistently selects and upgrades to our signed build by epoch first, independent of the version string. WHM and `dnf` may surface the `2:` prefix in a few places; read it as "epoch 2, version 1.15.0".

---

cPanel, WHM, and EasyApache are trademarks of cPanel, L.L.C. We-Amp B.V. is not affiliated with or endorsed by cPanel, L.L.C.
