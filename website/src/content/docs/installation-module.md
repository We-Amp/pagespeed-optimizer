---
title: 'Install the module on Apache and nginx'
description: 'Install the native mod_pagespeed 2.1 module for Apache and nginx from the signed packages.modpagespeed.com repository, or run the Docker reverse proxy.'
order: 3
group: 'Install'
lastUpdated: 2026-10-06
faq:
  - q: 'Is there a native nginx module for mod_pagespeed 2.1?'
    a: 'Yes. The native Apache and nginx module ships from the signed packages.modpagespeed.com repository — the same channel mod_pagespeed 1.15 uses.'
  - q: 'How do I run mod_pagespeed on nginx today?'
    a: 'Two options. Install the native nginx module — a dynamic module from the signed packages.modpagespeed.com repository, built for Debian and Ubuntu on amd64 and arm64 and pinned to each distribution stock nginx (1.18 to 1.26). Or put the Docker / nginx reverse proxy in front of your origin. The native nginx module runs on its own by default; to use the optimizer worker with it, install the pagespeed-optimizer package of the same release and set pagespeed DaemonSocketPath and pagespeed DaemonVolumePath in the server block.'
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
RHEL) — edit that file to turn filters on. The Apache packages also install the
configuration that points the module at the `pagespeed-optimizer` worker, so the
module reaches the worker without further configuration.

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

