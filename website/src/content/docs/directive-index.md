---
title: 'mod_pagespeed Directive Index (Apache & nginx)'
description: 'Alphabetical reference of every mod_pagespeed 2.1 configuration directive — Apache and nginx syntax, default values, and links to the full caching, image, CSS/JS, and domain docs.'
order: 73
group: 'Reference'
lastUpdated: 2026-07-27
---

Every mod_pagespeed 2.1 directive is listed alphabetically below. In Apache, prefix each directive name with `ModPagespeed` (e.g., `ModPagespeedEnableFilters`). In nginx, prefix with `pagespeed` and terminate with a semicolon (e.g., `pagespeed EnableFilters ...;`).

Use this page for quick lookups. To see how directives fit together into a working setup, read the [configuration reference](/docs/configuration/); to turn individual optimizations on or off, see [filter selection](/docs/filter-selection/).

| Directive                               | Default                 | Description                                               | Docs                                                       |
| --------------------------------------- | ----------------------- | --------------------------------------------------------- | ---------------------------------------------------------- |
| `AddResourceHeader`                     | (none)                  | Adds custom header to optimized resources                 | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `AdminDomains`                          | Allow \*                | ACL for admin page access                                 | [Admin Console](/docs/admin-console/)                  |
| `Allow`                                 | (none)                  | Allows optimization of resources matching a URL pattern   | [Domain Configuration](/docs/domain-configuration/)    |
| `AdminPath`                             | (none; IIS: /pagespeed_admin) | URL path for admin pages — unset means no handler on nginx/Apache | [Admin Console](/docs/admin-console/)                  |
| `AutoCreateCachePath`                   | on                      | Auto-creates the cache directory on first request (IIS)   | [IIS Configuration](/docs/iis-configuration/)          |
| `AvifAnimatedRecompressionQuality`      | 50                      | Quality for animated AVIF output                          | [Image Filters](/docs/image-filters/#avif)             |
| `AvifQualityForSaveData`                | 45                      | AVIF quality for clients sending `Save-Data: on`          | [Image Filters](/docs/image-filters/#avif)             |
| `AvifRecompressionQuality`              | 60                      | AVIF quality level; -1 uses `ImageRecompressionQuality`   | [Image Filters](/docs/image-filters/#avif)             |
| `AvifRecompressionQualityForSmallScreens` | 50                    | AVIF quality for small-screen clients                     | [Image Filters](/docs/image-filters/#avif)             |
| `AvifTimeoutMs`                         | 5000                    | Wall-clock budget for one AVIF encode; exceeded encodes are abandoned | [Image Filters](/docs/image-filters/#avif)  |
| `CacheFlushFilename`                    | cache.flush             | Filename for legacy cache flush                           | [Caching](/docs/cache-modes/#flush-the-entire-cache)       |
| `CacheFlushPollIntervalSec`             | 5                       | Interval to check for flush file                          | [Caching](/docs/cache-modes/#flush-the-entire-cache)       |
| `CacheFragment`                         | (auto)                  | Cache partition key                                       | [Caching](/docs/cache-modes/#additional-cache-directives)  |
| `ConsoleDomains`                        | Allow \*                | ACL for console access                                    | [Admin Console](/docs/admin-console/)                  |
| `ConsolePath`                           | (none; IIS: /pagespeed_console) | URL path for console — unset means no handler on nginx/Apache | [Admin Console](/docs/admin-console/)                  |
| `CriticalImagesBeaconEnabled`           | on                      | Enables beacon for image optimization                     | [Image Filters](/docs/image-filters/)                  |
| `CssFlattenMaxBytes`                    | 1024000                 | Max size for flattened CSS                                | [CSS Filters](/docs/css-filters/)                      |
| `CssImageInlineMaxBytes`                | 0                       | Max image size to inline in CSS                           | [CSS Filters](/docs/css-filters/)                      |
| `CssInlineMaxBytes`                     | 2048                    | Max CSS file size to inline                               | [CSS Filters](/docs/css-filters/)                      |
| `CssOutlineMinBytes`                    | 3000                    | Min inline CSS to externalize                             | [CSS Filters](/docs/css-filters/)                      |
| `CycloneRamCacheKb`                     | 0 (r18+)                | Cyclone RAM tier size (0 = off, -1 = legacy LRU coupling) | [Caching](/docs/cache-modes/#ram-cache-tier)               |
| `CycloneZeroCopy`                       | off                     | Opt-in (r19+): read cached hits directly from the mapped cache file, no copy; the switch that opts a server into zero-copy serving | [Caching](/docs/cache-modes/#zero-copy-serving)            |
| `CycloneZeroCopyServe`                  | on (needs `CycloneZeroCopy`) | Serve cached hits by reference (aliased) into the output buffer; engages only once `CycloneZeroCopy` maps values — set explicitly on Apache and IIS | [Caching](/docs/cache-modes/#zero-copy-serving)            |
| `DefaultSharedMemoryCacheKB`            | 51200                   | Shared memory metadata cache size                         | [Caching](/docs/cache-modes/#shared-memory-metadata-cache) |
| `Disallow`                              | (none)                  | Prevents optimization of resources matching a URL pattern | [Domain Configuration](/docs/domain-configuration/)    |
| `DisableFilters`                        | (none)                  | Disables specific filters                                 | [Filter Selection](/docs/filter-selection/)            |
| `DisableRewriteOnNoTransform`           | on                      | Honors Cache-Control: no-transform                        | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `Domain`                                | (none)                  | Authorizes a domain for resource fetching                 | [Domain Configuration](/docs/domain-configuration/)    |
| `EnableCachePurge`                      | off                     | Enables per-URL cache purge                               | [Caching](/docs/cache-modes/#purge-via-http-purge-method)  |
| `EnableFilters`                         | (none)                  | Enables specific filters                                  | [Filter Selection](/docs/filter-selection/)            |
| `FetcherTimeoutMs`                      | 5000                    | Timeout for resource fetches                              | [Caching](/docs/cache-modes/#fetch-and-performance)        |
| `FetchHttps`                            | enable                  | Controls HTTPS resource fetching                          | [HTTPS Configuration](/docs/https-configuration/)      |
| `FetchWithGzip`                         | off                     | Fetches resources with gzip Accept-Encoding               | [Caching](/docs/cache-modes/#fetch-and-performance)        |
| `FileCachePath`                         | (required)              | Path for file-based cache storage                         | [Caching](/docs/cache-modes/#native-module-file-cache)     |
| `FileCacheSizeKb`                       | 1048576 (r18+)          | Maximum file cache size in KB                             | [Caching](/docs/cache-modes/#native-module-file-cache)     |
| `FileCacheSmallTierPercent`             | 10 (r18+)               | Percent of cache reserved for metadata/page data (0=off)  | [Caching](/docs/cache-modes/#shared-memory-metadata-cache) |
| `ForbidAllDisabledFilters`              | off                     | Forbids all non-enabled filters                           | [Filter Selection](/docs/filter-selection/)            |
| `ForbidFilters`                         | (none)                  | Permanently disables specific filters                     | [Filter Selection](/docs/filter-selection/)            |
| `GlobalAdminDomains`                    | Allow \*                | ACL for global admin access                               | [Admin Console](/docs/admin-console/)                  |
| `GlobalStatisticsDomains`               | Allow \*                | ACL for global statistics access                          | [Admin Console](/docs/admin-console/)                  |
| `GlobalAdminPath`                       | (none; IIS: /pagespeed_global_admin) | URL path for global admin — unset means no handler on nginx/Apache | [Admin Console](/docs/admin-console/)                  |
| `GlobalStatisticsPath`                  | (nginx path)            | URL for global stats (nginx)                              | [Admin Console](/docs/admin-console/)                  |
| `HonorCsp`                              | on                      | Respects Content-Security-Policy headers and meta tags    | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `HttpCacheCompressionLevel`             | 9                       | Compression level for cached HTTP responses               | [Caching](/docs/cache-modes/#fetch-and-performance)        |
| `ImageInlineMaxBytes`                   | 3072                    | Max image size to inline as data URI                      | [Image Filters](/docs/image-filters/)                  |
| `ImageLimitOptimizedPercent`            | 100                     | Only serve if optimized is smaller                        | [Image Filters](/docs/image-filters/)                  |
| `ImageLimitResizeAreaPercent`           | 100                     | Only resize if result area is this percent of original    | [Image Filters](/docs/image-filters/)                  |
| `ImageMaxRewritesAtOnce`                | 8                       | Parallel image optimization limit                         | [Image Filters](/docs/image-filters/)                  |
| `ImageRecompressionQuality`             | 85                      | Image recompression quality                               | [Image Filters](/docs/image-filters/)                  |
| `ImageResolutionLimitBytes`             | 33554432                | Max image size to optimize                                | [Image Filters](/docs/image-filters/)                  |
| `ImplicitCacheTtlMs`                    | 300000                  | Cache TTL for resources without explicit headers          | [Caching](/docs/cache-modes/#fetch-and-performance)        |
| `InfoUrlsLocalOnly`                     | on                      | Restricts admin/info URLs to the local machine (IIS)      | [Admin Console](/docs/admin-console/)                  |
| `InPlaceResourceOptimization`           | on                      | Enables IPRO                                              | [Caching](/docs/cache-modes/#in-place-resource-optimization-ipro-considerations) |
| `InPlaceRewriteDeadlineMs`              | 10                      | IPRO rewrite deadline per flush                           | [Caching](/docs/cache-modes/#additional-cache-directives)  |
| `InPlaceSMaxAgeSec`                     | 10                      | s-maxage for IPRO responses                               | [Caching](/docs/cache-modes/#in-place-resource-optimization-ipro-considerations) |
| `JpegRecompressionQuality`              | -1                      | JPEG quality (-1 = use general)                           | [Image Filters](/docs/image-filters/)                  |
| `JsInlineMaxBytes`                      | 2048                    | Max JS file size to inline                                | [JavaScript Filters](/docs/javascript-filters/)        |
| `JsOutlineMinBytes`                     | 3000                    | Min inline JS to externalize                              | [JavaScript Filters](/docs/javascript-filters/)        |
| `LazyloadImagesMode`                    | auto (r18+)             | Lazy-loading mechanism: auto, native, or js               | [Image Filters](/docs/image-filters/)                  |
| `LazyloadImagesSkipFirst`               | 1 (r18+)                | Images at page start never lazy-loaded without fold data  | [Image Filters](/docs/image-filters/)                  |
| `ListOutstandingUrlsOnError`            | off                     | Shows fetch URLs in error responses                       | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `LoadFromFile`                          | (none)                  | Loads resources from filesystem                           | [Domain Configuration](/docs/domain-configuration/)    |
| `LoadFromFileCacheTtlMs`                | (ImplicitCacheTtlMs)    | Cache TTL for filesystem-loaded resources                 | [Troubleshooting](/docs/troubleshooting/#mod_pagespeed-is-not-picking-up-file-changes) |
| `LogDir`                                | (none)                  | Directory for statistics logging                          | [Admin Console](/docs/admin-console/)                  |
| `LowercaseHtmlNames`                    | off                     | Lowercases HTML tag/attribute names                       | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `LRUCacheByteLimit`                     | 0                       | Legacy; no effect with the Cyclone backend                | [Caching](/docs/cache-modes/#legacy-lru-directives)        |
| `LRUCacheKbPerProcess`                  | 0                       | Legacy; sizes RAM tier when CycloneRamCacheKb is -1       | [Caching](/docs/cache-modes/#legacy-lru-directives)        |
| `MapOriginDomain`                       | (none)                  | Maps public domain to origin for fetching                 | [Domain Configuration](/docs/domain-configuration/)    |
| `MapProxyDomain`                        | (none)                  | Proxies and optimizes content from a separate origin      | [Domain Configuration](/docs/domain-configuration/)    |
| `MapRewriteDomain`                      | (none)                  | Rewrites resource URLs to another domain                  | [Domain Configuration](/docs/domain-configuration/)    |
| `MaxSegmentLength`                      | 1024                    | Max length of a generated URL segment                     | [Configuration](/docs/configuration/#max-url-segments)     |
| `MemcachedServers`                      | (none)                  | Memcached server list                                     | [Caching](/docs/cache-modes/#memcached)                    |
| `MemcachedTimeoutUs`                    | 500000                  | Memcached operation timeout                               | [Caching](/docs/cache-modes/#memcached)                    |
| `MessageBufferSize`                     | 0                       | Size of message history buffer                            | [Admin Console](/docs/admin-console/)                  |
| `MessagesDomains`                       | Allow \*                | ACL for message history                                   | [Admin Console](/docs/admin-console/)                  |
| `MessagesPath`                          | (nginx path)            | URL for messages (nginx)                                  | [Admin Console](/docs/admin-console/)                  |
| `ModifyCachingHeaders`                  | on                      | Sets caching headers on HTML                              | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `NumExpensiveRewriteThreads`            | auto                    | Threads for heavy optimization work (image transcoding); global, not per-vhost | [Configuration](/docs/configuration/#optimization-threads) |
| `NumRewriteThreads`                     | auto                    | Threads for short, latency-sensitive optimization work; global, not per-vhost  | [Configuration](/docs/configuration/#optimization-threads) |
| `NativeFetcherMaxKeepaliveRequests`     | 100                     | Max requests per keepalive connection for the native fetcher (nginx, http block only) | [HTTPS Configuration](/docs/https-configuration/)      |
| `PreserveUrlRelativity`                 | on                      | Preserves relative URLs                                   | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `PurgeMethod`                           | (none)                  | HTTP method for cache purge                               | [Caching](/docs/cache-modes/#additional-cache-directives)  |
| `RateLimitBackgroundFetches`            | on                      | Rate-limits background fetches                            | [Caching](/docs/cache-modes/#additional-cache-directives)  |
| `RedisDatabaseIndex`                    | 0                       | Redis database index                                      | [Caching](/docs/cache-modes/#redis)                        |
| `RedisReconnectionDelayMs`              | 1000                    | Redis reconnection delay                                  | [Caching](/docs/cache-modes/#additional-cache-directives)  |
| `RedisServer`                           | (none)                  | Redis server address                                      | [Caching](/docs/cache-modes/#redis)                        |
| `RedisTimeoutUs`                        | 50000                   | Redis operation timeout                                   | [Caching](/docs/cache-modes/#redis)                        |
| `RedisTTLSec`                           | -1                      | Redis entry TTL                                           | [Caching](/docs/cache-modes/#redis)                        |
| `RespectVary`                           | off                     | Respects Vary headers on resources                        | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `ResponsiveImageDensities`              | 1.5,2,3                 | Pixel densities generated for `responsive_images`         | [Image Filters](/docs/image-filters/)                  |
| `RetainComment`                         | (none)                  | Wildcard pattern for comments to keep                     | [HTML Filters](/docs/html-filters/)                    |
| `RewriteDeadlinePerFlushMs`             | 10                      | Rewrite timeout per HTML flush                            | [Caching](/docs/cache-modes/#fetch-and-performance)        |
| `RewriteLevel`                          | CoreFilters             | Baseline filter set                                       | [Filter Selection](/docs/filter-selection/)            |
| `RewriteRandomDropPercentage`           | 0                       | Random percentage of rewrites to skip                     | [Filter Selection](/docs/filter-selection/)            |
| `ShardDomain`                           | (none)                  | Distributes resources across domains                      | [Domain Configuration](/docs/domain-configuration/)    |
| `ShmMetadataCacheCheckpointIntervalSec` | 300                     | No-op; superseded by write-through (v1.15.0+r17)          | [Caching](/docs/cache-modes/#shared-memory-metadata-cache) |
| `SslCertDirectory`                      | (none)                  | SSL certificate directory                                 | [HTTPS Configuration](/docs/https-configuration/)      |
| `SslCertFile`                           | (none)                  | SSL certificate file                                      | [HTTPS Configuration](/docs/https-configuration/)      |
| `StaticAssetPrefix`                     | /pagespeed_static/      | URL prefix for static assets                              | [Configuration](/docs/configuration/#native-module-configuration-directives) |
| `Statistics`                            | on                      | Enables statistics collection                             | [Admin Console](/docs/admin-console/)                  |
| `StatisticsDomains`                     | Allow \*                | ACL for statistics access                                 | [Admin Console](/docs/admin-console/)                  |
| `StatisticsLogging`                     | off                     | Enables logging for console graphs                        | [Admin Console](/docs/admin-console/)                  |
| `StatisticsPath`                        | (nginx path)            | URL for statistics (nginx)                                | [Admin Console](/docs/admin-console/)                  |
| `SupportNoScriptEnabled`                | on                      | Inserts noscript redirect                                 | [Troubleshooting](/docs/troubleshooting/#beacons-causing-unexpected-post-requests) |
| `UseExperimentalJsMinifier`             | (ignored)               | Deprecated since v1.15.0+r21: accepted for compatibility but ignored — logs a deprecation warning at configuration load | [JavaScript Filters](/docs/javascript-filters/#rewrite_javascript) |
| `UseNativeFetcher`                      | off                     | Uses nginx's event-driven fetcher for resource fetches, HTTP and HTTPS (nginx, http block only) | [HTTPS Configuration](/docs/https-configuration/)      |
| `UsePerVhostStatistics`                 | off                     | Per-virtual-host statistics                               | [Admin Console](/docs/admin-console/)                  |
| `WebpAnimatedRecompressionQuality`      | 70                      | Quality for animated WebP conversion                      | [Image Filters](/docs/image-filters/)                  |
| `WebpRecompressionQuality`              | 80                      | WebP quality level                                        | [Image Filters](/docs/image-filters/)                  |
| `XHeaderValue`                          | (version)               | X-Mod-Pagespeed/X-Page-Speed header value                 | [Configuration](/docs/configuration/#native-module-configuration-directives) |

## See also

- [Configuration](/docs/configuration/) — general configuration reference
- [Filter Selection](/docs/filter-selection/) — filter enable/disable directives
- [PageSpeed markers reference](/pagespeed-markers/) — the attributes and headers these directives produce at runtime
