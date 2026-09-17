---
title: 'Caching: Cyclone, Memcached, Redis'
description: 'Configure caching in mod_pagespeed 1.15. Cyclone Cache (default), Memcached and Redis integration, shared memory metadata, cache purging, and IPRO.'
order: 14
group: 'Configuration'
lastUpdated: 2026-07-18
---

## Cyclone Cache

mod_pagespeed 1.15 replaces the open-source file-based cache with **Cyclone Cache**. This is the default backend — existing configurations keep working unchanged.

Cyclone Cache stores all cached data in a single fixed-size file. This eliminates the runaway disk usage and millions-of-small-files problem of the old cache. For the throughput and latency-tail numbers behind the change, see the [Cyclone vs. file-cache benchmark](/blog/cyclone-cache-vs-file-cache-benchmark/).

Key properties:

- **Fixed-size cache file** — the cache never grows beyond the configured size. When it fills, the least-recently-used entries are evicted automatically.
- **Lock-free reads** — concurrent requests read from the cache without blocking each other.
- **Memory-mapped I/O** — the operating system manages which parts of the cache file are held in RAM. Frequently accessed entries stay in memory without a separate in-process cache layer.
- **Shared across processes** — nginx worker processes and Apache prefork children all share the same cache file. No duplication, no coordination overhead.

### Configuration

The `FileCachePath` directive is accepted for compatibility, but Cyclone Cache uses its own storage location within that directory. Set the path and size:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed FileCachePath /var/cache/pagespeed;
pagespeed FileCacheSizeKb 2097152;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedFileCachePath /var/cache/pagespeed
ModPagespeedFileCacheSizeKb 2097152
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
pagespeed FileCacheSizeKb 2097152
```

The default cache path is `%ProgramData%\We-Amp\PageSpeed\Cache`. The IIS app pool identity must have write access to this directory. The installer configures permissions automatically.

On IIS, the cache file is shared across all worker processes in the same app pool. If multiple app pools serve different sites, each uses its own cache file within its configured `FileCachePath`.

</div>

The default cache size is sufficient for most sites. See [Cache sizing](#cache-sizing) below for guidance on larger deployments.

### Memory usage and monitoring

Because Cyclone Cache is memory-mapped, standard per-process memory metrics can be misleading. Pages of the cache file that a worker has touched are counted in that worker's resident memory (`RSS` in `top`/`htop`, `VmRSS`/`VmHWM` in `/proc`), so a busy server can show workers whose resident size approaches the size of the cache file. This is expected and healthy:

- The cache pages are **shared** — there is one copy in the operating system's page cache, no matter how many workers show it in their RSS. Summing RSS across workers counts that one copy once per worker.
- The pages are **reclaimable** — they are clean, file-backed memory, the first thing the kernel drops under memory pressure, with no writeback cost. They never contribute to out-of-memory conditions the way private allocations do.

To monitor actual per-worker memory consumption on Linux:

- **`RssAnon`** in `/proc/<pid>/status` is the private (anonymous) memory a worker really owns — this is the number to alert on and to compare across releases or configurations.
- **`Pss` / `Pss_File`** in `/proc/<pid>/smaps_rollup` give a deduplicated view if you need a single per-process figure that fairly splits shared pages.
- **Memory pressure** (`/proc/pressure/memory`, or `memory.pressure` in cgroup v2) tells you whether the system is actually short on memory — rising cache RSS with flat pressure means the OS is simply making good use of free RAM.

When setting container or cgroup memory limits, remember that the page cache for the cache file counts against the limit but is evicted automatically as the limit is approached. You do not need to reserve the full cache file size as RAM; size limits for the application's private memory plus your desired hot working set.

On Windows/IIS the same principle applies: shared cache file pages appear in each worker process's working set but exist once in system file cache, and are trimmed automatically under memory pressure.

## RAM cache tier

Cyclone Cache is memory-mapped, so cache hits are served straight from the mapped cache file and the operating system decides which entries stay in RAM. There is no separate per-process LRU cache layer in 1.15.

Cyclone can additionally keep a per-process RAM tier in front of the mapped file, controlled by `CycloneRamCacheKb`:

| Value    | Effect                                                                                         |
| -------- | ---------------------------------------------------------------------------------------------- |
| `0`      | RAM tier off — reads come from the memory-mapped cache file. Default in v1.15.0+r18 and later. |
| positive | RAM tier of that size in KB, per process.                                                      |
| `-1`     | Sizes the RAM tier from `LRUCacheKbPerProcess`. Default before v1.15.0+r18.                    |

Leave the RAM tier off unless a measurement says otherwise: the memory-mapped file already keeps hot entries in RAM through the page cache, so a RAM tier mostly duplicates those bytes — once per worker process.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed CycloneRamCacheKb 8192;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedCycloneRamCacheKb 8192
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed CycloneRamCacheKb 8192
```

