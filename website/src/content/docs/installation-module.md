---
title: 'ModPageSpeed for nginx'
description: 'Install the native mod_pagespeed 2.1 module for Apache and nginx from the signed packages.modpagespeed.com repository, or run the Docker / nginx reverse proxy.'
order: 11
group: 'Install'
lastUpdated: 2026-09-18
faq:
  - q: 'Is there a native nginx module for mod_pagespeed 2.1?'
    a: 'Yes. The native Apache and nginx module ships from the signed packages.modpagespeed.com repository — the same channel mod_pagespeed 1.15 uses.'
  - q: 'How do I run mod_pagespeed on nginx today?'
    a: 'Two options. Install the native nginx module — a dynamic module from the signed packages.modpagespeed.com repository, built for Debian and Ubuntu on amd64 and arm64 and pinned to each distribution stock nginx (1.18 to 1.26). Or put the Docker / nginx reverse proxy in front of your origin.'
---

mod_pagespeed ships a native module for Apache and nginx. You have three
working options:

- **Native Apache module** — install from the signed
  [packages.modpagespeed.com repository](/download/apt-yum/), or the
  [cPanel / EasyApache 4 guide](/docs/cpanel/) on cPanel hosts.
- **Native nginx module** — install from the signed
  [packages.modpagespeed.com repository](/download/apt-yum/).
- **Docker / nginx reverse proxy** — run the
  [Docker / nginx reverse proxy](/docs/installation-docker/) in front of your
  origin.

## Native Apache module

The signed repository at `packages.modpagespeed.com` ships the Apache module
(`mod-pagespeed`) — the same channel mod_pagespeed 1.15 uses — for Apache
2.4+:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install mod-pagespeed   # Debian/Ubuntu
sudo dnf install mod-pagespeed       # AlmaLinux/RHEL/Rocky
sudo systemctl restart apache2       # Debian/Ubuntu
sudo systemctl restart httpd         # RHEL family
```

The module keeps the same directive surface and filter names, so an existing
mod_pagespeed configuration carries over unchanged — see the
[configuration reference](/docs/configuration/) for the full directive
set. The package also drops a default `pagespeed.conf` (in
`/etc/apache2/mods-available/` on Debian/Ubuntu, `/etc/httpd/conf.d/` on
RHEL) — edit that file to turn filters on.

Running Apache under cPanel/WHM? Use the signed EasyApache 4 RPM instead —
see the [cPanel / EasyApache 4 guide](/docs/cpanel/). See
[Install from packages.modpagespeed.com](/download/apt-yum/) for the full
distribution matrix (including the yum packages) and the nginx module.

## Native nginx module

The signed repository at `packages.modpagespeed.com` ships the nginx module
(`nginx-module-pagespeed`) — the same channel mod_pagespeed 1.15 uses — for
Debian and Ubuntu on amd64 and arm64. Each build is pinned to that
distribution's stock nginx (1.18 through 1.26), so there is no version to
match by hand:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install nginx-module-pagespeed
```

See [Install from packages.modpagespeed.com](/download/apt-yum/) for the full
distribution matrix (including the yum packages) and the Apache module. The
signed packages also sidestep the
[ngx_pagespeed build failures on modern nginx](/blog/ngx-pagespeed-wont-build-modern-nginx/)
that come from compiling the old module from source.

## Docker / nginx reverse proxy

The [Docker / nginx reverse proxy](/docs/installation-docker/) runs the same
optimization core in front of any HTTP origin. It uses a dynamic nginx
module (`ngx_pagespeed_module.so`) paired with a separate worker process, and
runs the same pipeline the native module does:
[image transcoding](/blog/nginx-image-optimization-module/),
CSS/JS minification, critical CSS, and zero-copy serving from the Cyclone
shared-memory cache.

Run your site through a [PageSpeed Insights test](/analyze/) to see which
failing audits ModPageSpeed will fix, and read the
[Core Web Vitals](/core-web-vitals/) guide for the LCP, CLS, and INP plan.
