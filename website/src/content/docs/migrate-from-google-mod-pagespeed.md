---
title: 'Migrate from the archived Google module'
description: 'Move from the archived open-source mod_pagespeed or ngx_pagespeed binaries to mod_pagespeed 2.1 in three commands: what maps to what, cache, rollback.'
order: 15
group: 'Upgrade and migrate'
lastUpdated: 2026-10-08
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
sudo apt-get install mod-pagespeed             # Apache (dnf on Enterprise Linux)
sudo apt-get install nginx-module-pagespeed    # nginx
# 3. Restart the web server
sudo systemctl restart apache2                 # httpd on Enterprise Linux, or nginx
```

Before you run them, record the version you have and keep the old module file
for a rollback:

```bash
curl -I http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
sudo cp /usr/lib/apache2/modules/mod_pagespeed.so{,.bak}         # Apache
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so{,.bak}    # nginx, dynamic module
```

On Apache, remove the archived `mod-pagespeed-stable` or `mod-pagespeed-beta`
package first if you installed one: both want the same module file. On nginx,
make sure one `load_module` line names
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

```bash
# nginx
sudo systemctl stop nginx
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so.bak /usr/lib/nginx/modules/ngx_pagespeed_module.so
sudo systemctl start nginx
# Apache
sudo systemctl stop apache2
sudo cp /usr/lib/apache2/modules/mod_pagespeed.so.bak /usr/lib/apache2/modules/mod_pagespeed.so
sudo systemctl start apache2
```

Remove the new package first, so a later upgrade does not reinstall the module
over your restored file. The archived 1.0 documentation stays available at
[/1.0/](/1.0/), and
[Is mod_pagespeed still maintained?](/mod-pagespeed-still-maintained/) covers
what changed and what did not.