</div>

### Legacy LRU directives

`LRUCacheKbPerProcess` and `LRUCacheByteLimit` are accepted for compatibility. With the Cyclone backend, `LRUCacheKbPerProcess` matters in two cases only: it sizes the RAM tier when `CycloneRamCacheKb` is `-1`, and it sizes the in-memory fallback cache when the Cyclone cache file cannot be created (for example, an unwritable cache directory). `LRUCacheByteLimit` has no effect with the Cyclone backend.

### Zero-copy serving

Two related options reduce or skip the per-request copy of cached response bodies. As of v1.15.0+r19, zero-copy serving is **opt-in on all three platforms** while it accrues production soak; on-by-default is planned for a future revision. (The r18 notes described the aliased serve as effective by default on nginx — see the [r19 release notes](/1.1/docs/release-notes/) for the correction.)

**`CycloneZeroCopy`** reads cached hits directly from the memory-mapped cache file without copying the payload. It is off by default on every platform and is the switch that opts a server in.

**`CycloneZeroCopyServe`** then carries those memory-mapped bytes into the server's output buffer by reference (aliased) instead of copying them, with a safe copy-out before a cache eviction could overwrite the bytes. It only engages once `CycloneZeroCopy` maps cache values. On nginx it applies as soon as `CycloneZeroCopy` is on; on Apache and IIS the aliased serve activates only when the option is set explicitly.

To enable zero-copy serving:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed CycloneZeroCopy on;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedCycloneZeroCopy on
ModPagespeedCycloneZeroCopyServe on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed CycloneZeroCopy on
pagespeed CycloneZeroCopyServe on
```

</div>

Not every response is eligible for the aliased serve. A response is served by reference only when it is delivered verbatim — on Apache that means a plain-HTTP/1.x main-request `200` of at least 16&nbsp;KB with no `Range` request and no transforming output filter (`deflate`, TLS, HTTP/2). Anything else — for example a response that must be re-compressed for the client — automatically falls back to the regular copying path, and bytes an output filter holds for a slow client are copied out at that point.

Three [statistics](/1.1/docs/admin-console/) make the behavior observable: `zerocopy_serve_aliased` (responses served by reference), `zerocopy_serve_copied_out` (aliased bytes safely copied out before reuse), and `zerocopy_serve_ineligible` (requests that fell back to copied serving). The first fallback also logs a one-time message explaining why; on Apache that message needs `LogLevel info`, while the statistic is always on.

For the design background, see [zero-copy serving](/blog/memory-mapped-cache-zero-copy-serving/) and the [Cyclone vs. file-cache benchmark](/blog/cyclone-cache-vs-file-cache-benchmark/).

## Shared memory metadata cache

The shared memory metadata cache allows all processes on a server to share small metadata (response headers, cache invalidation records) through a shared memory segment. This avoids each process maintaining its own copy and reduces disk lookups.

| Directive                    | Default |
| ---------------------------- | ------: |
| `DefaultSharedMemoryCacheKB` |   51200 |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed DefaultSharedMemoryCacheKB 100000;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedDefaultSharedMemoryCacheKB 100000
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed DefaultSharedMemoryCacheKB 100000
```

</div>

In v1.15.0+r17 and later, every entry stored in the shared memory cache is also written to the Cyclone cache on disk. Shared memory serves hot reads; Cyclone holds the durable copy. After a restart, mod_pagespeed reads the metadata back from disk, so the server comes back warm instead of re-optimizing everything. Entries remain subject to normal cache capacity and eviction. Earlier 1.15 revisions started with a cold shared memory cache after a restart.

