---
title: 'Migrate from the archived Google module'
description: 'Move from the archived open-source mod_pagespeed or ngx_pagespeed binaries to mod_pagespeed 2.1 in three commands: what maps to what, cache, rollback.'
order: 15
group: 'Upgrade and migrate'
lastUpdated: 2026-10-18
datePublished: 2026-10-08
faq:
  - q: 'Is mod_pagespeed 2.1 a drop-in replacement for the archived module?'
    a: 'Yes. It continues the same codebase, keeps the `pagespeed` and `ModPagespeed` directive surface and the classic filter names, and loads in process on Apache and nginx. Your configuration, `.htaccess` directives included, carries over.'
  - q: 'What do I lose?'
    a: 'Nothing you configured. A handful of long-inert filters and experimental options are accepted and ignored with a warning, and the old file cache is left in place while the new Cyclone cache warms up.'
---

mod_pagespeed is an open-source project originally developed at Google. It was
donated to the Apache Software Foundation in 2017, the Incubator podling
retired in 2023, and the repositories became read-only in 2025; the 1.13.35.2
binaries still install but have had no security fixes since. mod_pagespeed
2.1, developed by We-Amp B.V. and not affiliated with or endorsed by Google,
continues that codebase: same directives, same filter names, built against
current Apache and nginx, with ongoing security fixes. The move is three
commands.

## The three commands

```bash
# 1. Add the signed repository; the script detects apt or dnf
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
# 2. Install the module for your server
sudo apt-get install mod-pagespeed mod-pagespeed-stable-   # Apache (dnf on Enterprise Linux: see below)
sudo apt-get install nginx-module-pagespeed                # nginx
# 3. Check the configuration, then restart the web server
sudo apache2ctl configtest && sudo systemctl restart apache2   # or: sudo nginx -t && sudo systemctl restart nginx
```

The web server keeps serving with the module it has loaded until the
restart in step 3, so the restart is the only interruption. Each step is
spelled out below for Apache on Debian and Ubuntu, Apache on Enterprise
Linux, and nginx.

## Before you start

Record the version you have, and keep a copy of your configuration and of the
archived package for a rollback:

```bash
curl -sI http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
# Apache on Debian or Ubuntu
sudo cp -a /etc/apache2/mods-available/pagespeed.conf ~/pagespeed.conf.google
curl -fsSLO https://dl.google.com/linux/direct/mod-pagespeed-stable_current_amd64.deb
# Apache on Enterprise Linux
sudo cp -a /etc/httpd/conf.d/pagespeed.conf ~/pagespeed.conf.google
curl -fsSLO https://dl.google.com/linux/direct/mod-pagespeed-stable_current_x86_64.rpm
# nginx, dynamic module
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so{,.bak}
```

If you run the archived beta, write `mod-pagespeed-beta` wherever this page
says `mod-pagespeed-stable`, here and below.

## Apache on Debian and Ubuntu

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install mod-pagespeed mod-pagespeed-stable-
sudo apache2ctl configtest && sudo systemctl restart apache2
sudo apt-get purge mod-pagespeed-stable
```

- **The archived apt source.** The archived package added its own source,
  `/etc/apt/sources.list.d/mod-pagespeed.list`. Its signing key no longer
  verifies, so while it is enabled every `apt-get update` fails with
  `NO_PUBKEY 78BD65473CB3BD13` and "is not signed". The install script
  renames the file to `mod-pagespeed.list.disabled`, which apt ignores; to do
  it by hand, run
  `sudo mv /etc/apt/sources.list.d/mod-pagespeed.list{,.disabled}`.
- **One step, no gap.** The trailing `-` on `mod-pagespeed-stable-` removes
  the archived package in the same run that installs the new one. Both ship
  the same files, so installing the new package while the archived one is
  still there fails with "trying to overwrite". Do not remove the archived
  package on its own first: that disables the module before the new one is in
  place.
- **If you edited `pagespeed.conf`,** dpkg stops and asks:
  `*** pagespeed.conf (Y/I/N/O/D/Z) [default=N] ?`. Answer `N` to keep your
  file. Its directives carry over unchanged, and the module itself is loaded
  from `pagespeed.load`, which the package replaces. The new defaults are
  saved beside it as `pagespeed.conf.dpkg-dist`; compare them with
  `diff /etc/apache2/mods-available/pagespeed.conf{,.dpkg-dist}`. A run
  without a terminal cannot answer and stops with "end of file on stdin at
  conffile prompt"; for an unattended run, keep your file with
  `sudo apt-get install -y -o Dpkg::Options::=--force-confold mod-pagespeed mod-pagespeed-stable-`.
- **Purge, do not just remove.** After the swap the archived package is left
  in the "config files" state, with its daily cron job and
  `/etc/default/mod-pagespeed`. `apt-get purge mod-pagespeed-stable` deletes
  those; it leaves `pagespeed.conf` alone, which now belongs to the new
  package.

## Apache on Enterprise Linux

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo dnf swap mod-pagespeed-stable mod-pagespeed
sudo apachectl configtest && sudo systemctl restart httpd
```

`dnf swap` removes the archived package and installs the new one in one
transaction. If you edited `/etc/httpd/conf.d/pagespeed.conf`, rpm keeps your
file and writes the new one as `pagespeed.conf.rpmnew`. On Enterprise Linux
the `LoadModule` line lives in that file, and yours names
`mod_pagespeed_ap24.so`, which the new package does not ship, so
`configtest` fails until you take the new file and carry your edits over:

