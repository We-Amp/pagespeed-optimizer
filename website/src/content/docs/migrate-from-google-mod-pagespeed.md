---
title: 'Migrate from the archived Google module'
description: 'Move from the archived mod_pagespeed or ngx_pagespeed binaries to mod_pagespeed 2: package swap, what maps to what, cache, rollback.'
order: 15
group: 'Upgrade and migrate'
lastUpdated: 2026-10-18
datePublished: 2026-10-08
faq:
  - q: 'Is mod_pagespeed 2 a drop-in replacement for the archived module?'
    a: 'Yes. It continues the same codebase, keeps the `pagespeed` and `ModPagespeed` directive surface and the classic filter names, and loads in process on Apache and nginx. Your configuration, `.htaccess` directives included, carries over.'
  - q: 'What do I lose?'
    a: 'Nothing you configured. A handful of long-inert filters and experimental options are accepted and ignored with a warning, and the old file cache is left in place while the new Cyclone cache warms up.'
---

mod_pagespeed is an open-source project originally developed at Google. It was
donated to the Apache Software Foundation in 2017, the Incubator podling
retired in 2023, and the repositories became read-only in 2025; the 1.13.35.2
binaries still install but have had no security fixes since. mod_pagespeed 2,
developed by We-Amp B.V. and not affiliated with or endorsed by Google,
continues that codebase: same
directives, same filter names, built against current Apache and nginx, with
ongoing security fixes. The move is a package swap and a restart.

## Before you start

Record the version you have, and keep a copy of your configuration and of the
archived package for a rollback. If you run the archived beta, write
`mod-pagespeed-beta` wherever this page says `mod-pagespeed-stable`, in the
file names below too.

```bash
curl -sI http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
# Apache on Debian or Ubuntu
sudo cp -a /etc/apache2/mods-available/pagespeed.conf ~/pagespeed.conf.google
curl -fsSL -o ~/mod-pagespeed-stable_current_amd64.deb https://dl.google.com/linux/direct/mod-pagespeed-stable_current_amd64.deb
# Apache on Enterprise Linux
sudo cp -a /etc/httpd/conf.d/pagespeed.conf ~/pagespeed.conf.google
curl -fsSL -o ~/mod-pagespeed-stable_current_x86_64.rpm https://dl.google.com/linux/direct/mod-pagespeed-stable_current_x86_64.rpm
# nginx: the module file and the load_module line that loads it
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so /usr/lib/nginx/modules/ngx_pagespeed_module.so.bak
# sudo cp /usr/lib64/nginx/modules/ngx_pagespeed_module.so /usr/lib64/nginx/modules/ngx_pagespeed_module.so.bak   # Enterprise Linux
sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx | tee ~/ngx_pagespeed-load_module.txt
```

## Apache on Debian and Ubuntu

```bash
# 1. Disable the archived package's apt source (skip if the file is gone)
sudo mv /etc/apt/sources.list.d/mod-pagespeed.list /etc/apt/sources.list.d/mod-pagespeed.list.disabled
sudo truncate -s 0 /etc/default/mod-pagespeed
# 2. Add the signed repository
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
# 3. Swap the packages in one run
sudo apt-get install mod-pagespeed mod-pagespeed-stable-
# 4. Check the configuration, then restart
sudo apache2ctl configtest && sudo systemctl restart apache2
# 5. Remove what the archived package left behind
sudo apt-get purge mod-pagespeed-stable
```

- **Step 1, the archived apt source.** The archived package added its own
  source, `/etc/apt/sources.list.d/mod-pagespeed.list`. Its signing key no
  longer verifies, so while it is enabled every `apt-get update` fails with
  `NO_PUBKEY 78BD65473CB3BD13` and "is not signed", and the install script in
  step 2 stops there. The archived package's daily cron job puts the source
  back while `/etc/default/mod-pagespeed` asks it to; an empty file turns that
  off. Newer versions of the install script do both for you.
- **Step 3, the swap.** The trailing `-` on `mod-pagespeed-stable-` removes
  the archived package explicitly, in the same run that installs the new one,
  so Apache never has its module configuration removed without a replacement.
  Do not remove the archived package on its own first. Apache keeps serving
  on the module it has loaded until the restart in step 4.
- **If you edited `pagespeed.conf`,** dpkg stops in step 3 and asks:
  `*** pagespeed.conf (Y/I/N/O/D/Z) [default=N] ?`. Answer `N` to keep your
  file. Its directives carry over unchanged; kept directives the new module
  no longer needs, such as `ModPagespeedFileCacheInodeLimit`, log a
  deprecation warning and still work. The new defaults are saved beside it as
  `/etc/apache2/mods-available/pagespeed.conf.dpkg-dist`; compare them with
  `diff /etc/apache2/mods-available/pagespeed.conf /etc/apache2/mods-available/pagespeed.conf.dpkg-dist`.
- **Unattended runs.** Without a terminal dpkg cannot answer the prompt, so
  keep your file with
  `sudo apt-get install -y -o Dpkg::Options::=--force-confold mod-pagespeed mod-pagespeed-stable-`.
  A run without a terminal and without the option stops with "end of file on
  stdin at conffile prompt" and exits 100, leaving `mod-pagespeed`
  unconfigured (`iU` in `dpkg -l`) and no PageSpeed module enabled in Apache.
  Apache keeps serving the archived module only until its next reload (the
  nightly log rotation does one), then serves without PageSpeed. To recover,
  run the unattended command above again, or
  `sudo dpkg --configure -a --force-confold`, then restart Apache.