Page properties — beacon-collected data such as critical image and selector information — get the same treatment in v1.15.0+r17 and later: they are written through to Cyclone as they are stored, so they survive restarts and do not have to be re-learned from live traffic.

In v1.15.0+r18 and later, this metadata and page-property data is held in a dedicated area of the cache, kept apart from optimized images and other large responses. Large files churning through the cache can no longer push this smaller, higher-value data out, so the information that lets a server come back warm after a restart stays available even under heavy image traffic. The share of the cache reserved for it is set by `FileCacheSmallTierPercent` (default 10; set it to `0` to turn the reservation off; values are capped at 50, so at most half the cache can be reserved). The reservation engages once the cache is large enough to set aside a dedicated region — roughly 256 MB or more; below that, all entries share the cache as before.

Two virtual hosts that share the same `FileCachePath` also share the same shared memory cache segment.

## External caches

mod_pagespeed 1.15 supports memcached and Redis as external cache backends. These are useful when multiple servers need to share cached resources.

**You cannot use both memcached and Redis for the same virtual host.** Pick one.

### Memcached

Point mod_pagespeed at one or more memcached servers:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MemcachedServers "cache1.example.com:11211,cache2.example.com:11211";
pagespeed MemcachedTimeoutUs 500000;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMemcachedServers "cache1.example.com:11211,cache2.example.com:11211"
ModPagespeedMemcachedTimeoutUs 500000
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MemcachedServers 127.0.0.1:11211
pagespeed MemcachedTimeoutUs 500000
```

On Windows, install memcached as a Windows service:

1. Download the 64-bit memcached binary for Windows
2. Extract and open an elevated command prompt in the directory
3. Run `memcached -d install` to install the service
4. Run `net start memcached` to start the service

The default memory pool is 64 MB. To increase it, modify the service's `ImagePath` in the registry at `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\memcached`:

```
"C:\memcached\memcached.exe" -m 256 -d runservice
```

A file cache (`FileCachePath`) is still required alongside memcached — memcached has size limits per object, so the file cache handles larger resources and cache flushing.

</div>

| Directive            | Default |
| -------------------- | ------: |
| `MemcachedServers`   |  (none) |
| `MemcachedTimeoutUs` |  500000 |

Health checking: if a memcached server exceeds 4 timeouts within 30 seconds, mod_pagespeed stops performing optimization for 30 seconds rather than blocking requests on a slow cache.

### Redis

Point mod_pagespeed at a Redis server:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed RedisServer "cache.example.com:6379";
pagespeed RedisTimeoutUs 50000;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedRedisServer "cache.example.com:6379"
ModPagespeedRedisTimeoutUs 50000
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed RedisServer "cache.example.com:6379"
pagespeed RedisTimeoutUs 50000
```

</div>

| Directive            |        Default |
| -------------------- | -------------: |
| `RedisServer`        |         (none) |
| `RedisTimeoutUs`     |          50000 |
| `RedisDatabaseIndex` |              0 |
| `RedisTTLSec`        | -1 (no expiry) |

`RedisDatabaseIndex` selects a specific Redis database. This directive is incompatible with Redis clustering, which does not support database selection.

`RedisTTLSec` sets a TTL on all cache entries. The default of -1 means entries do not expire in Redis (mod_pagespeed manages its own expiration logic). Set a positive value if you need Redis to reclaim memory independently.

## Cache flushing and purging

### Flush the entire cache

Touch a `cache.flush` file in the cache directory to force mod_pagespeed to discard all cached data:

```bash
sudo touch /var/cache/pagespeed/cache.flush
```

mod_pagespeed checks for this file periodically. All entries cached before the file's modification time are treated as expired.

### Purge via admin page

Send a purge request through the built-in admin interface:

```bash
# Purge everything
curl 'http://example.com/pagespeed_admin/cache?purge=*'

# Purge a specific URL
curl 'http://example.com/pagespeed_admin/cache?purge=http://example.com/style.css'

# Purge with a wildcard
curl 'http://example.com/pagespeed_admin/cache?purge=http://example.com/images/*'
```

### Purge via HTTP PURGE method

