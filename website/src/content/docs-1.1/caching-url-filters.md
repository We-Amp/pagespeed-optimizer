---
title: 'Caching & URL Filters'
description: 'Cache-extension and URL-management filters in mod_pagespeed 1.15: extend_cache for 1-year content-hashed caching, extend_cache_pdfs, rewrite_domains for CDN and sharding, and local_storage_cache.'
order: 25
group: 'Filters'
lastUpdated: 2026-07-12
---

## Overview

These filters extend browser cache lifetimes and manage resource URLs across domains. Browsers cache resources for a year, but pick up updated content the moment a resource changes, because the URL carries a content hash. The `extend_cache` CoreFilter drives this: it sets a 1-year cache lifetime on CSS, JS, and image URLs.

## Quick reference

| Filter                                        | Core | On-by-default | Description                                      | Safe         |
| --------------------------------------------- | ---- | ------------- | ------------------------------------------------ | ------------ |
| [`extend_cache`](#extend_cache)               | Yes  | Yes           | Content-hashed URLs with 1-year TTL              | Yes          |
| [`extend_cache_pdfs`](#extend_cache_pdfs)     | No   | No            | Same cache extension for PDF links               | Yes          |
| [`rewrite_domains`](#rewrite_domains)         | No   | No            | Applies domain mappings to resource URLs         | Test first   |
| [`local_storage_cache`](#local_storage_cache) | No   | No            | localStorage-based caching for inlined resources | Experimental |

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```
pagespeed EnableFilters extend_cache
```

See [IIS Configuration](/1.1/docs/iis-configuration/) for the full file format reference.

## extend_cache {#extend_cache}

**Core filter.** Rewrites resource URLs (CSS, JS, images) to include a content hash, then serves the optimized resource with a 1-year `Cache-Control: max-age` header. When the original resource changes, the hash changes, generating a new URL that bypasses the browser cache.

Resources that previously had short or no cache lifetimes gain a 1-year cache lifetime. Because the URL carries the content hash, a changed resource gets a new URL and is never served stale.

The sub-filters `extend_cache_css`, `extend_cache_images`, and `extend_cache_scripts` are included when you enable `extend_cache`, which turns on all three. Each can also be enabled individually with `EnableFilters` (for example, `extend_cache_images` alone).

## extend_cache_pdfs {#extend_cache_pdfs}

**Not a CoreFilter.** Applies the same content-hash-based cache extension to PDF file links. Enable this filter if your site serves PDFs that change infrequently.

```apache
ModPagespeedEnableFilters extend_cache_pdfs
```

## rewrite_domains {#rewrite_domains}

**Not a CoreFilter. Test before deploying.** Rewrites resource URLs to use domains specified by `MapRewriteDomain` or `ShardDomain` directives. Useful for CDN integration or domain sharding.

```apache
ModPagespeedEnableFilters rewrite_domains
```

This filter only affects resources that mod_pagespeed does not otherwise optimize. Resources already rewritten by other filters ([`rewrite_css`](/1.1/docs/css-filters/#rewrite_css), [`rewrite_images`](/1.1/docs/image-filters/#rewrite_images), etc.) already have their domains set by those filters.

See [Domain Configuration](/1.1/docs/domain-configuration/) for `MapRewriteDomain` and `ShardDomain` setup.

## local_storage_cache {#local_storage_cache}

**Experimental.** Stores inlined CSS and JavaScript in the browser's `localStorage` on first visit, then loads from `localStorage` on subsequent visits instead of re-inlining. This reduces HTML payload on repeat views at the cost of JavaScript complexity and reliance on `localStorage` availability.

```apache
ModPagespeedEnableFilters local_storage_cache
```

Not recommended for most deployments. Browser `localStorage` has size limits (typically 5-10 MB per origin) and can be cleared by the user at any time. Sites with many inlined resources may exceed these limits. Since v1.15.0+r18, with `HonorCsp` (default on) the filter stands down on pages whose `Content-Security-Policy` disallows the inline script it relies on.

## See also

- [Caching](/1.1/docs/caching/) -- server-side caching configuration (Cyclone Cache, memcached, Redis)
- [Filter Selection](/1.1/docs/filter-selection/)
- [Filter Reference](/1.1/docs/filter-reference/)
