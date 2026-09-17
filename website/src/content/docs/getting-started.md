---
title: 'Getting Started'
description: 'Install ModPageSpeed 2.0. Two integrations share one optimization pipeline: a Docker / nginx reverse proxy, or ASP.NET Core middleware.'
order: 1
group: 'Start here'
lastUpdated: 2026-07-04
faq:
  - q: 'Which ModPageSpeed 2.0 integration should I pick?'
    a: 'Docker / nginx reverse proxy for new deployments and Kubernetes; ASP.NET Core middleware for in-process .NET pipelines. Both share the same C++ optimization core. For native Apache/nginx/IIS/Envoy modules on bare-metal today, use mod_pagespeed 1.15 — a standalone nginx 2.0 module is on the 2.x roadmap.'
  - q: 'What does the request flow look like on a cache hit?'
    a: 'Nginx classifies the client into a 32-bit capability mask, finds a matching optimized variant in the Cyclone cache, and serves it zero-copy from the memory-mapped file with `X-PageSpeed: HIT`. No origin round-trip and no allocation.'
  - q: 'How do I verify ModPageSpeed is working?'
    a: 'Send `curl -I` to any page. The first response shows `X-PageSpeed: MISS` (proxied to origin), the second shows `X-PageSpeed: HIT` (served from cache). For images, request with `Accept: image/webp` and compare downloaded size against the original.'
  - q: 'What is safe cache mode and why is it the default?'
    a: 'Safe mode caches optimized resources for short periods (5 minutes for CSS/JS, 30 minutes for images) with mandatory revalidation, so misconfigurations self-correct quickly. It is the recommended mode while validating a new setup before switching to aggressive.'
  - q: 'What are the prerequisites?'
    a: 'For the Docker / nginx reverse proxy you need only Docker — nginx ships inside the image, so you do not install it yourself. For the ASP.NET Core middleware, the .NET 8 or .NET 10 SDK. The worker runs on Linux x86_64 or arm64 (Debian/Ubuntu or RHEL/Rocky).'
---

ModPageSpeed 2.0 ships two integrations:

- **Docker / nginx reverse proxy** — drop in front of any HTTP origin (Apache,
  Node.js, Caddy, IIS, your CDN's origin). Best for new deployments and
  Kubernetes. [Get started with Docker &rarr;](/docs/installation-docker/)
- **ASP.NET Core middleware** — a NuGet package that runs inside your
  ASP.NET Core pipeline. No proxy, no sidecar. [Get started with ASP.NET Core &rarr;](/docs/aspnet-getting-started/)

Both run the same C++ optimization pipeline: image transcoding, CSS/JS
minification, critical CSS, and variant-aware caching with zero-copy serving
from the Cyclone shared-memory cache. See the
[full optimization filter set](/features/) for everything the pipeline applies.

To see which failing audits ModPageSpeed will fix, run your site through a
[PageSpeed Insights test](/analyze/). For a per-platform plan to improve LCP,
CLS, and INP, read the [Core Web Vitals](/core-web-vitals/) guide.

> **Looking for a standalone nginx 2.0 module?**
> Not in the current 2.0 release. For native Apache, nginx, IIS, or Envoy modules today, use
> **[mod_pagespeed 1.15](/1.1/)** — the native server module line for Apache,
> nginx, IIS, and Envoy. A 2.0 standalone nginx module is on the 2.x roadmap.

## How the Docker / nginx reverse-proxy integration works

The reverse-proxy stack uses three components that work together:

1. **Caching Proxy (nginx)** — A dynamic nginx module (`ngx_pagespeed_module.so`)
   that classifies incoming requests, serves cached optimized content via
   zero-copy mmap, and proxies cache misses to your origin server.

2. **Cyclone Cache** — A shared disk cache file that stores both original and
   optimized content variants. Both nginx and the worker access it via
   memory-mapped I/O for sub-millisecond lookups.

3. **Worker** — A C++ worker that reads original content
   from the cache, performs optimizations (image transcoding, CSS/JS
   minification), and writes optimized variants back to the cache.

For evaluation and small single-host deployments, the combined
`ghcr.io/we-amp/pagespeed-combined` image runs nginx and the worker together in
one container — see [Install with Docker](/docs/installation-docker/#quick-try-one-container).

The ASP.NET Core integration collapses this into a single in-process pipeline
— the optimization library runs as P/Invoke calls from the middleware, no
separate worker process needed. See
[ASP.NET Core Getting Started](/docs/aspnet-getting-started/) for the
middleware-specific architecture.

### Request flow (nginx integrations)

When a request arrives:

1. Nginx classifies the client's capabilities (image format support, viewport,
   transfer encoding, Save-Data preference) into a 32-bit capability mask.
2. The cache is checked for an optimized variant matching that mask.
3. **On cache hit:** The content is served directly from the memory-mapped cache
   file — no copies, no allocations. The response includes an `X-PageSpeed: HIT`
   header.
4. **On cache miss:** The request is proxied to your origin. The response is
   stored in the cache and served to the client with an `X-PageSpeed: MISS`
   header. A notification is sent to the worker.
5. The worker reads the original content, optimizes it, and writes the result
   back to the cache. Future requests for the same capability mask get the
   optimized version.

## Prerequisites

- **Docker** — for the Docker / nginx reverse proxy. nginx (1.30.2) ships
  inside the image; you do not install it separately.
- **.NET 8 or .NET 10 SDK** — for the ASP.NET Core middleware
- **Linux** (Debian/Ubuntu or RHEL/Rocky), x86_64 or arm64 — for the worker.
  The .NET middleware also runs on macOS and Windows.

## Quick verification

Once installed (via either method), verify that ModPageSpeed is
working:

```bash
# Request a page — first request will be a cache miss
curl -I http://localhost/

# Look for the X-PageSpeed header
# X-PageSpeed: MISS   (first request, proxied to origin)
# X-PageSpeed: HIT    (subsequent requests, served from cache)
```

Request the same URL again after a moment. Optimization happens asynchronously
between the two requests: the first returns the original bytes with
`X-PageSpeed: MISS` and notifies the worker, and the second should show
`X-PageSpeed: HIT` once the worker has written the optimized variant.

To verify that optimizations are being applied, request an image with WebP
support:

```bash
curl -H "Accept: image/webp,*/*" -o /dev/null -w "%{size_download}" \
  http://localhost/image.jpg
```

The response size should be smaller than the original once the worker has
processed it. If it still matches, the worker has not finished yet — wait a
moment and retry. The URL stays the same; only the bytes and `Content-Type`
change, because 2.0 negotiates by the `Accept` header instead of rewriting URLs.

## Safe cache mode

By default, ModPageSpeed runs in **safe cache mode**. Optimized resources are
cached for short periods (5 minutes for CSS/JS, 30 minutes for images) with
mandatory revalidation, so misconfigurations self-correct quickly. This is the
recommended mode while you validate your setup. See [Cache Modes](/docs/cache-modes/)
for details and options.

## Next steps

- [Configuration Reference](/docs/configuration/) — All nginx directives,
  worker flags, and tuning options
- [Deployment Guide](/docs/deployment/) — Production setup, monitoring, and
  cache sizing
- [Web Console](/docs/workbench/) — Inspect cache state, monitor performance,
  and tune configuration
- [Troubleshooting](/docs/troubleshooting/) — Common issues and diagnostics
