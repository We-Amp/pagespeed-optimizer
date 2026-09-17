---
title: 'mod_pagespeed and ngx_pagespeed alternatives in 2026'
description: 'Google mod_pagespeed and ngx_pagespeed are no longer actively developed. Two maintained successors — mod_pagespeed 1.15 and ModPageSpeed 2.0 — exist. Here is how to choose.'
date: 2026-03-16
lastUpdated: 2026-07-04
author: 'Otto van der Schaaf'
tags: ['migration', 'comparison', 'alternatives']
draft: false
---

If you landed here, you are probably running mod_pagespeed or ngx_pagespeed on a server that is overdue for an OS upgrade, and you just discovered that the module does not compile against your new nginx version. Or you are evaluating web optimization tools for the first time and wondering whether the Google project is still a viable option.

Short answer: the original Google project is [effectively no longer actively developed](/mod-pagespeed-still-maintained/). Two actively maintained successors exist, both built by We-Amp — the team that helped build ngx_pagespeed, maintained mod_pagespeed, and drove the project's Apache incubation. Here is the full picture.

## The current state of Google's mod_pagespeed

Google open-sourced mod_pagespeed in 2010. It was a full-stack optimization module that could rewrite HTML, transcode images, minify CSS and JavaScript, inline critical resources, combine files, and defer loading. All automatically, with no application code changes. The nginx port (ngx_pagespeed) followed in 2013.

The last meaningful development on the Google project happened around 2020-2021. The GitHub repositories are still public, but there have been no releases, no patch merges, and no maintainer activity since. Issues pile up. Pull requests go unreviewed. The CI pipelines have not run in years.

For Apache users, the existing module still functions if you can get it to compile. The situation is worse for nginx. ngx_pagespeed must be compiled against exact nginx source headers. It is a static module, not a dynamic one. Every nginx version upgrade requires downloading the matching source tree and recompiling the module from scratch. Modern distributions (Ubuntu 24.04, Debian 12, Rocky 9) ship nginx versions that the last ngx_pagespeed release was never built or tested against. Headers have changed. APIs have shifted. Getting it to compile is an exercise in patching, and even when it builds, you are running untested code against a production web server.

## What broke and why

The project was designed for a team. At its center sits RewriteDriver: over 2,000 lines of C++ orchestrating 60+ filters through a pipeline where ordering matters, filters interact in subtle ways, and the configuration matrix spans hundreds of possible combinations. This is the kind of system that takes years and dedicated engineers to get right, and continuous effort to keep working.

Maintaining it means tracking changes across two web servers (Apache and nginx), testing the full filter matrix against new compiler versions, new OS releases, new TLS libraries, and new nginx APIs. When Google's team moved on, that maintenance surface did not shrink. The community could file issues and submit patches, but reviewing a change to the filter pipeline safely takes deep, sustained familiarity with how those filters interact — the kind of expertise that lives in a funded, full-time team, not in occasional volunteer time. The maintenance economics did not add up once the original team's investment ended.

There was an attempt to fix this. We-Amp helped drive mod_pagespeed into the Apache Software Foundation incubator, hoping to build a broader maintainer community. The incubation failed. We-Amp remained the only active contributor, and no other parties stepped up to share the maintenance burden.

## Two successors, two approaches

I maintained the original project for years. When Google moved on, I continued the work commercially under We-Amp B.V. ([Why I rebuilt mod_pagespeed from scratch](/blog/why-i-rebuilt-mod-pagespeed/) is the longer version of that story.) The result is two actively maintained products that take different approaches to the same problem.

### mod_pagespeed 1.15: the native in-process module

mod_pagespeed 1.15 runs inside Apache, nginx, and IIS as a native in-process module, with no separate worker and no reverse-proxy hop. If you are running Google's mod_pagespeed today and want the closest drop-in replacement, this is it.

**What stayed the same:** The filter architecture. The Apache and nginx module integration. The configuration directives. Your existing `ModPagespeed*` directives work. Your Disallow patterns work. The optimization pipeline you know (rewrite_images, rewrite_css, rewrite_javascript, prioritize_critical_css) is the same pipeline, actively maintained and tested.

**What changed:**

