---
title: 'Long-standing upstream issues resolved'
description: 'Open issues in the archived mod_pagespeed and ngx_pagespeed trackers that current mod_pagespeed resolves, with each fix and the release that shipped it.'
order: 16
group: 'Upgrade and migrate'
lastUpdated: 2026-10-30
datePublished: 2026-10-30
---

The Apache Incubator trackers for
[mod_pagespeed](https://github.com/apache/incubator-pagespeed-mod/issues) and
[ngx_pagespeed](https://github.com/apache/incubator-pagespeed-ngx/issues) are
archived and read-only. Nobody can close an issue there any more, so reports
that current mod_pagespeed has resolved still show as open. This page lists
them by their original URL, with the change that resolved each one and the
release that shipped it.

Each row links to its source: an entry in the module's
[CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md),
an entry in these docs' release notes, or the source file that carries the
change. Rows marked **likely** fix the cause the report describes, but nobody
has checked them against the reporter's exact setup. Issues we could not tie to
a specific change are left out, even where they probably no longer occur.

Release numbers are module releases: 1.1.0 (May 2026, the first
general-availability release), the 1.15 line (1.15.0 to 1.15.0+r22), and 2.1.0
onward. See [the release notes](/docs/release-notes/) for the full
history.

If you still hit one of these on a current release, open an issue in
[We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed/issues) with your
version, server and a reproduction.

## Build, packaging and platforms

| Upstream issue | What changed | Shipped in |
| --- | --- | --- |
| mod [#2094](https://github.com/apache/incubator-pagespeed-mod/issues/2094), [#2043](https://github.com/apache/incubator-pagespeed-mod/issues/2043): the Google apt repository fails with GPG and "no longer signed" errors | Signed deb and rpm packages from a maintained repository, `packages.modpagespeed.com`. Since 2.5.0, `apt-get install mod-pagespeed` replaces the archived Google package in one step, and `dnf swap mod-pagespeed-stable mod-pagespeed` does so on Enterprise Linux. See [Migrate from the archived Google module](/docs/migrate-from-google-mod-pagespeed/). | 1.1.0 ([release notes](/docs/release-notes-1-1/#v1-1-0)); one-step move in 2.5.0 ([CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md#250---2026-10-19), "Moving from Google's archived package works in one step") |
| ngx [#1615](https://github.com/apache/incubator-pagespeed-ngx/issues/1615), [#1735](https://github.com/apache/incubator-pagespeed-ngx/issues/1735), [#1743](https://github.com/apache/incubator-pagespeed-ngx/issues/1743), [#1758](https://github.com/apache/incubator-pagespeed-ngx/issues/1758), [#1762](https://github.com/apache/incubator-pagespeed-ngx/issues/1762); mod [#2083](https://github.com/apache/incubator-pagespeed-mod/issues/2083): the module does not build against newer nginx or glibc | A prebuilt, signed nginx module, built for each supported distribution against that distribution's nginx and C library, replaces compiling it yourself. See [Install on Apache and nginx](/docs/installation-module/). | 1.1.0 ([release notes](/docs/release-notes-1-1/#v1-1-0), "Prebuilt, signed packages") |
| ngx [#1121](https://github.com/apache/incubator-pagespeed-ngx/issues/1121), [#1135](https://github.com/apache/incubator-pagespeed-ngx/issues/1135), [#1250](https://github.com/apache/incubator-pagespeed-ngx/issues/1250), [#1297](https://github.com/apache/incubator-pagespeed-ngx/issues/1297), [#1740](https://github.com/apache/incubator-pagespeed-ngx/issues/1740): building ngx_pagespeed as a dynamic module fails | The nginx module ships as a dynamic module, and the package loads it for you. | 1.1.0 ([release notes](/docs/release-notes-1-1/#v1-1-0), "a prebuilt dynamic module for stock nginx") |
| ngx [#637](https://github.com/apache/incubator-pagespeed-ngx/issues/637), [#661](https://github.com/apache/incubator-pagespeed-ngx/issues/661), [#704](https://github.com/apache/incubator-pagespeed-ngx/issues/704): no packages for a distribution's own nginx, no yum repository, no installer or updater | Signed apt and yum repositories for the nginx module, with a build for each distribution's stock nginx. Updates arrive through the package manager. | 1.1.0+r18 ([release notes](/docs/release-notes-1-1/#v1-1-0-r18-r21), "Packaging") |
| ngx [#1213](https://github.com/apache/incubator-pagespeed-ngx/issues/1213), [#1745](https://github.com/apache/incubator-pagespeed-ngx/issues/1745), [#1763](https://github.com/apache/incubator-pagespeed-ngx/issues/1763): no ARM64 (aarch64) build | arm64/aarch64 builds for Apache and nginx on Linux. The aarch64 nginx module rpm for Enterprise Linux 9 followed in 1.15.0+r11. | 1.1.0 ([release notes](/docs/release-notes-1-1/#v1-1-0), "arm64/aarch64 support"); EL9 rpm in 1.15.0+r11 ([release notes](/docs/release-notes-1-15/#v1-15-0-r11-r14)) |
| mod [#1753](https://github.com/apache/incubator-pagespeed-mod/issues/1753), [#1844](https://github.com/apache/incubator-pagespeed-mod/issues/1844): the bundled libwebp is out of date | The bundled image libraries were updated, libwebp to 1.5.0. | 1.1.0 ([release notes](/docs/release-notes-1-1/#v1-1-0), "Security") |

## Caching and headers

| Upstream issue | What changed | Shipped in |
| --- | --- | --- |
| mod [#1332](https://github.com/apache/incubator-pagespeed-mod/issues/1332), [#1503](https://github.com/apache/incubator-pagespeed-mod/issues/1503), [#1648](https://github.com/apache/incubator-pagespeed-mod/issues/1648), [#1936](https://github.com/apache/incubator-pagespeed-mod/issues/1936); ngx [#1516](https://github.com/apache/incubator-pagespeed-ngx/issues/1516): the file cache cleaner loads the disk, the cache grows past its limit, file names get too long, and the log fills with lock-file and cache-clean errors | The file-per-entry disk cache is replaced by Cyclone Cache, a fixed-size, memory-mapped cache with no periodic cleanup passes. `FileCachePath` keeps working and now locates the Cyclone cache. | 1.1.0 ([release notes](/docs/release-notes-1-1/#v1-1-0), "Cyclone Cache") |
| mod [#1855](https://github.com/apache/incubator-pagespeed-mod/issues/1855) (**likely**): the module fails under SELinux in enforcing mode | The error in the report ("Failed to mkdir … outputlock") came from the old cache's lock files, which no longer exist. | 1.1.0 ([release notes](/docs/release-notes-1-1/#v1-1-0), "Cyclone Cache") |
| mod [#1287](https://github.com/apache/incubator-pagespeed-mod/issues/1287): respect `s-maxage` (the `s-maxage` half of the report) | `s-maxage` is honored for shared-cache freshness. It can only shorten the lifetime set by `max-age` or `Expires`, and `s-maxage=0` keeps a response out of the cache. `Age` is not covered. | 2.5.0 ([CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md#250---2026-10-19), Changed) |
| ngx [#1271](https://github.com/apache/incubator-pagespeed-ngx/issues/1271): `Vary: Accept-Encoding` repeated on optimized resources | The nginx module's filters run at their intended place in nginx's filter chain, so responses carry exactly one `Vary: Accept-Encoding`. | 2.2.0 ([CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md#220---2026-10-06), "nginx: the module's filters run at their intended place in nginx's filter chain") |
| ngx [#1589](https://github.com/apache/incubator-pagespeed-ngx/issues/1589): `Cache-Control: immutable` | Optimized `.pagespeed.` resources declare `Cache-Control: public, immutable` next to their one-year `max-age`. | 1.15.0+r22 ([CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md#1150r22---2026-08-08), "Optimized `.pagespeed.` resources now declare `Cache-Control: public, immutable`") |
| mod [#1704](https://github.com/apache/incubator-pagespeed-mod/issues/1704): a TTL for Redis keys | `RedisTTLSec` sets an expiry on the keys the module writes to Redis. This was fixed in the upstream code in 2019 ([commit `7659ff7`](https://github.com/We-Amp/mod_pagespeed/commit/7659ff743638781f847e37500b8a7093fb49a2f8), "Fixes #1704"), but the issue was never closed. | Upstream code, 2019; in every release of the maintained module ([`system_rewrite_options.cc`](https://github.com/We-Amp/mod_pagespeed/blob/v2.6.0/pagespeed/system/system_rewrite_options.cc)) |

## Filters

| Upstream issue | What changed | Shipped in |
| --- | --- | --- |
| mod [#2051](https://github.com/apache/incubator-pagespeed-mod/issues/2051), [#1505](https://github.com/apache/incubator-pagespeed-mod/issues/1505); ngx [#1663](https://github.com/apache/incubator-pagespeed-ngx/issues/1663): use the browser's native lazy loading instead of a placeholder image and a script | `lazyload_images` has a native mode that writes `loading="lazy"` on below-the-fold images, so the real image URL stays in `src`. `LazyloadImagesMode` chooses it (`auto`, the default, uses it where the browser supports native lazy loading). | 1.15.0+r18 ([release notes](/docs/release-notes-1-15/#v1-15-0-r18), "`lazyload_images` gains a native mode") |
| mod [#1773](https://github.com/apache/incubator-pagespeed-mod/issues/1773), [#1514](https://github.com/apache/incubator-pagespeed-mod/issues/1514): `inline_javascript` inlines scripts that carry `defer` | `inline_javascript` no longer inlines external scripts that carry `async` or `defer`, which would turn a deferred script into a blocking one and leave an attribute that is invalid on an inline script. | 1.15.0+r20 ([release notes](/docs/release-notes-1-15/#v1-15-0-r20), "`inline_javascript` no longer inlines external scripts carrying `async` or `defer`") |
| mod [#1992](https://github.com/apache/incubator-pagespeed-mod/issues/1992): "Unrecognized script" messages for `application/ld+json` blocks | Data-only script blocks (JSON-LD, JSON data, import maps, speculation rules, templates) no longer log the "Unrecognized script" message ([`javascript_filter.cc`](https://github.com/We-Amp/mod_pagespeed/blob/v2.6.0/net/instaweb/rewriter/javascript_filter.cc)). | 1.15.0+r21 ([release notes](/docs/release-notes-1-15/#v1-15-0-r21), "Data-only `<script>` blocks … no longer emit a spurious 'Unrecognized script' info message") |
| ngx [#1537](https://github.com/apache/incubator-pagespeed-ngx/issues/1537): `add_instrumentation` uses the deprecated `chrome.loadTimes()` | The instrumentation beacon measures with the standard performance APIs and reports Core Web Vitals ([`add_instrumentation.js`](https://github.com/We-Amp/mod_pagespeed/blob/v2.6.0/net/instaweb/rewriter/add_instrumentation.js)). | 1.15.0+r20 ([release notes](/docs/release-notes-1-15/#v1-15-0-r20), "Core Web Vitals reporting from the instrumentation beacon") |

## Reload, robustness and configuration

| Upstream issue | What changed | Shipped in |
| --- | --- | --- |
| mod [#1775](https://github.com/apache/incubator-pagespeed-mod/issues/1775): deadlock in shared-memory statistics | The mutexes the Apache and nginx modules keep in shared memory are robust. When a process dies while holding one, the next process takes it over instead of every process blocking until a restart (Linux and FreeBSD). | 2.2.0 ([CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md#220---2026-10-06), Fixed) |
| mod [#1563](https://github.com/apache/incubator-pagespeed-mod/issues/1563): `<Files>` sections in `.htaccess` deny `.pagespeed.` URLs | Apache `<Files>` and `<FilesMatch>` sections apply to `.pagespeed.` URLs, matched against the real resource name. | 2.6.0 ([CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md#260---2026-10-28), "Apache: `<Files>` and `<FilesMatch>` sections now apply to `.pagespeed.` URLs") |
| ngx [#387](https://github.com/apache/incubator-pagespeed-ngx/issues/387) (**likely**): `pagespeed on`/`off` in nested `location` blocks | Nested locations and `if` blocks inherit the enclosing location's `pagespeed` settings, and `pagespeed` directives set in the nested block itself apply there, as for any other nginx module. | 2.6.0 ([CHANGELOG](https://github.com/We-Amp/mod_pagespeed/blob/master/CHANGELOG.md#260---2026-10-28), Changed) |
| ngx [#1380](https://github.com/apache/incubator-pagespeed-ngx/issues/1380): an nginx worker spins at 100% CPU when a write returns `EAGAIN` | When the module's internal event pipe is full, events wait in a queue and are written once the pipe drains, instead of the writer retrying in a loop ([`ngx_event_connection.cc`](https://github.com/We-Amp/mod_pagespeed/blob/v2.6.0/pagespeed/nginx/ngx_event_connection.cc)). | In the code since July 2026, and in every release from 2.1.0 on ([source at 2.1.0](https://github.com/We-Amp/mod_pagespeed/blob/v2.1.0/pagespeed/nginx/ngx_event_connection.cc)) |
