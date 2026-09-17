---
title: 'ModPageSpeed for nginx'
description: 'ModPageSpeed 2.0 has no standalone nginx module yet. For a native nginx module today use mod_pagespeed 1.15; for 2.0 use the Docker / nginx reverse proxy.'
order: 11
group: 'Install'
lastUpdated: 2026-07-04
faq:
  - q: 'Is there a standalone nginx module for ModPageSpeed 2.0?'
    a: 'Not in the current 2.0 release. The 2.0 line ships the Docker / nginx reverse proxy. A standalone nginx 2.0 module is on the 2.x roadmap. For a native nginx module today, use mod_pagespeed 1.15.'
  - q: 'How do I run ModPageSpeed on nginx today?'
    a: 'Two options. Use the mod_pagespeed 1.15 nginx module — a native dynamic module from the signed packages.modpagespeed.com repository, built for Debian and Ubuntu on amd64 and arm64 and pinned to each distribution stock nginx (1.18 to 1.26). Or put the ModPageSpeed 2.0 Docker / nginx reverse proxy in front of your origin.'
---

ModPageSpeed 2.0 does not yet ship a standalone nginx module. You have two
working options for nginx:

- **Native nginx module today** — use [mod_pagespeed 1.15](/1.1/).
- **ModPageSpeed 2.0 today** — use the [Docker / nginx reverse proxy](/docs/installation-docker/).

A standalone nginx 2.0 module is on the 2.x roadmap. This page covers
both options until it ships.

## Native nginx module: mod_pagespeed 1.15

[mod_pagespeed 1.15](/1.1/) is the native server-module line for Apache, nginx,
IIS, and Envoy. The signed repository at `packages.modpagespeed.com` ships the
nginx module (`nginx-module-pagespeed`) for Debian and Ubuntu on amd64 and
arm64. Each build is pinned to that distribution's stock nginx (1.18 through
1.26), so there is no version to match by hand:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install nginx-module-pagespeed
```

See the [1.15 nginx guide](/1.1/) for configuration. The signed packages
also sidestep the [ngx_pagespeed build failures on modern nginx](/blog/ngx-pagespeed-wont-build-modern-nginx/)
that come from compiling the old module from source.

## ModPageSpeed 2.0: Docker / nginx reverse proxy

The 2.0 [Docker / nginx reverse proxy](/docs/installation-docker/) runs the new
C++ optimization core in front of any HTTP origin. It uses a dynamic nginx
module (`ngx_pagespeed_module.so`) paired with a separate worker process, and
runs the same pipeline the future standalone module will:
[image transcoding](/blog/nginx-image-optimization-module/),
CSS/JS minification, critical CSS, and zero-copy serving from the Cyclone
shared-memory cache.

Run your site through a [PageSpeed Insights test](/analyze/) to see which
failing audits ModPageSpeed will fix, and read the
[Core Web Vitals](/core-web-vitals/) guide for the LCP, CLS, and INP plan.