The native nginx module runs on its own by default. To have it use the
optimizer worker, set the two directives described in
[Using the optimizer daemon with nginx](#using-the-optimizer-daemon-with-nginx).

See [Install from packages.modpagespeed.com](/download/apt-yum/) for the full
distribution matrix (including the yum packages) and the Apache module. The
signed packages also sidestep the
[ngx_pagespeed build failures on modern nginx](/blog/ngx-pagespeed-wont-build-modern-nginx/)
that come from compiling the old module from source.

## Using the optimizer daemon with nginx

The native nginx module can hand in-place optimization to the optimizer
worker (the `pagespeed-optimizer` package, also called the optimizer daemon),
as the Apache module does. With two directives set in a `server` block, the
module records each eligible resource into the optimizer's cache, the
optimizer builds optimized variants of it, and the module serves the variant
that fits each client from that cache. With both directives unset, in-place
optimization works exactly as before.

### Before you start

- **Install the optimizer of the same release.** Install the
  `pagespeed-optimizer` package from the same repository, at the same release
  as `nginx-module-pagespeed`, and start it. The module and the optimizer are
  a matched pair: install, upgrade and roll them back together.
- **Let nginx reach the optimizer's files.** The optimizer runs as the
  unprivileged `pagespeed` user. Its cache directory, notification socket and
  shared configuration are open to members of the `pagespeed` group only. When
  `nginx-module-pagespeed` is installed or upgraded and that group exists, the
  package adds the nginx user to it (`www-data` on Debian and Ubuntu, `nginx`
  on the RHEL family). If you installed the module before the optimizer, or
  nginx runs as another user, add the user yourself, for example
  `sudo usermod -a -G pagespeed www-data`. Restart nginx afterwards; the new
  group membership takes effect only in a restarted nginx.
- **Find the two paths.** The packaged optimizer's defaults are
  `/run/pagespeed-optimizer/notify.sock` for the notification socket and
  `/var/cache/pagespeed-optimizer/v2/cache` for the cache volume. If you
  changed `SOCKET_PATH` or `CACHE_DIR` in `/etc/default/pagespeed-optimizer`,
  use those values instead: the volume path is the cache directory followed by
  `/cache`.

### Configuration

Set both directives in each `server` block that should use the optimizer:

```nginx
server {
    listen 80;
    server_name <your-domain>;

    pagespeed on;
    pagespeed FileCachePath /var/cache/ngx_pagespeed;

    # The optimizer daemon: its notification socket and its cache volume.
    pagespeed DaemonSocketPath /run/pagespeed-optimizer/notify.sock;
    pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v2/cache;
}
```

- Set the two together. With only one of them set, in-place optimization is
  off and the error log says that both are needed.
- Keep `DaemonVolumePath` on a different path from `FileCachePath`. The
  module's own cache and the optimizer's cache are separate.
- nginx connects to the optimizer when it starts. After you change either
  directive, restart nginx.

### Start order and upgrades

The packaged optimizer service is ordered ahead of `nginx.service` and waits
up to 30 seconds for its notification socket before it counts as started, so
at boot nginx starts after the optimizer. The ordering does not make nginx depend on the
optimizer: nginx still starts when the optimizer is absent. An nginx that
runs under another unit name is not ordered against the optimizer; give it an
ordering drop-in (`After=pagespeed-optimizer.service`).

When you upgrade, upgrade and start the optimizer first, let it create its
cache volume, then restart nginx. If nginx starts in between, in-place
optimization is off until you restart nginx, with the reason in the error log.
After an update that changes the cache format, a fresh start of nginx in that
window can fail instead, again with the reason in the error log. Starting the
optimizer and then restarting nginx clears both.

### When the optimizer is not available

If the directives are set but the optimizer cannot be used — it is not
running, it is a release this module cannot work with, or nginx is not allowed
to reach its files — then for that server:

- in-place optimization is off, and nginx serves every request normally;
- nginx logs the reason. When nginx is not allowed to reach the optimizer's
  files, the log does not name the `pagespeed` group: nginx repeats a warning
  that the optimizer's cache volume "cannot be used yet" because the daemon
  "does not publish the size of its cache volume", once per attempt, and
  nothing is optimized; after about six minutes it stops retrying and logs one
  error. If the optimizer is running, add the nginx user to the `pagespeed`
  group (for example `sudo usermod -a -G pagespeed www-data`) and restart
  nginx;
- when nginx refuses the optimizer at startup, the same reason appears once in
  the admin console's [message history](/docs/admin-console/#message-history).

The module's own in-place path does not take over in that case. Fix the cause
and restart nginx.

### Check that it works

1. Run `sudo nginx -t`, restart nginx, and read the nginx error log. A working
   pair logs no repeated warning about the optimizer; the one error line you
   may see is a note about left-over cache volume files after you change the
   optimizer's cache size.
2. Request a stylesheet or script of your site a few times.
3. Open the module's statistics (the admin console's Statistics page, or your
   `StatisticsPath`). `ipro_daemon_served` counts in-place requests answered
   from the optimizer's cache, and `ipro_daemon_fallthrough` counts the ones
   passed to the ordinary path. Once the optimizer has built a variant,
   `ipro_daemon_served` rises. If only `ipro_daemon_fallthrough` rises, see
   [Limits](#limits).

The admin console's [optimizer daemon panels](/docs/admin-console/#optimizer-daemon-panels)
read the optimizer's management API through the separate
`DaemonApiSocketPath` directive.

### Limits

- A response that carries headers this path cannot reproduce from the cache —
  a `Set-Cookie`, a CORS or security header, a `Vary` on another axis — is
  served by the ordinary path: complete, but not optimized in place. This
  includes headers your own nginx configuration adds to every response with
  `add_header` (an HSTS header, for example): the module cannot tell them
  apart from headers the origin sent. On a server that sets such a header
  site-wide, little or nothing is optimized in place and `ipro_daemon_served`
  does not rise.
- A request that nginx redirects internally (`rewrite`, a `try_files`
  fallback, `index`, `error_page`) is not optimized in place.
- By default the module serves the uncompressed optimized copy and nginx
  compresses it as configured. `pagespeed DaemonServeStoredEncodings on;`
  sends the optimizer's stored gzip and brotli copies instead; it is off by
  default.

## Docker / nginx reverse proxy

The [Docker / nginx reverse proxy](/docs/installation-docker/) runs the same
optimization core in front of any HTTP origin. It uses a dynamic nginx
module (`ngx_pagespeed_module.so`) paired with the optimizer worker:
[image transcoding](/blog/nginx-image-optimization-module/),
CSS/JS minification, critical CSS, and zero-copy serving from the Cyclone
shared-memory cache.

Run your site through a [PageSpeed Insights test](/analyze/) to see which
failing audits mod_pagespeed will fix, and read the
[Core Web Vitals](/core-web-vitals/) guide for the LCP, CLS, and INP plan.
