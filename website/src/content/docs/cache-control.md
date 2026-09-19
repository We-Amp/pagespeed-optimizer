---
title: 'Set Cache-Control headers'
description: 'Which Cache-Control headers to set on your origin, by content type and by framework, for correct caching with mod_pagespeed 2.1.'
order: 21
group: 'Configure'
lastUpdated: 2026-09-19
---

> **Requires ModPageSpeed 2.0.x or later** with conditional revalidation support.
> Without conditional revalidation, `must-revalidate` causes full origin
> re-fetches on every stale request. Upgrade before following these
> recommendations.

Your origin's `Cache-Control` headers directly control how mod_pagespeed caches
and serves your content. Getting these right prevents stale content and cuts
unnecessary origin traffic.

When your origin sends no `Cache-Control` header, mod_pagespeed applies
configurable defaults (HTML: `no-cache`, CSS/JS: 300s, images: 1800s in
[safe mode](/docs/cache-modes/)) and logs
a warning. Explicit headers are better than relying on
[these fallback TTLs](/blog/default-cache-ttl-heuristic-freshness/).

## Recommended Headers by Content Type

### HTML Pages

```http
Cache-Control: public, max-age=60, must-revalidate
ETag: "content-hash-or-version"
```

HTML references hashed assets. A deploy changes HTML content to point at new
asset URLs. Short `max-age=60` provides one minute of caching. With conditional
revalidation, stale requests are cheap — origin returns 304 when content hasn't
changed.

Use `max-age=0` only when absolute real-time freshness is required (stock
tickers, live scores). Every request with `max-age=0` triggers a conditional
revalidation, which means an origin round-trip even when content hasn't changed.

### Hashed Static Assets (CSS/JS with Fingerprints)

```http
Cache-Control: public, max-age=31536000, immutable
```

The URL changes on every build. Content at a given URL never changes.
mod_pagespeed strips `immutable` from its transformed output — the optimized
bytes depend on mutable config and software version — but preserves the long
TTL, capping max-age at `pagespeed_immutable_max_age` (default: 7 days).

### Non-Hashed Static Assets

```http
Cache-Control: public, max-age=3600, must-revalidate
ETag: "file-mtime-and-size"
```

The URL is stable but content may change. Short max-age with `must-revalidate`
keeps content fresh. Conditional revalidation makes the revalidation cheap.

### User-Uploaded Images

```http
Cache-Control: public, max-age=86400
```

User images rarely change at the same URL. 24-hour caching is reasonable.
If images can be replaced at the same URL, add `must-revalidate` and an `ETag`.

### API Responses and Dynamic Content

```http
Cache-Control: no-store
```

mod_pagespeed does not cache `no-store` responses. The response passes through
unchanged.

## Framework Configuration

| Framework          | Where to Set Headers                | HTML                          | Hashed Assets                 |
| ------------------ | ----------------------------------- | ----------------------------- | ----------------------------- |
| **nginx** (static) | `nginx.conf` per `location`         | `max-age=60, must-revalidate` | `max-age=31536000, immutable` |
| **Apache**         | `.htaccess` `Header set`            | `max-age=60, must-revalidate` | `max-age=31536000, immutable` |
| **Next.js 13+**    | `next.config.js` `headers()`        | `max-age=60, must-revalidate` | Automatic for `_next/static/` |
| **Astro 4+**       | Server adapter `headers`            | `max-age=60, must-revalidate` | Automatic via content hashing |
| **Rails 7+**       | `config.public_file_server.headers` | `max-age=60, must-revalidate` | Automatic via Propshaft       |
| **WordPress**      | Cache plugin or `.htaccess`         | `max-age=60, must-revalidate` | Theme-dependent               |

## PURGE vs Natural Expiration

`PURGE` immediately invalidates all cached variants for a URL. Use it for
urgent content corrections. For routine deploys, let `must-revalidate` handle
freshness — conditional revalidation is cheaper than purge-and-rebuild
because it preserves optimized image variants (AVIF, WebP) when only the HTML
changed. See [304 conditional revalidation vs PURGE](/blog/conditional-revalidation-304-vs-active-purge/)
for when each is the right invalidation strategy.

## Cache Mode

The `pagespeed_cache_mode` directive controls how mod_pagespeed assembles
Cache-Control headers on optimized responses. Safe mode (the default) adds
`must-revalidate` and uses short TTLs for quick recovery from
misconfigurations. Aggressive mode uses long TTLs with `public` for maximum
cache efficiency. See [Cache Modes](/docs/cache-modes/) for full details, or
[the safety math behind must-revalidate vs aggressive TTLs](/blog/cache-mode-safety-must-revalidate-vs-aggressive/)
for how each mode trades freshness against origin load.

## mod_pagespeed Directives {#modpagespeed-directives}

These directives control the default max-age values applied when origin
responses lack a `Cache-Control` header. They do not override explicit
origin headers.

