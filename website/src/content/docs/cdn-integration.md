---
title: 'Integrate with a CDN'
description: 'Run mod_pagespeed 2.1 behind Cloudflare, CloudFront, or Fastly: how Vary headers affect edge caching, per-CDN cache-key settings, and how to keep hit rates high.'
order: 23
group: 'Configure'
lastUpdated: 2026-09-19
---

mod_pagespeed 2.1 runs behind any CDN. The one thing to get right is how your CDN
treats the `Vary` headers mod_pagespeed emits — get it wrong and edge hit rates
collapse. This page explains that interaction and gives concrete settings for
Cloudflare, CloudFront, and Fastly.

## Vary: User-Agent

mod_pagespeed serves different optimized variants at the same URL based on client
capabilities (image format support, viewport class, pixel density, encoding).
To tell downstream caches about this, it emits Vary headers:

| Content type | Vary header                     |
| ------------ | ------------------------------- |
| Images       | `Accept, Save-Data, User-Agent` |
| HTML         | `Accept-Encoding, User-Agent`   |
| CSS / JS     | `Accept-Encoding`               |

Internally, mod_pagespeed classifies User-Agent strings into a small number of
capability dimensions: 3 viewport classes (mobile, tablet, desktop) × 2 pixel
densities = 6 distinct variants. This makes the variation finite and cacheable.
(See [viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
for how those viewport classes drive the image pipeline.)

However, most CDNs treat `Vary: User-Agent` as effectively uncacheable because
they see every unique User-Agent string as a different cache key — not the 6
capability classes that mod_pagespeed actually uses.

### Impact

- **Browser caching** works correctly regardless. The same browser always sends
  the same User-Agent, so resources stay cached within a session.
- **CDN edge caching** is impaired for images and HTML. The CDN stores many
  near-identical variants, reducing hit rates.
- **CSS/JS** is unaffected — it uses only `Vary: Accept-Encoding`, which CDNs
  handle well.

## Per-CDN recommendations

### Cloudflare

Cloudflare ignores `Vary: User-Agent` by default on most plan tiers. It caches
one variant regardless of User-Agent, using its own device classification for
mobile/desktop decisions.

**Recommendation:** mod_pagespeed's safe mode (short TTLs + `must-revalidate`)
is a good fit. If image variant accuracy is critical, consider Enterprise tier
with custom cache keys.

### AWS CloudFront

CloudFront does not vary on User-Agent by default. To enable device-aware
caching, whitelist CloudFront's built-in device headers in your cache policy:

- `CloudFront-Is-Mobile-Viewer`
- `CloudFront-Is-Tablet-Viewer`
- `CloudFront-Is-Desktop-Viewer`

These headers are derived from User-Agent by CloudFront itself and produce a
manageable number of cache variants.

**Recommendation:** Use CloudFront's device headers for images. Leave CSS/JS
caching as-is.

### Fastly

Fastly (Varnish-based) respects `Vary` faithfully, creating separate cache
entries per unique Vary combination. With `Vary: User-Agent`, this creates
thousands of variants per URL. Fastly limits variants to 256 per URL, after
which it evicts aggressively.

**Recommendation:** Use custom VCL to normalize User-Agent into capability
classes, or strip User-Agent from Vary and rely on mod_pagespeed's internal
classification.

### Other CDNs / No CDN

If mod_pagespeed is your edge (no CDN in front), the Vary headers work correctly.
Downstream browser caches and ISP proxies handle `Vary: User-Agent` at the
per-client level, which produces correct behavior since each client has a stable
User-Agent.

## Future directions

We are evaluating these approaches to improve CDN compatibility:

- **Configurable Vary strategy** — A directive to remove User-Agent from Vary
  for operators behind CDNs that handle device classification themselves.
- **Client Hints** — Using `Sec-CH-Mobile`, `Sec-CH-Viewport-Width`, and
  `Sec-CH-DPR` instead of User-Agent parsing. Modern standard but not
  universally supported yet.
- **Custom capability header** — Emitting an `X-PS-Cap` header with the
  normalized capability class, enabling CDN cache key customization.

For now, the [safe cache mode](/docs/cache-modes/) mitigates the worst impact of
CDN cache misses — even with low CDN hit rates, the short TTLs and ETag-based
304 revalidation keep bandwidth overhead minimal. See
[cache-control headers](/docs/cache-control/) for the TTL and revalidation
directives behind this behavior.
