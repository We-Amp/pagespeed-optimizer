---
title: 'Choose a cache mode: safe vs aggressive'
description: 'Control the Cache-Control headers ModPageSpeed 2.0 sets on optimized responses. Safe mode (default) adds must-revalidate for fast recovery; aggressive mode uses long TTLs with stale-if-error for cache efficiency.'
order: 22
group: 'Configure'
lastUpdated: 2026-07-04
---

ModPageSpeed 2.0 transforms your origin's content — optimizing images, minifying
CSS, and rewriting HTML. Because the output depends on ModPageSpeed's
configuration and software version, the [Cache-Control headers your origin
sends](/docs/cache-control/) don't apply verbatim to the transformed output. A misconfigured option or a software bug
can produce broken content that gets cached by browsers and CDNs.

Cache modes control how quickly you can recover from that situation. For the
reasoning behind the defaults — why `must-revalidate`, not `max-age`, is the
real safety net — see [Cache mode safety: must-revalidate vs aggressive TTL](/blog/cache-mode-safety-must-revalidate-vs-aggressive/).

## Two modes

|                   | Safe (default)                  | Aggressive (opt-in)                           |
| ----------------- | ------------------------------- | --------------------------------------------- |
| **CSS / JS**      | `max-age=300, must-revalidate`  | `public, max-age=86400, stale-if-error=86400` |
| **Images**        | `max-age=1800, must-revalidate` | `public, max-age=86400, stale-if-error=86400` |
| **HTML**          | `no-cache`                      | `no-cache`                                    |
| **Recovery time** | 5–30 minutes                    | Up to 24 hours (or CDN purge)                 |
| **SWR synthesis** | Suppressed                      | Allowed                                       |
| **`immutable`**   | Stripped                        | Stripped                                      |

Aggressive mode lets downstream caches serve optimized content without
revalidation for the full TTL. This matches typical production CDN-backed
deployments; the tradeoff is slower recovery if a misconfiguration produces
broken output.

## Safe mode (default)

Safe mode adds `must-revalidate` to all non-HTML responses and uses short
max-age values. After the TTL expires, downstream caches **must** revalidate
with the origin — they cannot serve stale content.

This means:

- Broken CSS self-corrects within 5 minutes.
- Broken images self-correct within 30 minutes.
- HTML is always fresh (revalidated on every request).
- If the origin is unreachable after TTL expiry, caches return 504 rather than
  serving stale content. This is intentional — a visible error is better than
  silently serving corrupted content.

Revalidation costs little: ModPageSpeed generates ETags on all cache hits, so
most revalidation requests return 304 Not Modified with no body transfer.

## Aggressive mode

Aggressive mode uses long TTLs with `public` and `stale-if-error=86400`. CDN
edges and browsers cache content for up to 24 hours. If the origin is
unreachable, stale content is served for up to 24 hours before returning 504.

`stale-while-revalidate` synthesis is enabled by default in aggressive mode,
allowing browsers to serve stale content while revalidating in the background.

Switch to aggressive mode when:

- You have run in safe mode for at least a week without issues.
- Your optimization configuration is stable and tested.
- You have CDN purge capability for emergency corrections.
- Cache efficiency matters more than instant recovery.

## Configuration

### nginx

```nginx
pagespeed_cache_mode safe;        # default
pagespeed_cache_mode aggressive;  # opt-in
```

**Context:** `http`, `server`, `location`
**Default:** `safe`

The per-type max-age defaults change based on the active mode:

| Directive                 | Safe default  | Aggressive default |
| ------------------------- | ------------- | ------------------ |
| `pagespeed_css_max_age`   | 300 (5 min)   | 86400 (1 day)      |
| `pagespeed_image_max_age` | 1800 (30 min) | 86400 (1 day)      |

You can override these in either mode:

```nginx
pagespeed_cache_mode safe;
pagespeed_css_max_age 600;     # 10 min instead of 5
pagespeed_image_max_age 3600;  # 1 hour instead of 30 min
```

In safe mode, the response includes `must-revalidate` regardless of your
max-age value. The safety mechanism is `must-revalidate`, not the TTL.

### ASP.NET Core

```json
{
  "PageSpeed": {
    "CacheMode": "Safe",
    "CssMaxAgeSeconds": 300,
    "ImageMaxAgeSeconds": 1800
  }
}
```

| Option               | Type   | Default                          | Description                                      |
| -------------------- | ------ | -------------------------------- | ------------------------------------------------ |
| `CacheMode`          | string | `Safe`                           | `Safe` or `Aggressive`.                          |
| `CssMaxAgeSeconds`   | int    | 300 (safe) / 86400 (aggressive)  | Max-age for CSS and JS cache hits.               |
| `ImageMaxAgeSeconds` | int    | 1800 (safe) / 86400 (aggressive) | Max-age for image cache hits.                    |
| `HtmlMaxAgeSeconds`  | int    | 0                                | Max-age for HTML cache hits. 0 means `no-cache`. |

## What about `immutable`?

ModPageSpeed strips `immutable` from all transformed content in both modes.
Your origin may send `Cache-Control: immutable` for fingerprinted assets, but
ModPageSpeed's output depends on mutable state (configuration, software version,
capability detection). The long TTL from `pagespeed_immutable_max_age` is
preserved in aggressive mode — you get the cache performance without a false
immutability claim.

## CDN considerations

`must-revalidate` in safe mode means CDN edges must revalidate after TTL expiry.
If the origin is unreachable, the CDN returns 504 instead of serving stale
content. This is by design.

In aggressive mode, `stale-if-error=86400` allows CDN edges to serve stale
content for up to 24 hours during origin outages.

Some CDN configurations can override origin cache headers (e.g., Cloudflare
"Cache Everything" page rules with edge TTL). These CDN-level overrides take
precedence over ModPageSpeed's headers.

For CDN-specific guidance including Vary header recommendations, see
[CDN Integration](/docs/cdn-integration/).
