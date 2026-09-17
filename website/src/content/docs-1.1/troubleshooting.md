---
title: 'Troubleshooting'
description: 'Fix common mod_pagespeed 1.15 problems: a missing X-Mod-Pagespeed header, 404s on optimized resources, broken layout or JavaScript, cache and memory issues, and how to read .pagespeed. URLs and debug markers.'
order: 32
group: 'Operations'
lastUpdated: 2026-07-27
---

## No X-Mod-Pagespeed or X-Page-Speed header

**Cause:** mod_pagespeed is not running or not intercepting the response.

**Fix:**

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

- Verify the module is loaded: check `nginx -V` for the module.
- Verify `pagespeed on;` is set in your server block.
- Check that the response Content-Type is `text/html`. mod_pagespeed only rewrites HTML responses.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

- Verify the module is loaded: check `apachectl -M` for `pagespeed_module`.
- Verify `ModPagespeed on` is set.
- Verify `AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html` is present (usually set automatically).
- Check that the response Content-Type is `text/html`. mod_pagespeed only rewrites HTML responses.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

- Open IIS Manager and verify the module appears under **Modules** for your site.
- Verify `pagespeed on` is set in your `pagespeed.config`.
- Check that the response Content-Type is `text/html`. mod_pagespeed only rewrites HTML responses.
- Check the Windows Event Viewer (**Windows Logs > Application**) for startup errors from the mod_pagespeed source.
- Verify that the IIS worker process (app pool identity) can read the `pagespeed.config` file.

</div>

## Pages are never served optimized / cache never warms

**Cause:** Resources are being optimized but the cache is not warming.

**Fix:**

- Optimization is progressive: the first request to a page triggers background optimization. Wait for the rewrite to complete and retry.
- If repeated requests still serve the original resources, check cache directory permissions and disk space.
- Verify the cache path exists and is writable by the web server process.

## Optimized resources return 404

**Cause:** The `.pagespeed.` URL pattern is not being routed to mod_pagespeed.

**Fix:**

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

Ensure the `.pagespeed.` location block is present and appears before other location blocks that might match.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

If using `mod_rewrite`, add `RewriteCond %{REQUEST_URI} !\.pagespeed\.` to prevent rewrite rules from intercepting pagespeed URLs.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

Check that URL Rewrite rules (if installed) do not intercept `.pagespeed.` URLs. Add a stop-processing rule before other rewrite rules:

```xml
<rule name="PageSpeed" stopProcessing="true">
    <match url="\.pagespeed\." />
    <action type="None" />
</rule>
```

Also verify that the IIS app pool has not been recycled mid-optimization. The module reloads its cache on app pool restart, but in-flight rewrites are lost.

</div>

Verify that the configuration options match between the HTML-serving VHost and the resource-serving VHost.

## mod_pagespeed broke my page layout or JavaScript

**Cause:** A filter is incompatible with your site's CSS or JavaScript.

**Fix:**