```bash
sudo mv /etc/httpd/conf.d/pagespeed.conf.rpmnew /etc/httpd/conf.d/pagespeed.conf
diff ~/pagespeed.conf.google /etc/httpd/conf.d/pagespeed.conf
```

Because `configtest` runs first, Apache keeps serving on the old module until
the configuration is valid.

## nginx

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install nginx-module-pagespeed    # dnf on Enterprise Linux
sudo nginx -t && sudo systemctl restart nginx
```

Make sure one `load_module` line names
`/usr/lib/nginx/modules/ngx_pagespeed_module.so`, the file the package
installs. If you compiled ngx_pagespeed into nginx, you are also moving to the
distribution's nginx package; see
[Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/) for the
version pinning that matters there.

## What maps to what

| You have                                                                | You get                                                                                                                                                              |
| ----------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `mod-pagespeed-stable` or `mod-pagespeed-beta` (Apache)                 | `mod-pagespeed` from packages.modpagespeed.com; it depends on the `pagespeed-optimizer` worker and installs the configuration that points the module at it          |
| `ngx_pagespeed` compiled into nginx, or a third-party package           | `nginx-module-pagespeed`, a dynamic module pinned to your distribution's stock nginx                                                                                  |
| `ModPagespeed*` directives, `pagespeed` directives, `.htaccess`          | Unchanged                                                                                                                                                            |
| The filter names                                                        | Unchanged; the long-inert and experimental ones are accepted and ignored with a warning (see the [compatibility table](/docs/migrate-from-ngx-pagespeed/#configuration-compatibility)) |
| `ModPagespeedFileCachePath` / `pagespeed FileCachePath`                  | Accepted; the module keeps a Cyclone volume under it, and the old per-resource cache files in that directory are ignored                                             |
| Memcached or Redis as external cache                                    | Still supported; see [cache modes](/docs/cache-modes/#external-cache-backends-memcached-and-redis)                                                                   |
| `/pagespeed_admin`, `/pagespeed_global_admin`, statistics and messages  | Same paths, with a reworked console; restrict them as before                                                                                                         |
| `X-Mod-Pagespeed` (Apache) / `X-Page-Speed` (nginx)                     | Unchanged                                                                                                                                                            |
| `.pagespeed.` resource URLs, the beacon, `?PageSpeed=off`                | Unchanged                                                                                                                                                            |
| Nothing                                                                 | The optimizer worker: a separate process that builds optimized variants beside the module. The Apache packages wire it up; on nginx two directives do                |

## The cache

The module uses Cyclone Cache, a different cache backend. On first start after
the move, the old per-resource file cache is ignored, not deleted; the Cyclone
volume starts empty and resources are re-optimized on first request, so expect
a brief warm-up. The warm-up is a one-time cost: cache contents persist across
restarts. Once the move has run for a while, remove the old per-resource files
from the file-cache directory and leave the Cyclone volume beside them alone.
For cache sizing and storage options, see
[cache sizing](/docs/cache-modes/#cache-sizing).

## Verify

```bash
curl -I http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
```

The header value is the new module version. Open `/pagespeed_global_admin` on
the server to see the configuration and statistics;
[Is it working?](/docs/is-it-working/) has the rest.

## Roll back

Rolling back reinstalls the archived package you saved under
[Before you start](#before-you-start) and puts your configuration back. If
you no longer have the file, the same URL still serves 1.13.35.2.

### Apache on Debian and Ubuntu

```bash
sudo apt-get purge mod-pagespeed pagespeed-optimizer
sudo touch /etc/default/mod-pagespeed
sudo apt-get install ./mod-pagespeed-stable_current_amd64.deb
sudo cp ~/pagespeed.conf.google /etc/apache2/mods-available/pagespeed.conf
sudo apache2ctl configtest && sudo systemctl restart apache2
```

The purge removes the new module, the worker and their configuration; your
own configuration comes back from the copy. The empty
`/etc/default/mod-pagespeed` stops the archived package from adding its apt
source again, which would break `apt-get update` once more. Apache serves on
the module it has loaded until the restart.

### Apache on Enterprise Linux

```bash
sudo mv /etc/yum.repos.d/modpagespeed.repo{,.disabled}
sudo dnf swap mod-pagespeed ./mod-pagespeed-stable_current_x86_64.rpm
sudo cp ~/pagespeed.conf.google /etc/httpd/conf.d/pagespeed.conf
sudo apachectl configtest && sudo systemctl restart httpd
```

The new package declares that it replaces the archived one, so disable the
repository first, or a later `dnf upgrade` swaps it back. `dnf swap` also
removes the worker, which nothing needs any more.

### nginx

```bash
sudo apt-get remove nginx-module-pagespeed     # dnf on Enterprise Linux
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so.bak /usr/lib/nginx/modules/ngx_pagespeed_module.so
sudo nginx -t && sudo systemctl restart nginx
```

Remove the package before you restore the file: the package owns that path,
and removing it afterwards would delete the module you restored.

After a rollback the header reads `X-Mod-Pagespeed: 1.13.35.2-0` again. The
archived 1.0 documentation stays available at [/1.0/](/1.0/), and
[Is mod_pagespeed still maintained?](/mod-pagespeed-still-maintained/) covers
what changed and what did not.
