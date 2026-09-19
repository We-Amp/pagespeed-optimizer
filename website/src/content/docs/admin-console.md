---
title: 'mod_pagespeed Admin Console'
description: 'mod_pagespeed 2.1 admin console: statistics, cache inspection, message history, and config. Setup and access control for nginx, Apache, and IIS at /pagespeed_admin/.'
order: 37
group: 'Operate'
lastUpdated: 2026-09-06
---

## Overview

mod_pagespeed 2.1 includes built-in admin pages for monitoring, configuration inspection, and cache management. Access them at `/pagespeed_admin/` on your server.

Two endpoints are exposed: `/pagespeed_admin` is the per-vhost admin handler, and `/pagespeed_global_admin` is the process-wide handler. Both are read-write (cache purge) and warrant the same access restrictions in production.

<img
  src="/images/console-1.1-admin.png"
  alt="mod_pagespeed 1.15 admin console showing live statistics with per-variable deltas and trend sparklines"
  width="1440"
  height="900"
  loading="lazy"
  class="rounded-lg border border-border"
/>

**See it live:** explore the [mod_pagespeed 1.15 admin console](https://demo-httpd-1.1.modpagespeed.com/pagespeed_global_admin/#/console) running on our public Apache demo — live statistics, caches, histograms, and message history, no install required.

## Admin pages

| Page          | URL                                | Description                                    |
| ------------- | ---------------------------------- | ---------------------------------------------- |
| Statistics    | `/pagespeed_admin/statistics`      | Filter activity, cache hit rates, latency      |
| Configuration | `/pagespeed_admin/config`          | Active filters and directive values            |
| Histograms    | `/pagespeed_admin/histograms`      | Page load time, rewrite latency distributions  |
| Caches        | `/pagespeed_admin/cache`           | Cache status, inspection, and purge            |
| Console       | `/pagespeed_admin/console`         | Historical graphs (requires StatisticsLogging) |
| Messages      | `/pagespeed_admin/message_history` | Recent log messages                            |

## Setup

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

Add handler paths and corresponding location blocks:

```nginx
pagespeed StatisticsPath /ngx_pagespeed_statistics;
pagespeed GlobalStatisticsPath /ngx_pagespeed_global_statistics;
pagespeed MessagesPath /ngx_pagespeed_message;
pagespeed ConsolePath /pagespeed_console;
pagespeed AdminPath /pagespeed_admin;
pagespeed GlobalAdminPath /pagespeed_global_admin;
```

Location blocks for the admin paths must appear before the `.pagespeed.` resource regex in your config.

Restrict access at the location layer. The `=` (exact) and `^~` (prefix) matchers run against the request path _after_ nginx normalizes it, which defuses common path-rewriting bypasses (see [URL-path ACLs are brittle](#url-path-acls-are-brittle) below):

```nginx
# Restrict mod_pagespeed admin endpoints to localhost.
# Widen explicitly (e.g. add `allow <admin-CIDR>;`) for ops access.
# Use `location =` exact-match: nginx normalizes the request path before
# this match, defeating common path-bypass tricks (//, /./, %2e, case).
location = /pagespeed_admin {
    allow 127.0.0.1;
    allow ::1;
    deny all;
}
location ^~ /pagespeed_admin/ {
    allow 127.0.0.1;
    allow ::1;
    deny all;
}
# Repeat for /pagespeed_global_admin (process-wide — at least as sensitive).
location = /pagespeed_global_admin { allow 127.0.0.1; allow ::1; deny all; }
location ^~ /pagespeed_global_admin/ { allow 127.0.0.1; allow ::1; deny all; }
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
<Location /pagespeed_admin>
    Order allow,deny
    Allow from localhost
    Allow from 127.0.0.1
    SetHandler pagespeed_admin
</Location>

<Location /pagespeed_global_admin>
    Order allow,deny
    Allow from localhost
    Allow from 127.0.0.1
    SetHandler pagespeed_global_admin
</Location>
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

Add admin path directives to your `pagespeed.config`:

```
pagespeed AdminPath /pagespeed_admin
pagespeed GlobalAdminPath /pagespeed_global_admin
pagespeed StatisticsPath /pagespeed_statistics
pagespeed GlobalStatisticsPath /pagespeed_global_statistics
pagespeed MessagesPath /pagespeed_message
pagespeed ConsolePath /pagespeed_console
```

By default, admin pages are accessible only from `localhost`. To allow access from other hosts:

```
pagespeed InfoUrlsLocalOnly off
```

Use this setting with caution in production — restrict access at the network level (firewall rules) when exposing admin pages to non-local clients.

### Windows Event Viewer

The IIS module logs warnings and errors to the Windows Event Viewer automatically. Open Event Viewer and look under **Windows Logs > Application** for entries from the mod_pagespeed source.

Event Viewer captures module startup/shutdown messages, configuration errors, and runtime warnings without any additional configuration.

</div>

## Access control

Restrict admin page access by domain using domain-level ACLs:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed AdminDomains Allow localhost;
pagespeed AdminDomains Allow 10.0.0.*;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedAdminDomains Allow localhost
ModPagespeedAdminDomains Allow 10.0.0.*
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed AdminDomains Allow localhost
pagespeed AdminDomains Allow 10.0.0.*
```

On IIS, the `InfoUrlsLocalOnly` directive provides an additional layer of access control. When set to `on` (the default), admin URLs are only accessible from the local machine regardless of the `AdminDomains` setting.

</div>

Available ACL directives: `StatisticsDomains`, `GlobalStatisticsDomains`, `MessagesDomains`, `ConsoleDomains`, `AdminDomains`, `GlobalAdminDomains`.

Default is `Allow *`. Once any `Allow` is specified, all other domains are implicitly denied. Wildcards are supported.

## Statistics

Statistics are enabled by default and required for features like image rewrite concurrency limiting and background fetch rate limiting. Do not disable them.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed Statistics on;
pagespeed UsePerVhostStatistics on;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedStatistics on
ModPagespeedUsePerVhostStatistics on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed Statistics on
pagespeed UsePerVhostStatistics on
```

Per-vhost statistics are enabled by default on IIS. Each website gets its own set of counters, accessible at `/pagespeed_admin/statistics` on that site; site-wide aggregates remain at `/pagespeed_global_admin/statistics`.

</div>

`UsePerVhostStatistics` enables per-virtual-host stats while still exposing aggregates at the global admin path.

## Cache telemetry (v1.15.0+r18)

v1.15.0+r18 adds cache observability counters to the caches page (`/pagespeed_admin/cache`), reported per Cyclone cache:

- **Current entries / Current size bytes** — how full the cache is.
- **RAM cache bytes / RAM cache hits / RAM cache misses** — activity of the optional RAM tier (`CycloneRamCacheKb`).
- **Disk cache hits / Disk cache misses** — hit rate of the memory-mapped volume.
- **Evictions / Tag collision evictions** — entries displaced by capacity pressure or key collisions.
- **Write buffer wraps / Wraps deferred by lease / Writes dropped by lease / Wraps forced past lease** — write-buffer turnover and how zero-copy serving interacts with it; sustained deferred or dropped counts indicate a cache that is too small for its write rate.

The statistics page also gains counters for zero-copy serving (all 0 while the feature is off): `zerocopy_serve_aliased`, `zerocopy_serve_copied_out`, `zerocopy_serve_renew_fail_reset`, `zerocopy_serve_aborted`, and `zerocopy_serve_ring_refills`. A persistently nonzero `zerocopy_serve_renew_fail_reset` rate is the signal that a cache stripe is hot enough to warrant tuning; see [Caching](/docs/cache-modes/#zero-copy-serving) for the zero-copy options.

## Console (historical graphs)

The console requires `StatisticsLogging` to record data over time:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed StatisticsLogging on;
pagespeed LogDir /var/log/pagespeed;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedStatisticsLogging on
ModPagespeedLogDir /var/log/pagespeed
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed StatisticsLogging on
pagespeed LogDir %ProgramData%\We-Amp\PageSpeed\Logs
```

The log directory must be writable by the IIS app pool identity.

</div>

## Message history

Set the message buffer size to enable recent log message viewing:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MessageBufferSize 100000;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMessageBufferSize 100000
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MessageBufferSize 100000
```

Messages are collected globally from all IIS worker processes and accessible at `/pagespeed_global_admin/message_history`.

</div>

Default is 0 (disabled).

## URL-path ACLs are brittle

A WAF or upstream filter that blocks the admin endpoint by string-matching the literal URL is easy to bypass. Common evasion techniques target path normalization differences between the filter and the origin web server:

- Duplicate slashes (`//pagespeed_admin/`)
- Path segments (`/./pagespeed_admin/`, `/foo/../pagespeed_admin/`)
- Percent-encoded characters (`/%70agespeed_admin/`, `/pagespeed_admin/%2e./statistics`)
- Mixed case (`/PageSpeed_Admin/`)
- Trailing slash present/absent
- Path parameters (`/pagespeed_admin;param=x/statistics`)

Gate access at the web server's normalized location/handler layer — `location =` / `^~` on nginx, `<Location>` on Apache, the handler-bound check on IIS — not at a separate filter that operates on the raw URL string. The web server runs its match against the same normalized path the handler will see.

## Important notes

- `pagespeed_admin` and `pagespeed_global_admin` are read-write (can purge cache). Other handlers are read-only.
- Always restrict admin page access in production.

## See also

- [Caching](/docs/cache-modes/#purge-via-admin-page) — cache purging via the admin interface
- [Configuration](/docs/configuration/) — general configuration reference