```nginx
pagespeed_html_max_age 0;      # Default for HTML (seconds, default: 0 = no-cache)
pagespeed_css_max_age 300;     # Default for CSS/JS (seconds, default: 300)
pagespeed_image_max_age 1800;  # Default for images (seconds, safe: 1800, aggressive: 86400)
```

When `pagespeed_html_max_age` is 0 and the origin sends no `Cache-Control`,
HTML responses are served with `Cache-Control: no-cache` — the client must
revalidate on every request.

### Conditional Revalidation

```nginx
pagespeed_conditional_revalidation on;   # default: on
```

When enabled, stale cache entries with stored `ETag` or `Last-Modified` values
trigger conditional requests (`If-None-Match` / `If-Modified-Since`) instead of
full re-fetches. On 304, the cached content is refreshed without re-downloading
or re-optimizing. Disable only if your origin mishandles conditional requests.

### Browser Force-Refresh

```nginx
pagespeed_force_refresh_html on;   # default: on
pagespeed_force_refresh off;       # default: off
```

When a user performs a force-refresh (Ctrl+F5 or Shift+Reload), the browser sends
`Cache-Control: no-cache` or `Pragma: no-cache`. mod_pagespeed detects these signals
and forces revalidation against your origin, even if the cached entry is fresh.

For HTML, this is enabled by default — users expect force-refresh to fetch the latest
page. For images, CSS, and JS, it is off by default to prevent unnecessary origin
load. Enable `pagespeed_force_refresh` if you want force-refresh to apply to all
content types.

When conditional revalidation is also enabled (the default), force-refresh uses
`If-None-Match` / `If-Modified-Since` to avoid re-downloading unchanged content.

## Cache-extension filters

The filters in this section belong to the in-process module on Apache,
nginx and IIS. They extend browser cache lifetimes and manage resource URLs
across domains. Browsers cache resources for a year, but pick up updated
content the moment a resource changes, because the URL carries a content
hash. The `extend_cache` CoreFilter drives this: it sets a 1-year cache
lifetime on CSS, JS, and image URLs.

### extend_cache {#extend_cache}

**Core filter.** Rewrites resource URLs (CSS, JS, images) to include a content hash, then serves the optimized resource with a 1-year `Cache-Control: max-age` header. When the original resource changes, the hash changes, generating a new URL that bypasses the browser cache.

Resources that previously had short or no cache lifetimes gain a 1-year cache lifetime. Because the URL carries the content hash, a changed resource gets a new URL and is never served stale.

The sub-filters `extend_cache_css`, `extend_cache_images`, and `extend_cache_scripts` are included when you enable `extend_cache`, which turns on all three. Each can also be enabled individually with `EnableFilters` (for example, `extend_cache_images` alone).

### extend_cache_pdfs {#extend_cache_pdfs}

**Not a CoreFilter.** Applies the same content-hash-based cache extension to PDF file links. Enable this filter if your site serves PDFs that change infrequently.

```apacheconf
# Apache
ModPagespeedEnableFilters extend_cache_pdfs
```

```nginx
# Nginx
pagespeed EnableFilters extend_cache_pdfs;
```

### rewrite_domains {#rewrite_domains}

**Not a CoreFilter. Test before deploying.** Rewrites resource URLs to use domains specified by `MapRewriteDomain` or `ShardDomain` directives. Useful for CDN integration or domain sharding.

```apacheconf
# Apache
ModPagespeedEnableFilters rewrite_domains
```

```nginx
# Nginx
pagespeed EnableFilters rewrite_domains;
```

This filter only affects resources that mod_pagespeed does not otherwise optimize. Resources already rewritten by other filters ([`rewrite_css`](/docs/css-filters/#rewrite_css), [`rewrite_images`](/docs/image-filters/#rewrite_images), etc.) already have their domains set by those filters.

See [Domain Configuration](/docs/domain-configuration/) for `MapRewriteDomain` and `ShardDomain` setup.

### local_storage_cache {#local_storage_cache}

**Experimental.** Stores inlined CSS and JavaScript in the browser's `localStorage` on first visit, then loads from `localStorage` on subsequent visits instead of re-inlining. This reduces HTML payload on repeat views at the cost of JavaScript complexity and reliance on `localStorage` availability.

```apacheconf
# Apache
ModPagespeedEnableFilters local_storage_cache
```

```nginx
# Nginx
pagespeed EnableFilters local_storage_cache;
```

Not recommended for most deployments. Browser `localStorage` has size limits (typically 5-10 MB per origin) and can be cleared by the user at any time. Sites with many inlined resources may exceed these limits. Since v1.15.0+r18, with `HonorCsp` (default on) the filter stands down on pages whose `Content-Security-Policy` disallows the inline script it relies on.

On IIS the same directives use the syntax described in [IIS configuration](/docs/iis-configuration/).

## Auditing Your Origin

The [web console](https://we-amp.com/console/) flags URLs where the origin sends no `Cache-Control`
header. Use the URL inspector to check freshness status and identify origins that
need explicit headers.
