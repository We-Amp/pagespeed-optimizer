---
title: 'mod_pagespeed Directive Index (Apache & nginx)'
description: 'Alphabetical reference of every mod_pagespeed 1.15 configuration directive — Apache and nginx syntax, default values, and links to the full caching, image, CSS/JS, and domain docs.'
order: 41
group: 'Reference'
lastUpdated: 2026-07-27
---

Every mod_pagespeed 1.15 directive is listed alphabetically below. In Apache, prefix each directive name with `ModPagespeed` (e.g., `ModPagespeedEnableFilters`). In nginx, prefix with `pagespeed` and terminate with a semicolon (e.g., `pagespeed EnableFilters ...;`).

Use this page for quick lookups. To see how directives fit together into a working setup, read the [configuration reference](/1.1/docs/configuration/); to turn individual optimizations on or off, see [filter selection](/1.1/docs/filter-selection/).

| Directive                               | Default                 | Description                                               | Docs                                                       |
| --------------------------------------- | ----------------------- | --------------------------------------------------------- | ---------------------------------------------------------- |
| `AddResourceHeader`                     | (none)                  | Adds custom header to optimized resources                 | [Configuration](/1.1/docs/configuration/)                  |
| `AdminDomains`                          | Allow \*                | ACL for admin page access                                 | [Admin Console](/1.1/docs/admin-console/)                  |
| `Allow`                                 | (none)                  | Allows optimization of resources matching a URL pattern   | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `AdminPath`                             | (none; IIS: /pagespeed_admin) | URL path for admin pages — unset means no handler on nginx/Apache | [Admin Console](/1.1/docs/admin-console/)                  |
| `AutoCreateCachePath`                   | on                      | Auto-creates the cache directory on first request (IIS)   | [IIS Configuration](/1.1/docs/iis-configuration/)          |
| `AvifAnimatedRecompressionQuality`      | 50                      | Quality for animated AVIF output                          | [Image Filters](/1.1/docs/image-filters/#avif)             |
| `AvifQualityForSaveData`                | 45                      | AVIF quality for clients sending `Save-Data: on`          | [Image Filters](/1.1/docs/image-filters/#avif)             |
| `AvifRecompressionQuality`              | 60                      | AVIF quality level; -1 uses `ImageRecompressionQuality`   | [Image Filters](/1.1/docs/image-filters/#avif)             |
| `AvifRecompressionQualityForSmallScreens` | 50                    | AVIF quality for small-screen clients                     | [Image Filters](/1.1/docs/image-filters/#avif)             |
| `AvifTimeoutMs`                         | 5000                    | Wall-clock budget for one AVIF encode; exceeded encodes are abandoned | [Image Filters](/1.1/docs/image-filters/#avif)  |
| `CacheFlushFilename`                    | cache.flush             | Filename for legacy cache flush                           | [Caching](/1.1/docs/caching/)                              |
| `CacheFlushPollIntervalSec`             | 5                       | Interval to check for flush file                          | [Caching](/1.1/docs/caching/)                              |
| `CacheFragment`                         | (auto)                  | Cache partition key                                       | [Caching](/1.1/docs/caching/)                              |
| `ConsoleDomains`                        | Allow \*                | ACL for console access                                    | [Admin Console](/1.1/docs/admin-console/)                  |
| `ConsolePath`                           | (none; IIS: /pagespeed_console) | URL path for console — unset means no handler on nginx/Apache | [Admin Console](/1.1/docs/admin-console/)                  |
| `CriticalImagesBeaconEnabled`           | on                      | Enables beacon for image optimization                     | [Image Filters](/1.1/docs/image-filters/)                  |
| `CssFlattenMaxBytes`                    | 1024000                 | Max size for flattened CSS                                | [CSS Filters](/1.1/docs/css-filters/)                      |
| `CssImageInlineMaxBytes`                | 0                       | Max image size to inline in CSS                           | [CSS Filters](/1.1/docs/css-filters/)                      |
| `CssInlineMaxBytes`                     | 2048                    | Max CSS file size to inline                               | [CSS Filters](/1.1/docs/css-filters/)                      |
| `CssOutlineMinBytes`                    | 3000                    | Min inline CSS to externalize                             | [CSS Filters](/1.1/docs/css-filters/)                      |
| `CycloneRamCacheKb`                     | 0 (r18+)                | Cyclone RAM tier size (0 = off, -1 = legacy LRU coupling) | [Caching](/1.1/docs/caching/)                              |
| `CycloneZeroCopy`                       | off                     | Opt-in (r19+): read cached hits directly from the mapped cache file, no copy; the switch that opts a server into zero-copy serving | [Caching](/1.1/docs/caching/)                              |
| `CycloneZeroCopyServe`                  | on (needs `CycloneZeroCopy`) | Serve cached hits by reference (aliased) into the output buffer; engages only once `CycloneZeroCopy` maps values — set explicitly on Apache and IIS | [Caching](/1.1/docs/caching/)                              |
| `DefaultSharedMemoryCacheKB`            | 51200                   | Shared memory metadata cache size                         | [Caching](/1.1/docs/caching/)                              |
| `Disallow`                              | (none)                  | Prevents optimization of resources matching a URL pattern | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `DisableFilters`                        | (none)                  | Disables specific filters                                 | [Filter Selection](/1.1/docs/filter-selection/)            |
| `DisableRewriteOnNoTransform`           | on                      | Honors Cache-Control: no-transform                        | [Configuration](/1.1/docs/configuration/)                  |
| `Domain`                                | (none)                  | Authorizes a domain for resource fetching                 | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `EnableCachePurge`                      | off                     | Enables per-URL cache purge                               | [Caching](/1.1/docs/caching/)                              |
| `EnableFilters`                         | (none)                  | Enables specific filters                                  | [Filter Selection](/1.1/docs/filter-selection/)            |
| `FetcherTimeoutMs`                      | 5000                    | Timeout for resource fetches                              | [Caching](/1.1/docs/caching/)                              |
| `FetchHttps`                            | enable                  | Controls HTTPS resource fetching                          | [HTTPS Configuration](/1.1/docs/https-configuration/)      |
| `FetchWithGzip`                         | off                     | Fetches resources with gzip Accept-Encoding               | [Caching](/1.1/docs/caching/)                              |
| `FileCachePath`                         | (required)              | Path for file-based cache storage                         | [Caching](/1.1/docs/caching/)                              |
| `FileCacheSizeKb`                       | 1048576 (r18+)          | Maximum file cache size in KB                             | [Caching](/1.1/docs/caching/)                              |
| `FileCacheSmallTierPercent`             | 10 (r18+)               | Percent of cache reserved for metadata/page data (0=off)  | [Caching](/1.1/docs/caching/)                              |
| `ForbidAllDisabledFilters`              | off                     | Forbids all non-enabled filters                           | [Filter Selection](/1.1/docs/filter-selection/)            |
| `ForbidFilters`                         | (none)                  | Permanently disables specific filters                     | [Filter Selection](/1.1/docs/filter-selection/)            |
| `GlobalAdminDomains`                    | Allow \*                | ACL for global admin access                               | [Admin Console](/1.1/docs/admin-console/)                  |
| `GlobalStatisticsDomains`               | Allow \*                | ACL for global statistics access                          | [Admin Console](/1.1/docs/admin-console/)                  |
| `GlobalAdminPath`                       | (none; IIS: /pagespeed_global_admin) | URL path for global admin — unset means no handler on nginx/Apache | [Admin Console](/1.1/docs/admin-console/)                  |
| `GlobalStatisticsPath`                  | (nginx path)            | URL for global stats (nginx)                              | [Admin Console](/1.1/docs/admin-console/)                  |
| `HonorCsp`                              | on                      | Respects Content-Security-Policy headers and meta tags    | [Configuration](/1.1/docs/configuration/)                  |
| `HttpCacheCompressionLevel`             | 9                       | Compression level for cached HTTP responses               | [Caching](/1.1/docs/caching/)                              |
| `ImageInlineMaxBytes`                   | 3072                    | Max image size to inline as data URI                      | [Image Filters](/1.1/docs/image-filters/)                  |
| `ImageLimitOptimizedPercent`            | 100                     | Only serve if optimized is smaller                        | [Image Filters](/1.1/docs/image-filters/)                  |
| `ImageLimitResizeAreaPercent`           | 100                     | Only resize if result area is this percent of original    | [Image Filters](/1.1/docs/image-filters/)                  |
| `ImageMaxRewritesAtOnce`                | 8                       | Parallel image optimization limit                         | [Image Filters](/1.1/docs/image-filters/)                  |
| `ImageRecompressionQuality`             | 85                      | Image recompression quality                               | [Image Filters](/1.1/docs/image-filters/)                  |
| `ImageResolutionLimitBytes`             | 33554432                | Max image size to optimize                                | [Image Filters](/1.1/docs/image-filters/)                  |
| `ImplicitCacheTtlMs`                    | 300000                  | Cache TTL for resources without explicit headers          | [Caching](/1.1/docs/caching/)                              |
| `InfoUrlsLocalOnly`                     | on                      | Restricts admin/info URLs to the local machine (IIS)      | [Admin Console](/1.1/docs/admin-console/)                  |
| `InPlaceResourceOptimization`           | on                      | Enables IPRO                                              | [Caching](/1.1/docs/caching/)                              |
| `InPlaceRewriteDeadlineMs`              | 10                      | IPRO rewrite deadline per flush                           | [Caching](/1.1/docs/caching/)                              |
| `InPlaceSMaxAgeSec`                     | 10                      | s-maxage for IPRO responses                               | [Caching](/1.1/docs/caching/)                              |
| `JpegRecompressionQuality`              | -1                      | JPEG quality (-1 = use general)                           | [Image Filters](/1.1/docs/image-filters/)                  |
| `JsInlineMaxBytes`                      | 2048                    | Max JS file size to inline                                | [JavaScript Filters](/1.1/docs/javascript-filters/)        |
| `JsOutlineMinBytes`                     | 3000                    | Min inline JS to externalize                              | [JavaScript Filters](/1.1/docs/javascript-filters/)        |
| `LazyloadImagesMode`                    | auto (r18+)             | Lazy-loading mechanism: auto, native, or js               | [Image Filters](/1.1/docs/image-filters/)                  |
| `LazyloadImagesSkipFirst`               | 1 (r18+)                | Images at page start never lazy-loaded without fold data  | [Image Filters](/1.1/docs/image-filters/)                  |
| `ListOutstandingUrlsOnError`            | off                     | Shows fetch URLs in error responses                       | [Configuration](/1.1/docs/configuration/)                  |
| `LoadFromFile`                          | (none)                  | Loads resources from filesystem                           | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `LoadFromFileCacheTtlMs`                | (ImplicitCacheTtlMs)    | Cache TTL for filesystem-loaded resources                 | [Caching](/1.1/docs/caching/)                              |
| `LogDir`                                | (none)                  | Directory for statistics logging                          | [Admin Console](/1.1/docs/admin-console/)                  |
| `LowercaseHtmlNames`                    | off                     | Lowercases HTML tag/attribute names                       | [Configuration](/1.1/docs/configuration/)                  |
| `LRUCacheByteLimit`                     | 0                       | Legacy; no effect with the Cyclone backend                | [Caching](/1.1/docs/caching/)                              |
| `LRUCacheKbPerProcess`                  | 0                       | Legacy; sizes RAM tier when CycloneRamCacheKb is -1       | [Caching](/1.1/docs/caching/)                              |
| `MapOriginDomain`                       | (none)                  | Maps public domain to origin for fetching                 | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `MapProxyDomain`                        | (none)                  | Proxies and optimizes content from a separate origin      | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `MapRewriteDomain`                      | (none)                  | Rewrites resource URLs to another domain                  | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `MaxSegmentLength`                      | 1024                    | Max length of a generated URL segment                     | [Configuration](/1.1/docs/configuration/#max-url-segments) |
| `MemcachedServers`                      | (none)                  | Memcached server list                                     | [Caching](/1.1/docs/caching/)                              |
| `MemcachedTimeoutUs`                    | 500000                  | Memcached operation timeout                               | [Caching](/1.1/docs/caching/)                              |
| `MessageBufferSize`                     | 0                       | Size of message history buffer                            | [Admin Console](/1.1/docs/admin-console/)                  |
| `MessagesDomains`                       | Allow \*                | ACL for message history                                   | [Admin Console](/1.1/docs/admin-console/)                  |
| `MessagesPath`                          | (nginx path)            | URL for messages (nginx)                                  | [Admin Console](/1.1/docs/admin-console/)                  |
| `ModifyCachingHeaders`                  | on                      | Sets caching headers on HTML                              | [Configuration](/1.1/docs/configuration/)                  |
| `NumExpensiveRewriteThreads`            | auto                    | Threads for heavy optimization work (image transcoding); global, not per-vhost | [Configuration](/1.1/docs/configuration/#optimization-threads) |
| `NumRewriteThreads`                     | auto                    | Threads for short, latency-sensitive optimization work; global, not per-vhost  | [Configuration](/1.1/docs/configuration/#optimization-threads) |
| `NativeFetcherMaxKeepaliveRequests`     | 100                     | Max requests per keepalive connection for the native fetcher (nginx, http block only) | [HTTPS Configuration](/1.1/docs/https-configuration/)      |
| `PreserveUrlRelativity`                 | on                      | Preserves relative URLs                                   | [Configuration](/1.1/docs/configuration/)                  |
| `PurgeMethod`                           | (none)                  | HTTP method for cache purge                               | [Caching](/1.1/docs/caching/)                              |
| `RateLimitBackgroundFetches`            | on                      | Rate-limits background fetches                            | [Caching](/1.1/docs/caching/)                              |
| `RedisDatabaseIndex`                    | 0                       | Redis database index                                      | [Caching](/1.1/docs/caching/)                              |
| `RedisReconnectionDelayMs`              | 1000                    | Redis reconnection delay                                  | [Caching](/1.1/docs/caching/)                              |
| `RedisServer`                           | (none)                  | Redis server address                                      | [Caching](/1.1/docs/caching/)                              |
| `RedisTimeoutUs`                        | 50000                   | Redis operation timeout                                   | [Caching](/1.1/docs/caching/)                              |
| `RedisTTLSec`                           | -1                      | Redis entry TTL                                           | [Caching](/1.1/docs/caching/)                              |
| `RespectVary`                           | off                     | Respects Vary headers on resources                        | [Configuration](/1.1/docs/configuration/)                  |
| `ResponsiveImageDensities`              | 1.5,2,3                 | Pixel densities generated for `responsive_images`         | [Image Filters](/1.1/docs/image-filters/)                  |
| `RetainComment`                         | (none)                  | Wildcard pattern for comments to keep                     | [HTML Filters](/1.1/docs/html-filters/)                    |
| `RewriteDeadlinePerFlushMs`             | 10                      | Rewrite timeout per HTML flush                            | [Caching](/1.1/docs/caching/)                              |
| `RewriteLevel`                          | CoreFilters             | Baseline filter set                                       | [Filter Selection](/1.1/docs/filter-selection/)            |
| `RewriteRandomDropPercentage`           | 0                       | Random percentage of rewrites to skip                     | [Filter Selection](/1.1/docs/filter-selection/)            |
| `ShardDomain`                           | (none)                  | Distributes resources across domains                      | [Domain Configuration](/1.1/docs/domain-configuration/)    |
| `ShmMetadataCacheCheckpointIntervalSec` | 300                     | No-op; superseded by write-through (v1.15.0+r17)          | [Caching](/1.1/docs/caching/)                              |
| `SslCertDirectory`                      | (none)                  | SSL certificate directory                                 | [HTTPS Configuration](/1.1/docs/https-configuration/)      |
| `SslCertFile`                           | (none)                  | SSL certificate file                                      | [HTTPS Configuration](/1.1/docs/https-configuration/)      |
| `StaticAssetPrefix`                     | /pagespeed_static/      | URL prefix for static assets                              | [Configuration](/1.1/docs/configuration/)                  |
| `Statistics`                            | on                      | Enables statistics collection                             | [Admin Console](/1.1/docs/admin-console/)                  |
| `StatisticsDomains`                     | Allow \*                | ACL for statistics access                                 | [Admin Console](/1.1/docs/admin-console/)                  |
| `StatisticsLogging`                     | off                     | Enables logging for console graphs                        | [Admin Console](/1.1/docs/admin-console/)                  |
| `StatisticsPath`                        | (nginx path)            | URL for statistics (nginx)                                | [Admin Console](/1.1/docs/admin-console/)                  |
| `SupportNoScriptEnabled`                | on                      | Inserts noscript redirect                                 | [Troubleshooting](/1.1/docs/troubleshooting/)              |
| `UseExperimentalJsMinifier`             | (ignored)               | Deprecated since v1.15.0+r21: accepted for compatibility but ignored — logs a deprecation warning at configuration load | [JavaScript Filters](/1.1/docs/javascript-filters/#rewrite_javascript) |
| `UseNativeFetcher`                      | off                     | Uses nginx's event-driven fetcher for resource fetches, HTTP and HTTPS (nginx, http block only) | [HTTPS Configuration](/1.1/docs/https-configuration/)      |
| `UsePerVhostStatistics`                 | off                     | Per-virtual-host statistics                               | [Admin Console](/1.1/docs/admin-console/)                  |
| `WebpAnimatedRecompressionQuality`      | 70                      | Quality for animated WebP conversion                      | [Image Filters](/1.1/docs/image-filters/)                  |
| `WebpRecompressionQuality`              | 80                      | WebP quality level                                        | [Image Filters](/1.1/docs/image-filters/)                  |
| `XHeaderValue`                          | (version)               | X-Mod-Pagespeed/X-Page-Speed header value                 | [Configuration](/1.1/docs/configuration/)                  |

## See also

- [Configuration](/1.1/docs/configuration/) — general configuration reference
- [Filter Selection](/1.1/docs/filter-selection/) — filter enable/disable directives
- [PageSpeed markers reference](/pagespeed-markers/) — the attributes and headers these directives produce at runtime