- **Modernized build and dependencies.** The build system was migrated to Bazel and every dependency updated to current versions. No more pinned-to-2018 libraries or bespoke build scripts.
- **Actively maintained against current distributions.** Compiled and tested on Debian 11, 12, and 13, Ubuntu 22.04 and 24.04, AlmaLinux/RHEL/Rocky 9 and 10, and current nginx/Apache releases. The compatibility issues that plague the Google version (no longer actively developed) do not exist here.
- **Multi-port.** Beyond the original Apache and nginx, 1.15 adds a GA IIS port and an experimental Envoy port, broadening the server coverage that Google never pursued. Apache and nginx packages ship as signed `.deb`/`.rpm` from [packages.modpagespeed.com](https://packages.modpagespeed.com/). The nginx dynamic module — `nginx-module-pagespeed` — is prebuilt for Debian 11/12/13 and Ubuntu 22.04/24.04 on amd64 and arm64, each pinned to its distro's stock nginx, so there is nothing to compile.
- **Cyclone cache.** The file-based cache from the original was replaced with Cyclone, the same variant-aware, memory-mapped cache that powers 2.0. Faster lookups, proper LRU eviction, and shared cache format between 1.15 and 2.0.
- **Unified licensing and admin console.** A web-based admin console for cache management, statistics, and license activation. Ed25519 token-based licensing with auto-renewal.

**Best for:** Teams that already run mod_pagespeed or ngx_pagespeed, want a maintained version, and prefer a native server module over a reverse proxy. Apache users in particular: 1.15 integrates directly into Apache's output filter chain exactly like the original.

### ModPageSpeed 2.0: the out-of-process worker behind a reverse proxy

ModPageSpeed 2.0 keeps the optimization libraries from the original (the image codecs, the CSS minifier, the JavaScript minifier, the HTML parser) but replaces everything above them. RewriteDriver, the filter pipeline, the resource manager, the cache coordination: all rebuilt in C++23.

The new architecture separates the system into three components:

**The interceptor.** A dynamic nginx component (no recompilation required) that classifies each request into a 32-bit capability mask encoding image format support (WebP, AVIF), viewport class, pixel density, Save-Data preference, and transfer encoding. It checks the Cyclone cache for a matching variant and serves it via `mmap`. Zero-copy, no allocations, no processing in the request path.

**The worker.** A separate C++ process that does the actual optimization. When nginx records a cache miss, it sends a fire-and-forget notification over a Unix socket. The worker reads the original content, runs the appropriate optimization, and writes the optimized variant back to the cache.

**Cyclone cache.** The same variant-aware disk cache used in 1.15, shared between the interceptor and worker via memory-mapped I/O. Stores multiple variants of the same resource under a single URL key, each identified by its capability mask. Lookups use best-fit fallback: if no exact match exists, the cache degrades gracefully to the closest available variant.

The practical effect: the first request gets the original content (`X-PageSpeed: MISS`). The worker optimizes it in the background. Subsequent requests get the optimized version (`X-PageSpeed: HIT`). Nginx never blocks on optimization work.

**Best for:** New deployments. Teams that want a reverse proxy architecture (Docker Compose in front of any HTTP origin). Sites that benefit from the 32-bit capability mask, which generates up to 37 image variants per source image, automatically matched to each visitor's browser, viewport, density, and data-saving preference. Also available as [ASP.NET Core middleware](/blog/aspnet-core-middleware/) for .NET applications.

## Choosing between 1.15 and 2.0

|                                           | mod_pagespeed 1.15                                   | ModPageSpeed 2.0                                                                     |
| ----------------------------------------- | ---------------------------------------------------- | ------------------------------------------------------------------------------------ |
| **Architecture**                          | Native server module (in-process)                    | Reverse proxy + async worker                                                         |
| **Web servers**                           | Apache, nginx, IIS (Envoy experimental)              | Any HTTP origin (Docker Compose)                                                     |
| **Configuration**                         | `ModPagespeed*` directives (familiar)                | Worker flags + nginx directives (new)                                                |
| **Optimization model**                    | Synchronous, in the request path                     | Asynchronous, background worker                                                      |
| **Image formats**                         | WebP, AVIF, JPEG, PNG, GIF                           | + SVG auto-vectorization, Jpegli, ML-predicted quality                               |
| **Image variants**                        | Format negotiation                                   | 32-bit capability mask (format × viewport × density × Save-Data = up to 37 variants) |
| **Cache**                                 | Cyclone (shared)                                     | Cyclone (shared)                                                                     |
| **Critical CSS**                          | Filter-based extraction                              | Heuristic extraction (sub-5ms, no headless browser)                                  |
| **Classic filters**                       | combine_css/js, image spriting, IPRO, domain mapping | Content-type dispatch (HTTP/2-era set)                                               |
| **Admin UI**                              | Built-in `/pagespeed_admin/`                         | SvelteKit web console + Prometheus                                                   |
| **Native IIS module**                     | Yes (GA, the IISpeed successor)                      | Via reverse proxy in front of IIS                                                    |
| **ASP.NET Core middleware**               | Native module ports instead                          | Yes (NuGet middleware)                                                               |
| **Migration from Google's mod_pagespeed** | Drop-in (same directives)                            | Config mapping required ([migration guide](/blog/migrating-from-1x/))                |
| **Pricing**                               | Per-site flat rate ([see pricing](/pricing/))        | Per-site flat rate ([see pricing](/pricing/))                                        |

**If you are already running mod_pagespeed on Apache** and it works, 1.15 is the native in-process module. It integrates directly into Apache's output filter chain, reads your exact `ModPagespeed*` directives, and is maintained and tested on current distributions.

**If you are running ngx_pagespeed** and you are tired of downloading the matching nginx source tree and recompiling the module on every upgrade, mod_pagespeed 1.15 ships a prebuilt, signed `nginx-module-pagespeed` for Debian 11, 12, and 13 and Ubuntu 22.04 and 24.04, on both amd64 and arm64 — no compiling. AlmaLinux/RHEL/Rocky 9 (x86_64 + aarch64) and 10 (x86_64) are covered via the yum repo. Each module is pinned to its distro's stock nginx; if you run a different nginx version, [contact us](/contact/) for a matching build. See [/download/](/download/).

**If you want a reverse proxy in front of any HTTP origin,** 2.0 runs an async worker outside the request path, so optimization adds no latency to the response. The 32-bit capability mask generates up to 37 variants per image, matched to each visitor's browser, viewport, density, and Save-Data preference.

## Other alternatives

Neither 1.15 nor 2.0 is the only option. Depending on your needs, other tools may be a better fit.

**Image-only optimization.** If your bottleneck is images and nothing else, [imgproxy](https://imgproxy.net/) and [thumbor](https://www.thumbor.org/) are lighter self-hosted options. They handle format conversion and resizing well. They do not touch CSS, JavaScript, or HTML. We compare each directly in [imgproxy vs ModPageSpeed](/vs/imgproxy/) and [thumbor vs ModPageSpeed](/vs/thumbor/).

**CDN-based optimization.** Cloudflare Polish, [Cloudinary](/vs/cloudinary/), and [imgix](/vs/imgix/) optimize images at the edge. They work well for globally distributed audiences and require no server-side setup. The trade-off is per-request pricing that scales linearly with traffic, vendor lock-in via proprietary URL schemes, and routing your content through third-party infrastructure. At high request volumes the monthly transformation and bandwidth bill runs into the thousands and keeps climbing with traffic, versus a flat per-site rate for [self-hosted optimization](/self-hosted-image-optimization/) that covers every server behind the site ([see pricing](/pricing/)). Our [detailed cost comparison](/blog/economics-of-image-optimization/) has the full breakdown.

**Manual optimization.** Build scripts that run imagemin, cssnano, and terser at deploy time. This works for static sites. It does not help with dynamic content, user-uploaded images, or content from a CMS. And it does not adapt to client capabilities. Every visitor gets the same assets regardless of whether their browser supports AVIF or their connection is 3G.

**The gap both products fill:** automatic, full-stack optimization (images + CSS + JS + HTML + critical CSS) that runs on your infrastructure, requires no application code changes, and costs a flat rate regardless of traffic volume.

## Getting started

**ModPageSpeed 2.0** is available now. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/). Deploy with Docker Compose in front of your existing origin:

```bash
docker compose up -d
curl -I http://localhost:8080/
# X-PageSpeed: MISS → first request, original content
# X-PageSpeed: HIT  → subsequent requests, optimized
```

See the [getting started guide](/docs/getting-started/) for full setup instructions, or the [migration guide](/blog/migrating-from-1x/) if you are moving from Google's mod_pagespeed.

**mod_pagespeed 1.15** ships prebuilt packages for Apache (`.deb`/`.rpm`) and a prebuilt, signed `nginx-module-pagespeed` for Debian 11/12/13 and Ubuntu 22.04/24.04 (amd64 + arm64), plus AlmaLinux/RHEL/Rocky 9 (x86_64 + aarch64) and 10 (x86_64) — see [Downloads](https://modpagespeed.com/download/) or [packages.modpagespeed.com](https://packages.modpagespeed.com/) for the signed apt/yum repo.

Both products share the same per-site licensing: one license covers the site whether it runs 1.15 or 2.0, and a Business license covers unlimited servers behind it. See the [pricing page](/pricing/) for current rates.
