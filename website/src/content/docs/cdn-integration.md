---
title: 'Integrate with a CDN'
description: 'Run mod_pagespeed 2.1 behind Cloudflare, CloudFront or Fastly: how Vary headers affect edge caching, per-CDN cache-key settings and keeping hit rates high.'
order: 23
group: 'Configure'
lastUpdated: 2026-10-21
---

mod_pagespeed 2.1 runs behind any CDN. The one thing to get right is how your CDN
treats the `Vary` headers mod_pagespeed emits — get it wrong and edge hit rates
collapse. This page explains that interaction and gives concrete settings for
Cloudflare, CloudFront, and Fastly.

## Vary: User-Agent

mod_pagespeed serves different optimized variants at the same URL based on client
capabilities (image format support, viewport class, pixel density, encoding).
To tell downstream caches about this, it emits Vary headers:

| Content type                                  | Vary header                                 |
| --------------------------------------------- | ------------------------------------------- |
| Images (optimizer's nginx module, containers) | `Accept, Save-Data, User-Agent, Sec-CH-DPR` |
| Images (ASP.NET Core middleware)              | `Accept, Save-Data, User-Agent`             |
| HTML                                          | `Accept-Encoding, User-Agent`               |
| CSS / JS                                      | `Accept-Encoding`                           |

The native Apache, nginx and IIS modules with the optimizer worker attached
send a shorter line on images: `Vary: Accept` on an optimized copy chosen from
the request's `Accept` header (with `Accept-Encoding` where compression
applies). A response that carries the origin's own bytes, such as the first
request for a URL, carries no `Vary: Accept` unless the origin sent one.

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
  near-identical variants, reducing hit rates. A CDN that ignores
  `Vary: User-Agent` (Cloudflare does, by default) can do worse on the
  optimizer's nginx module: it caches the viewport-sized copy the first
  visitor got and serves it to every device class.
- **CSS/JS** is unaffected — it uses only `Vary: Accept-Encoding`, which CDNs
  handle well.

## Image format and the Accept header {#image-format-negotiation}

An image keeps its URL; the format of the body is chosen per request from the
`Accept` header. For a `.jpg` or `.png` URL, once the optimizer has written the
copies:

| The request's `Accept`                                            | Body served                              |
| ----------------------------------------------------------------- | ---------------------------------------- |
| names `image/avif`                                                | AVIF                                     |
| names `image/webp`, or contains `image/*` or `*/*` (but not AVIF) | WebP                                     |
| is absent or empty                                                | WebP: treated exactly like `*/*`         |
| anything else, for example `image/jpeg` or `image/png` only       | the original format, optimized in place |

So a client that sends `Accept: */*`, `Accept: image/*` or no `Accept` header at
all receives WebP for a `.jpg` URL. That includes `curl`, `wget` and most HTTP
libraries, which send `Accept: */*` by default. This is deliberate: under RFC
9110 §12.5.1, `*/*` and `image/*` say the client takes any image type, and a
request with no `Accept` header means the same thing. Some browsers rely on it:
Chrome on iOS, for example, sends `Accept: */*` for images without naming WebP.
AVIF is served only when the request names `image/avif`; a wildcard never
selects it.

Two details of the matching: quality values are not weighed, so a listed type
counts even with `;q=0`, and a wildcard anywhere in the header wins over the
specific types next to it, so `Accept: image/jpeg, */*` still gets WebP. When no
copy in the negotiated format exists at the client's size class (not written
yet, or declined by the quality check), the request gets the original format
at that size; a client that names `image/avif` is never given WebP instead. On the native modules without the
optimizer worker (the module's own in-place path), none of this applies: an
image optimized in place is the same bytes for every client and carries no
`Vary: Accept`.

A response whose format was chosen this way carries `Vary: Accept` (see the
table above), so a cache that honours `Vary` keeps the formats apart. A CDN
that ignores `Vary: Accept` caches whichever format the first visitor got, and
can hand an AVIF or WebP body to a client that cannot decode it.

To cache images at the edge, key them on `Accept`, normalized with the same
mapping the optimizer uses, so the edge holds three copies per URL instead of
one per distinct `Accept` string:

- `avif` when `Accept` contains `image/avif`;
- `webp` when it contains `image/webp`, `image/*` or `*/*`, or is absent;
- `original` otherwise.

Forward the normalized value as the request's `Accept` (for example
`image/avif`, `image/webp` or `image/jpeg`), so the origin answers for the
class the edge cached it under. If you cannot do that, do not cache images at
the edge.

