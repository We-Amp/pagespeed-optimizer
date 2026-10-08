---
title: 'Uninstall and rollback'
description: 'Remove mod_pagespeed 2.1 per shape (apt, dnf, cPanel, Docker, Helm, IIS, NuGet), what each leaves behind, cache cleanup, and rolling back a release.'
order: 56
group: 'Operate'
lastUpdated: 2026-10-08
datePublished: 2026-10-08
faq:
  - q: 'Does removing the packages delete the cache?'
    a: 'Not on `remove`. On Debian and Ubuntu, `apt-get purge` also deletes the worker cache at its default location, `/var/cache/pagespeed-optimizer`; a relocated cache directory and the module cache under `FileCachePath` are yours to delete, on every distribution.'
  - q: 'Can I roll back to the previous release?'
    a: 'Yes. Roll the module and the `pagespeed-optimizer` worker back together, optimizer first. After a release that changed the cache format the previous release files are still on disk, so the rollback starts with a warm cache.'
---

Removing mod_pagespeed 2.1 is the reverse of installing it: take the module
out of the web server, stop the worker, then decide what to do with the cache
and configuration each shape leaves behind. The last section covers rolling
back to a previous release instead.

Before you remove the module, remove or comment out the `pagespeed` directives
and, on nginx, the `load_module` line: a web server that starts with
directives from a module that is gone fails to start.

## Apache and nginx packages

```bash
# Debian / Ubuntu: keep the configuration files, or purge them
sudo apt-get remove mod-pagespeed pagespeed-optimizer            # Apache
sudo apt-get remove nginx-module-pagespeed pagespeed-optimizer   # nginx
sudo apt-get purge  mod-pagespeed pagespeed-optimizer            # also removes configuration and the default cache
# AlmaLinux / RHEL / Rocky
sudo dnf remove mod-pagespeed pagespeed-optimizer
sudo dnf remove nginx-module-pagespeed pagespeed-optimizer
sudo systemctl restart apache2   # httpd on Enterprise Linux, or nginx
```

What stays after `remove`:

- The module configuration: `pagespeed.conf` and `pagespeed_daemon.conf` under
  `/etc/apache2/` (Debian and Ubuntu) or `/etc/httpd/conf.d/` (Red Hat family),
  and your own nginx `pagespeed` directives.
- The worker's host overrides, `/etc/default/pagespeed-optimizer`, and
  `/etc/pagespeed-optimizer/daemon.env`.
- The worker cache at `/var/cache/pagespeed-optimizer`. `purge` removes only
  this default location; a relocated cache directory is yours to remove, and
  so is the module's own cache under `FileCachePath`
  (`/var/cache/ngx_pagespeed` in the nginx examples).
- The repository: `/etc/apt/sources.list.d/modpagespeed.list` and
  `/usr/share/keyrings/modpagespeed-archive-keyring.gpg`, or
  `/etc/yum.repos.d/modpagespeed.repo` and
  `/etc/pki/rpm-gpg/RPM-GPG-KEY-modpagespeed`. Delete them to stop the
  repository from being offered.

On cPanel / EasyApache 4: `sudo dnf remove ea-apache24-mod_pagespeed`. The
package's uninstall script reloads EA4 Apache, so the module is gone at once;
`/etc/apache2/conf.d/pagespeed.conf` stays on disk. See
[cPanel / EasyApache 4](/docs/cpanel/#uninstall).

## Docker

```bash
docker compose down        # stop and remove the containers; the cache volume stays
docker compose down -v     # also remove the cache volume
docker rmi ghcr.io/we-amp/pagespeed-nginx:<tag> ghcr.io/we-amp/pagespeed-worker:<tag>
```

A `docker run` of the combined image stops with `docker stop`; `--rm` removes
the container, and `docker rmi ghcr.io/we-amp/pagespeed-combined:latest`
removes the image.

## Helm

```bash
helm uninstall pagespeed
```

This removes every resource the chart created (Deployment, Service, Ingress,
HPA, ConfigMap). The `emptyDir` cache goes with the pods.

## IIS

1. Remove the `pagespeed` directives you no longer want, or leave the
   `pagespeed.config` files in place for a later reinstall.
2. _Apps & features_ (or _Programs and Features_) → uninstall the mod_pagespeed
   IIS module, then `iisreset`.
3. The cache and configuration directories under `%ProgramData%\We-Amp\` stay
   on disk; delete them for a clean slate.

## ASP.NET Core

```bash
dotnet remove package WeAmp.PageSpeed.AspNetCore
```

Remove `AddPageSpeed()` and `UsePageSpeed()` from `Program.cs` and the
`PageSpeed` section from `appsettings.json`. The cache volume lives at the
path you configured under `PageSpeed:Cache`; delete it once the app no longer
runs the middleware. See
[ASP.NET Core settings](/docs/aspnet-configuration/#cache-volume).

## Roll back to the previous release

Roll the module and the `pagespeed-optimizer` worker back together: they are a
matched pair, and a module and a worker on different cache formats do not
share a cache. Start the optimizer first, then the web server.

**Packages.** If the repository still offers the previous version, pin it:

```bash
# Debian / Ubuntu: list the versions on offer, then pin both
apt-cache policy mod-pagespeed pagespeed-optimizer
sudo apt-get install mod-pagespeed=<version> pagespeed-optimizer=<version>
# AlmaLinux / RHEL / Rocky
dnf --showduplicates list mod-pagespeed pagespeed-optimizer
sudo dnf downgrade mod-pagespeed-<version> pagespeed-optimizer-<version>
sudo systemctl restart pagespeed-optimizer && sudo systemctl restart apache2   # or nginx
```

Otherwise install the `.deb` or `.rpm` files you kept from the previous
release (`dpkg -i`, or `dnf install ./<file>.rpm`). After a release that
changed the cache format, the previous release's cache files were left on
disk, so the rollback starts with a warm cache; on nginx, only until you ran
the one-time cache flush that release asked for.

**Docker.** Set the previous tag in `deploy/.env` (`PAGESPEED_VERSION=`) and
run `docker compose up -d`; both images move together.

**Helm.** `helm history pagespeed` lists the revisions, and
`helm rollback pagespeed <revision>` returns to one.

**IIS.** Uninstall the current module, run the previous MSI, `iisreset`.

**ASP.NET Core.** Pin the previous package version in the project file
(`<PackageReference Include="WeAmp.PageSpeed.AspNetCore" Version="<version>" />`)
and redeploy.