1. Identify which filter causes the issue by adding [`?PageSpeed=off`](/pagespeed-markers/#pagespeed-off) to the URL to disable all optimization.
2. If the page works with `?PageSpeed=off`, narrow down the filter by disabling them one at a time.
3. Common culprits:
   - [`defer_javascript`](/1.1/docs/javascript-filters/#defer_javascript) — breaks scripts using `document.write` or expecting to run during parse.
   - [`combine_javascript`](/1.1/docs/javascript-filters/#combine_javascript) — can break order-dependent scripts.
   - [`prioritize_critical_css`](/1.1/docs/css-filters/#prioritize_critical_css) — can cause flash of unstyled content if critical CSS detection is incomplete.
4. Disable the offending filter with `DisableFilters`.

## mod_pagespeed is not picking up file changes

**Cause:** Resources are cached with their original TTL or mod_pagespeed's implicit cache TTL.

**Fix:**

- Flush the cache: `curl 'http://yoursite.com/pagespeed_admin/cache?purge=*'`
- Or touch the cache.flush file (legacy method).
- If resources are loaded from disk via `LoadFromFile`, changes are picked up after `LoadFromFileCacheTtlMs` expires (default: same as ImplicitCacheTtlMs, 5 minutes).
- For immediate pickup, set a shorter `ImplicitCacheTtlMs` or purge the specific URL.

## High memory usage

**Cause:** mod_pagespeed caches metadata and optimized resources in memory.

**Fix:**

- Check `CycloneRamCacheKb`. From v1.15.0+r18 the RAM tier is off by default (`0`); a positive value or `-1` (the pre-r18 default, which sizes it from `LRUCacheKbPerProcess`) adds per-process RAM on top of the shared memory-mapped cache.
- Reduce `DefaultSharedMemoryCacheKB` (default: 51200).
- If using IPRO on a site with many unique uncacheable URLs, the metadata cache can grow. Consider disabling IPRO for those URL patterns.

## SELinux blocks mod_pagespeed

**Cause:** SELinux policy prevents the web server from writing to the cache directory.

**Fix:**

```bash
sudo chcon -R -t httpd_sys_content_t /var/cache/pagespeed/
```

## Beacons causing unexpected POST requests

**Cause:** Filters like [`lazyload_images`](/1.1/docs/image-filters/#lazyload_images), `defer_javascript`, and `prioritize_critical_css` use a [JavaScript beacon](/pagespeed-markers/#pagespeed-beacon) to report client-side data. The beacon sends POST requests to the beacon URL.

**Fix:**

- This is expected behavior, not an error.
- If you do not use beacon-dependent filters, you can disable the beacon: `pagespeed CriticalImagesBeaconEnabled false;`
- To suppress the `<noscript>` redirect tag inserted by some filters: `pagespeed SupportNoScriptEnabled false;`

## Reading a `.pagespeed.` URL and debug markers

When mod_pagespeed optimizes a resource, it rewrites the URL to a self-describing form:

`originalName.pagespeed.FILTER_ID.HASH.extension`

For example, `styles.css.pagespeed.ce.GhT8kP2mNq.css`.

- **`FILTER_ID`** is a short code for the filter that produced the resource: `ce` (cache extension), `ic` (image rewriting), `jm` (JavaScript minification), `cc` (combined CSS), `cf` (CSS rewriting).
- **`HASH`** is a hash of the optimized resource's _content_. Because it is derived from the bytes, any content change yields a new URL. That is why [`extend_cache`](/1.1/docs/caching-url-filters/#extend_cache) can serve a one-year `Cache-Control` lifetime without the URL ever going stale.

### `data-pagespeed-url-hash` is a different hash

A `data-pagespeed-url-hash` attribute on an `<img>` tag is **not** the content hash above. It is a hash of the image's _original URL_, inserted by the critical-image beacon so the beacon JavaScript can report which images rendered above the fold without parsing rewritten URLs. Seeing it, along with the beacon POSTs that go with it, is expected; see [Beacons causing unexpected POST requests](#beacons-causing-unexpected-post-requests).

### Other markers you may see

| Marker                                          | Meaning                                                                                                          |
| ----------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| `data-pagespeed-no-defer`                       | marks a `<script>` to be skipped by `defer_javascript`                                                           |
| `type="text/psajs"`                             | `defer_javascript` retypes scripts to this non-executing type so the browser skips them during parse; the `src` attribute is left intact |
| `data-pagespeed-orig-index`                     | records a deferred script's original position in the page so execution order is preserved                        |
| `?PageSpeed=noscript`                           | requests the page with JavaScript-dependent rewrites (lazyload, defer) disabled — the `<noscript>` fallback link |
| `mod_pagespeed_beacon` / `ngx_pagespeed_beacon` | the URL the instrumentation beacon POSTs client-side measurements to                                             |

For the history behind content-hashed URLs, and why the rest of the web adopted the same pattern years later, see [Why optimized URLs carry a content hash](/blog/content-hash-urls/).

## How to get more diagnostic information

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

- Check the admin page at `/pagespeed_admin/` for statistics, active filters, and cache status.
- Enable message history: set `MessageBufferSize` to a non-zero value, then view messages at `/pagespeed_admin/message_history`.
- Add the `debug` filter temporarily to see detailed rewriting information in HTML comments: `pagespeed EnableFilters debug;`
- Check nginx error logs for mod_pagespeed messages.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

- Check the admin page at `/pagespeed_admin/` for statistics, active filters, and cache status.
- Enable message history: set `MessageBufferSize` to a non-zero value, then view messages at `/pagespeed_admin/message_history`.
- Add the `debug` filter temporarily: `ModPagespeedEnableFilters debug`
- Check Apache error logs for mod_pagespeed messages.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

- **Windows Event Viewer:** Open Event Viewer and check **Windows Logs > Application** for warnings and errors from the mod_pagespeed source. The module logs startup issues, configuration errors, and runtime warnings here automatically.
- **Admin pages:** Navigate to `/pagespeed_admin/` for statistics, active filters, and cache status.
- **Message history:** Set `pagespeed MessageBufferSize 100000` in your config, then view messages at `/pagespeed_global_admin/message_history`. Messages are collected globally from all IIS worker processes.
- **DebugView:** Use the [Sysinternals DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) tool for real-time debug output. Enable debug logging by adding `pagespeed diagnose` to your server-level `pagespeed.config`. In DebugView, enable **Capture > Capture Global Win32** to see the output.
- **Debug filter:** Add `?PageSpeedFilters=+debug` to any URL to see detailed optimization decisions, timing data, and filter descriptions in HTML comments.
- **App pool permissions:** Verify that the IIS app pool identity has read access to `pagespeed.config` and write access to the cache directory. Permission errors show up in Event Viewer.

</div>

## See also

- [Admin Console](/1.1/docs/admin-console/) — monitoring and cache management
- [Configuration](/1.1/docs/configuration/) — directive reference
- [FAQ](/1.1/docs/faq/) — common questions
- [PageSpeed markers reference](/pagespeed-markers/) — the runtime debug markers and what they mean