**The native modules need more than `Accept` today.** Behind the native
Apache, nginx or IIS module with the optimizer worker attached, the copy is
also chosen by device class and pixel density (from `User-Agent`) and by
`Save-Data`, but the response names only `Accept` in `Vary`
([mod_pagespeed#102](https://github.com/We-Amp/mod_pagespeed/issues/102)).
Until that is fixed, a shared cache keyed on `Accept` alone can hand one
device class's or Save-Data copy to another client. Bypass the edge cache for
images there, or key on a `User-Agent` device class and `Save-Data` as well.

If a non-browser client must receive the original format:

- **Send an `Accept` header that names only that format**, with no `*/*` or
  `image/*`, for example `Accept: image/jpeg`. This is the per-request switch;
  there is no setting that serves the original format to wildcard requests.
- **On the native Apache, nginx and IIS modules, append `?PageSpeed=off`** to
  the URL to get the response as the origin serves it. The Docker reverse proxy
  does not read this parameter.
- **Exclude the URLs from optimization** with
  [`pagespeed_disallow`](/docs/worker-configuration/#pagespeed_disallow) on the
  Docker reverse proxy, or [`Disallow`](/docs/configuration/#disallow) on the
  native modules. This applies to every client.
- **Turn off image transcoding** in the optimizer worker with
  [`--disable-image`](/docs/worker-configuration/#--disable-image). This also
  applies to every client and stops all image optimization.

## Per-CDN recommendations

### Cloudflare

Cloudflare does not key its cache on `Vary: Accept` or `Vary: User-Agent`
unless you configure it to: by default an image is cached once, for every
client. For images, choose one of:

- **The Vary setting in a Cache Rule** (all plans): give the `Accept` header
  the `normalize` action, and Cloudflare keeps a copy per normalized `Accept`
  value for responses that send `Vary: Accept`. Leave its media type list
  empty: narrowing it can put a request with no `Accept` header (served WebP)
  and one that names only `image/jpeg` (served the original) under one key.
- **Vary for Images** (Pro, Business and Enterprise plans, configured through
  the API only): declare that `.jpg`, `.jpeg` and `.png` have `image/avif` and
  `image/webp` variants, and Cloudflare caches each format separately.
  Requests with `Accept: */*` or no `Accept` header bypass that cache, because
  the origin answers them with WebP for a `.jpg` URL rather than the original
  type; they are served correctly, just not from the edge.
- **A custom cache key on the `Accept` header** (Enterprise plans).
- **Bypass the edge cache for images** with a Cache Rule.

See Cloudflare's [Vary](https://developers.cloudflare.com/cache/concepts/vary/)
and
[Vary for Images](https://developers.cloudflare.com/cache/advanced-configuration/vary-for-images/)
documentation for the current setup. For HTML, mod_pagespeed's safe mode
(short TTLs + `must-revalidate`) is a good fit.

### AWS CloudFront

CloudFront caches separate copies only for the request headers named in the
cache policy, and does not act on the origin's `Vary: Accept`. A header that is
not in the policy is not forwarded as sent: CloudFront removes `Accept`, and
replaces `User-Agent` with `Amazon CloudFront`. So with no `Accept` in the
cache policy, every client gets WebP (an absent `Accept` counts as `*/*`) and
none gets AVIF.

Add `Accept` to the cache policy for images, and normalize it first with a
CloudFront Function on the viewer request (the mapping above), so the cache
key holds three values instead of every browser's `Accept` string. Do not add
the `CloudFront-Is-*-Viewer` device headers for images: mod_pagespeed does not
read them, so they split the cache without changing the bytes.

**Recommendation:** Key images on the normalized `Accept` alone. Leave CSS/JS
caching as-is.

### Fastly

Fastly (Varnish-based) respects `Vary` faithfully, creating separate cache
entries per unique Vary combination. With `Vary: User-Agent`, this means one
variant per User-Agent string, and Fastly limits the number of variants per
object; its `vcl_hash` reference recommends moving the variation into the cache
key instead when there are many.

`Vary: Accept` is honoured the same way: every distinct `Accept` string gets
its own cache entry.

**Recommendation:** Use custom VCL to normalize `req.http.Accept` into the
three classes above (`image/avif`, `image/webp`, the original type) and
`User-Agent` into capability classes, and add both to the cache key in
`vcl_hash`, or bypass the cache for images. Do not strip `User-Agent` from
`Vary` and rely on mod_pagespeed's classification: the origin only classifies
on a cache miss, so Fastly would serve the first device class's copy to
everyone.

### Other CDNs / no CDN

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
