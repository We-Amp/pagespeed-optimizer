---
title: 'Set Cache-Control headers'
description: 'Which Cache-Control headers to set on your origin, by content type and by framework, for correct caching with ModPageSpeed 2.0.'
order: 21
group: 'Configure'
lastUpdated: 2026-07-04
---

> **Requires ModPageSpeed 2.0.x or later** with conditional revalidation support.
> Without conditional revalidation, `must-revalidate` causes full origin
> re-fetches on every stale request. Upgrade before following these
> recommendations.

Your origin's `Cache-Control` headers directly control how ModPageSpeed caches
and serves your content. Getting these right prevents stale content and cuts
unnecessary origin traffic.

When your origin sends no `Cache-Control` header, ModPageSpeed applies
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
ModPageSpeed strips `immutable` from its transformed output — the optimized
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

ModPageSpeed does not cache `no-store` responses. The response passes through
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

The `pagespeed_cache_mode` directive controls how ModPageSpeed assembles
Cache-Control headers on optimized responses. Safe mode (the default) adds
`must-revalidate` and uses short TTLs for quick recovery from
misconfigurations. Aggressive mode uses long TTLs with `public` for maximum
cache efficiency. See [Cache Modes](/docs/cache-modes/) for full details, or
[the safety math behind must-revalidate vs aggressive TTLs](/blog/cache-mode-safety-must-revalidate-vs-aggressive/)
for how each mode trades freshness against origin load.

## ModPageSpeed Directives

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
`Cache-Control: no-cache` or `Pragma: no-cache`. ModPageSpeed detects these signals
and forces revalidation against your origin, even if the cached entry is fresh.

For HTML, this is enabled by default — users expect force-refresh to fetch the latest
page. For images, CSS, and JS, it is off by default to prevent unnecessary origin
load. Enable `pagespeed_force_refresh` if you want force-refresh to apply to all
content types.

When conditional revalidation is also enabled (the default), force-refresh uses
`If-None-Match` / `If-Modified-Since` to avoid re-downloading unchanged content.

## Auditing Your Origin

The [web console](https://we-amp.com/console/) flags URLs where the origin sends no `Cache-Control`
header. Use the URL inspector to check freshness status and identify origins that
need explicit headers.