When `EnableCachePurge` is on, mod_pagespeed accepts standard HTTP PURGE requests:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed EnableCachePurge on;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedEnableCachePurge on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed EnableCachePurge on
```

</div>

Then purge by URL:

```bash
curl -X PURGE http://example.com/style.css
```

### Multi-server deployments

Cache purge and flush operations apply only to the server that receives the request. In a multi-server deployment, run the purge on every server independently. mod_pagespeed does not propagate purge requests across servers.

## Cache sizing

Size the Cyclone Cache file at 3 to 4 times the total size of your original assets. mod_pagespeed stores variants alongside the originals, so the cache needs space for both.

For example, if your site serves 500 MB of images, CSS, and JavaScript, set the cache to 1.5-2 GB.

In v1.15.0+r18 and later, the default cache size is 1 GB (earlier revisions defaulted to 100 MB). Cache files are sparse, so the larger default raises the ceiling rather than reserving the space up front — disk grows only as content is cached, and Cyclone self-evicts at the target. On hosts where disk is tight, set an explicit lower `FileCacheSizeKb`.

## In-Place Resource Optimization (IPRO)

IPRO optimizes resources served from your origin without changing their URLs. This is useful for resources referenced by third-party code or cached at CDN edge nodes where you cannot change the URL.

IPRO is on by default.

| Directive                     | Default |
| ----------------------------- | ------: |
| `InPlaceResourceOptimization` |      on |
| `InPlaceSMaxAgeSec`           |      10 |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed InPlaceResourceOptimization on;
pagespeed InPlaceSMaxAgeSec 10;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedInPlaceResourceOptimization on
ModPagespeedInPlaceSMaxAgeSec 10
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed InPlaceResourceOptimization on
pagespeed InPlaceSMaxAgeSec 10
```

</div>

`InPlaceSMaxAgeSec` controls how long the optimized resource is considered fresh before mod_pagespeed checks the origin again. Keep this low (the default of 10 seconds is appropriate for most sites) to ensure changes propagate quickly.

Considerations when using IPRO:

- Resources keep their original URLs, so they do not benefit from content-hash-based cache extension. Browser caches and CDNs follow the original `Cache-Control` headers.
- IPRO cannot combine resources (CSS combining, JS combining) because each resource retains its own URL.

## Fetch and performance

These directives control how mod_pagespeed fetches resources from your origin and how long it spends optimizing them per request.

| Directive                   |        Default | Description                                                                                                                                                              |
| --------------------------- | -------------: | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `FetcherTimeoutMs`          |           5000 | Maximum time to wait for a resource fetch from origin                                                                                                                    |
| `RewriteDeadlinePerFlushMs` |             10 | Time budget per flush window for rewriting. If rewrites take longer, the original resource is served and the optimized variant is cached for subsequent requests         |
| `ImplicitCacheTtlMs`        | 300000 (5 min) | Cache TTL applied to resources that do not have explicit `Cache-Control` headers                                                                                         |
| `FetchWithGzip`             |            off | Fetch resources from origin using gzip `Accept-Encoding`. Turn this on if your origin supports gzip and the network between mod_pagespeed and the origin is a bottleneck |
| `HttpCacheCompressionLevel` |              9 | Compression level (-1 to 9; 0 = off) for storing resources in the cache. Level 9 maximizes disk space savings at the cost of slightly more CPU during cache writes; out-of-range values fail configuration load since v1.15.0+r18 |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed FetcherTimeoutMs 10000;
pagespeed RewriteDeadlinePerFlushMs 20;
pagespeed ImplicitCacheTtlMs 600000;
pagespeed FetchWithGzip on;
pagespeed HttpCacheCompressionLevel 6;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedFetcherTimeoutMs 10000
ModPagespeedRewriteDeadlinePerFlushMs 20
ModPagespeedImplicitCacheTtlMs 600000
ModPagespeedFetchWithGzip on
ModPagespeedHttpCacheCompressionLevel 6
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed FetcherTimeoutMs 10000
pagespeed RewriteDeadlinePerFlushMs 20
pagespeed ImplicitCacheTtlMs 600000
pagespeed FetchWithGzip on
pagespeed HttpCacheCompressionLevel 6
```

The 1.15 IIS module uses WinHTTP for resource fetching. WinHTTP handles SSL certificate verification using the Windows certificate store — no `SslCertDirectory` or `SslCertFile` directives are needed.

</div>