- **Step 5, purge rather than remove.** After the swap the archived package is
  left in the "config files" state, with its daily cron job. The purge deletes
  the cron job and leaves `pagespeed.conf` alone, which now belongs to the new
  package. The empty `/etc/default/mod-pagespeed` from step 1 stays, which is
  harmless.

## Apache on Enterprise Linux

Do the swap right after the repository is configured:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo dnf swap mod-pagespeed-stable mod-pagespeed
sudo sed -i 's/mod_pagespeed_ap24\.so/mod_pagespeed.so/' /etc/httpd/conf.d/pagespeed.conf
sudo apachectl configtest && sudo systemctl restart httpd
```

`dnf swap` removes the archived package and installs the new one in one
transaction. If you edited `/etc/httpd/conf.d/pagespeed.conf`, rpm keeps your
file and writes the new one beside it as `pagespeed.conf.rpmnew`. On
Enterprise Linux the `LoadModule` line lives in that file, and yours names
`mod_pagespeed_ap24.so`, which packages up to 1.19.0 do not ship: the `sed`
line points it at `mod_pagespeed.so`, without which `configtest` fails. From
the release that ships `mod_pagespeed_ap24.so` as a link to the module, the
`sed` line is unnecessary and stays harmless. Compare your file with the new
defaults, then remove the `.rpmnew` file:

```bash
diff /etc/httpd/conf.d/pagespeed.conf /etc/httpd/conf.d/pagespeed.conf.rpmnew
sudo rm /etc/httpd/conf.d/pagespeed.conf.rpmnew
```

Kept directives the new module no longer needs log a deprecation warning and
still work. Because `configtest` runs first, Apache keeps serving on the old
module until the configuration is valid.

## nginx

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install nginx-module-pagespeed    # dnf on Enterprise Linux
sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx /usr/share/nginx/modules
# remove your own load_module line first; the grep above shows it
sudo nginx -t && sudo systemctl restart nginx
```

The package loads `ngx_pagespeed_module.so` itself; remove any `load_module`
line you added for the old module, so that the `grep` shows exactly one line,
the package's. The package installs its module at the path a hand-installed
`ngx_pagespeed_module.so` usually has, and overwrites that file, which is why
[Before you start](#before-you-start) keeps a `.bak` copy. If you compiled ngx_pagespeed into nginx, you are also moving
to the distribution's nginx package; see
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
curl -sI http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
```

The header now shows the version of the new package (1.19.0 or later), no
longer `1.13.35.2-0`: `X-Mod-Pagespeed` on Apache, `X-Page-Speed` on nginx. Open
`/pagespeed_global_admin` on the server to see the configuration and
statistics; [Is it working?](/docs/is-it-working/) has the rest.

## Roll back

Roll back after the restart if the site misbehaves on the new module. A
`configtest` failure does not need a rollback: the old module keeps serving
until the restart, so fix the configuration instead. Rolling back reinstalls
the archived package you saved under [Before you start](#before-you-start) and
puts your configuration back; if you no longer have the package, the same URL
still serves 1.13.35.2.

### Apache on Debian and Ubuntu

```bash
sudo apt-get purge mod-pagespeed pagespeed-optimizer
sudo truncate -s 0 /etc/default/mod-pagespeed
sudo apt-get install ~/mod-pagespeed-stable_current_amd64.deb
sudo cp ~/pagespeed.conf.google /etc/apache2/mods-available/pagespeed.conf
sudo apache2ctl configtest && sudo systemctl restart apache2
```

The purge removes the new module, the worker and their configuration, and
deletes the worker's cache directory; your own configuration comes back from
the copy. The empty `/etc/default/mod-pagespeed` stops the archived package
from adding its apt source again, which would break `apt-get update` once
more. Apache serves on the module it has loaded until the restart.

### Apache on Enterprise Linux

```bash
sudo truncate -s 0 /etc/default/mod-pagespeed
sudo dnf swap mod-pagespeed ~/mod-pagespeed-stable_current_x86_64.rpm
sudo cp ~/pagespeed.conf.google /etc/httpd/conf.d/pagespeed.conf
sudo rm -f /etc/httpd/conf.d/pagespeed.conf.rpmnew
sudo apachectl configtest && sudo systemctl restart httpd
```

`dnf swap` also removes the worker, which nothing needs any more, and rpm
saves the archived package's default configuration as `pagespeed.conf.rpmnew`,
which your restored copy replaces. Empty `/etc/default/mod-pagespeed` before
the swap: when the file is missing, the archived package's install script
creates it asking for its yum repository and adds that repository at once.
The repository can stay configured on either system: neither apt nor dnf installs
`mod-pagespeed` again unless you ask for it.

### nginx

```bash
sudo apt-get purge nginx-module-pagespeed      # sudo dnf remove nginx-module-pagespeed on Enterprise Linux
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so.bak /usr/lib/nginx/modules/ngx_pagespeed_module.so
# sudo cp /usr/lib64/nginx/modules/ngx_pagespeed_module.so.bak /usr/lib64/nginx/modules/ngx_pagespeed_module.so   # Enterprise Linux
cat ~/ngx_pagespeed-load_module.txt
# put your load_module line back first; the file above shows where it was
sudo nginx -t && sudo systemctl restart nginx
```

Remove the package before you restore the file: the package owns that path,
and removing it afterwards would delete the module you restored. The purge
also deletes the package's own `load_module` snippet, which holds nothing of
yours unless you edited it; the module's cache and log directories are kept. Then put back the
`load_module` line you removed during the migration, before `nginx -t`.

After a rollback Apache answers `X-Mod-Pagespeed: 1.13.35.2-0` again. The
archived 1.0 documentation stays available at [/1.0/](/1.0/), and
[Is mod_pagespeed still maintained?](/mod-pagespeed-still-maintained/) covers
what changed and what did not.
