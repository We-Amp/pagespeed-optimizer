---
title: 'Save-Data: serving lighter image variants to bandwidth-conscious users'
description: 'How ModPageSpeed 2.0 reads the Save-Data request header to serve lighter image variants -- a 30-50% bandwidth cut stacked on top of format and viewport optimization.'
date: 2026-02-06
lastUpdated: 2026-07-04
author: 'Otto van der Schaaf'
tags: ['performance', 'images', 'save-data', 'bandwidth']
draft: false
product: '2.0'
---

## What Save-Data is

The `Save-Data` HTTP request header is a signal from the user that they want reduced data usage. When a browser sends `Save-Data: on`, it means the user has explicitly opted into a data-saving mode -- either through a browser setting, an OS-level toggle, or a carrier-provided data saver.

The header was standardized as part of the Client Hints specification and has had broad browser support for years. Chrome, Edge, Opera, Samsung Internet, and most Chromium-based browsers support it. Firefox does not expose the header, though it has its own internal data-saving mechanisms. Safari does not support it either, but iOS users on metered connections can trigger it through third-party browsers.

Save-Data represents an explicit user preference. This is not the system guessing whether the user is on a slow connection. The user has actively said: "I would rather have a smaller page than a sharper image." Respecting that preference is a matter of user agency. The performance win is a side effect.

From a web standards perspective, Save-Data is also relevant to the `Vary` response header. Any server that serves different content based on `Save-Data` should include `Vary: Save-Data` in its response headers. Without that, intermediary caches (CDNs, corporate proxies) may serve the wrong variant to the wrong user. ModPageSpeed 2.0 handles this implicitly. The capability mask is the cache key, and it already includes the Save-Data bit -- the correct variant is always served without relying on `Vary` at the cache layer. The nginx interceptor still emits the appropriate `Vary` header on outgoing responses for downstream cache correctness.

## How Save-Data fits in the capability mask

The `Save-Data` preference occupies a single bit (bit 5) in the 32-bit capability mask. This placement is deliberate. The mask layout groups the highest-impact dimensions in the lowest bits (image format in bits 0-1, viewport in bits 2-3) and places the preference-based dimensions higher. Save-Data at bit 5 sits between pixel density (bit 4) and transfer encoding (bits 6-7), reflecting its role as a user preference that is more significant than encoding selection but less impactful than format or viewport selection.

When a request arrives with `Save-Data: on`, the nginx interceptor sets bit 5, which selects a different cache alternate. A Mobile/WebP/1x/Identity request without Save-Data maps to mask `0x01`. The same request with Save-Data maps to `0x21`. These select two different alternates within the same URL's cache entry -- one encoded at normal quality, the other at reduced quality.

## How ModPageSpeed 2.0 responds

When the nginx interceptor parses incoming request headers, the `Save-Data` value is encoded into bit 5 of the 32-bit capability mask. This means that a user with `Save-Data: on` selects a different cache alternate than an identical user without it, even if everything else -- browser, viewport, network -- is the same. The cache serves a dedicated variant tuned for reduced data consumption.

The worker uses two quality tiers when encoding images. Normal quality is the default, used when Save-Data is off. Save-Data quality kicks in when the header is present.

| Format | Normal Quality                    | Save-Data Quality              |       Reduction |
| :----- | :-------------------------------- | :----------------------------- | --------------: |
| JPEG   | 85                                | 60                             | ~30-40% smaller |
| WebP   | 75                                | 50                             | ~25-35% smaller |
| AVIF   | 25 (lower = better in AVIF scale) | 35 (higher = more compression) | ~20-30% smaller |

These are not arbitrary numbers. Each was chosen by visual quality testing against common image categories -- product photography, editorial content, UI screenshots, and hero banners. The normal quality settings produce images that are visually indistinguishable from the source at typical viewing distances. The Save-Data settings produce images where compression artifacts are detectable on close inspection of fine detail but acceptable for the purpose of browsing -- particularly when the user has explicitly asked for reduced data.

For JPEG, dropping from quality 85 to 60 typically saves 30-40% of the file size. The perceptual quality drop is noticeable in smooth gradients and fine textures but invisible in most product photography and editorial imagery. WebP's quality scale is different from JPEG's, but the same principle applies: quality 50 produces visually acceptable results at significantly reduced file sizes. [AVIF is the most efficient codec](/blog/avif-vs-webp-2026/). Even at quality 35 -- more compressed than the normal quality 25 -- AVIF's psychovisual model keeps the result coherent. Compounded with format and viewport savings, the desktop AVIF Save-Data case reaches a 76% cut versus the original, as the bandwidth table below shows.

## Bandwidth savings in practice

The compound effect of Save-Data quality reduction on top of modern codec compression and viewport-based resizing is substantial. Consider a 200 KB JPEG hero image served to different client configurations:

| Client Configuration          |   Size | Savings vs Original |
| :---------------------------- | -----: | ------------------: |
| Desktop, JPEG, normal         | 170 KB |                 15% |
| Desktop, WebP, normal         | 110 KB |                 45% |
| Desktop, AVIF, normal         |  64 KB |                 68% |
| Desktop, WebP, Save-Data      |  70 KB |                 65% |
| Desktop, AVIF, Save-Data      |  48 KB |                 76% |
| Mobile 480px, WebP, normal    |  18 KB |                 91% |
| Mobile 480px, AVIF, normal    |  10 KB |                 95% |
| Mobile 480px, WebP, Save-Data |  12 KB |                 94% |
| Mobile 480px, AVIF, Save-Data |   8 KB |                 96% |

The Save-Data column shows an additional 30-50% reduction compared to the same format at normal quality. For the mobile AVIF case, the user receives an 8 KB image instead of a 200 KB original -- a 96% reduction. On a page with 20 product images, this means the user downloads 160 KB instead of 4 MB. On a metered mobile connection at $10/GB, the per-page saving is roughly 4 cents. That adds up -- [the economics of image optimization](/blog/economics-of-image-optimization/) works the numbers through in full.

Save-Data is not either-or with other optimizations. It stacks. Viewport resizing reduces dimensions. Modern codecs reduce bits per pixel. Save-Data reduces quality. Each layer compounds on the previous one.

To put the bandwidth numbers in context: the HTTP Archive reports that the median mobile page loads 1.0 MB of images. A site running ModPageSpeed 2.0 with AVIF support serving mobile Save-Data users would reduce that to roughly 50-80 KB of images -- a 92-95% reduction. For users on metered connections, this is the difference between a page that costs a fraction of a cent to load and one that costs several cents.

## Interaction with viewport resizing

Save-Data variants interact with [viewport-based resizing](/blog/viewport-aware-image-optimization/) in a multiplicative way. When the worker generates a Save-Data variant for a mobile viewport, it first resizes the decoded pixel buffer to 480 pixels wide (preserving aspect ratio), then encodes at the lower Save-Data quality setting. The result benefits from both the reduced resolution and the reduced quality.

This is why the [proactive variant system](/blog/proactive-variant-generation-cache-warmup/) generates Save-Data siblings across all viewport classes. A desktop user with Save-Data enabled gets the full-resolution image at reduced quality. A mobile user with Save-Data enabled gets both reduced resolution and reduced quality. The system does not make assumptions about which dimensions to optimize -- it generates the full matrix and lets the cache key select the right variant for each specific client.

For 2x pixel density with Save-Data, the system doubles the viewport target width (mobile becomes 960px, tablet becomes 1536px) but uses the lower quality encoding. High-DPI Save-Data users still get sharper images than 1x users, but at reduced quality relative to high-DPI users without Save-Data. This respects both the device capability and the user preference simultaneously.

## Save-Data as the primary bandwidth signal

Save-Data is the primary bandwidth signal in ModPageSpeed 2.0. Earlier versions used the `ECT` (Effective Connection Type) header to apply per-connection quality multipliers. That was removed in favor of a simpler model: Save-Data selects the quality tier (normal or reduced), and the capability mask uses bits 6-7 for transfer encoding (identity, gzip, brotli) instead of connection type.

The rationale for removing ECT-based quality tuning:

1. **Save-Data already captures the intent.** A user who enables data saving has explicitly opted into reduced quality. Connection speed is a secondary signal that Save-Data subsumes for the explicit opt-in case.

2. **ECT was semi-broken in practice.** The proactive variant loop iterates formats, viewports, densities, and Save-Data -- but did not iterate connection types. All proactive variants inherited the triggering client's connection type. The first client to trigger processing determined the quality for all variants of that image.

3. **The bit space is better used for transfer encoding.** Pre-compressed gzip and brotli alternates eliminate dynamic compression CPU cost on every cache HIT -- a concrete, measurable improvement for every request, versus ECT quality tuning that only affected the small fraction of requests from slow-connection clients.

The quality model is now single-dimensional for bandwidth: Save-Data selects the base quality tier. Operators who need finer-grained quality control can adjust the Save-Data quality levels (`--savedata-jpeg-quality`, `--savedata-webp-quality`, `--savedata-avif-quality`).

## Proactive Save-Data variant generation

When proactive variant generation is enabled (`proactive_image_variants`, off by default), the worker generates Save-Data siblings as part of that proactive loop. When a notification arrives for any image URL, the loop iterates over both `SaveData::kOff` and `SaveData::kOn`, generating missing variants for each. This means a single notification from a user without Save-Data still triggers generation of the Save-Data variants.

The rationale is the same as for viewport siblings: the marginal cost of encoding at a different quality from an already-decoded pixel buffer is small. JPEG encoding at quality 60 takes roughly the same CPU time as quality 85. The additional cache storage is modest because Save-Data variants are smaller than their normal-quality counterparts by definition.

An operator can disable Save-Data sibling generation with `--no-proactive-savedata-variants`. When disabled, Save-Data variants are only generated when a user with `Save-Data: on` actually requests the image. The trade-off is that the first Save-Data user for each image hits the fallback path (receiving either a normal-quality variant or the original) while the worker generates their variant asynchronously.

## The fallback path for Save-Data

When a Save-Data user requests an image and the Save-Data variant does not exist yet, the nginx interceptor's fallback chain takes over. The chain preserves the Save-Data preference as long as possible: it tries the same format and viewport at Save-Data=on before falling back to Save-Data=off variants. Specifically:

1. Exact mask match (Save-Data=on, requested format, requested viewport)
2. Downgrade pixel density while preserving Save-Data
3. Downgrade image format while preserving Save-Data
4. Baseline with Save-Data (original format, 1x, same viewport, Save-Data=on)
5. Default mask (original content)

The baseline fallback preserves the Save-Data bit because serving a normal-quality image to a user who explicitly asked for reduced data is worse than serving a lower-capability image at reduced quality. The only case where Save-Data is dropped is the ultimate fallback to the default mask (`0x08`), which is the original content.

## Who sends Save-Data

The user profiles that enable Save-Data are worth understanding because they represent a specific, underserved audience.

**Users on metered connections.** In much of the world, mobile data is expensive relative to income. A user in India, Brazil, or Nigeria on a prepaid mobile plan has a direct financial incentive to enable data saving. Serving them 8 KB instead of 200 KB per image is not a nice-to-have -- it determines whether they can afford to browse the site at all.

**Users on slow connections.** Even without cost pressure, a user on a congested 3G network benefits from smaller payloads. A page that loads in 3 seconds with Save-Data on might take 15 seconds without it. The quality difference in the images is invisible compared to the difference in wait time.

**Users with explicit browser settings.** Chrome's "Lite mode" (formerly Data Saver) was discontinued in 2022, but the `Save-Data` header persists through OS-level and carrier-level proxies, particularly on Android. Samsung Internet, UC Browser, and Opera Mini all support or approximate the Save-Data signal.

The percentage of requests with `Save-Data: on` varies by geography and site audience. E-commerce sites with significant mobile traffic from emerging markets may see 5-15% of requests with the header. A B2B SaaS product marketed to enterprise users in North America might see less than 1%. The proactive variant system handles both cases: the cache storage for Save-Data variants is proportional to the number of unique images, not the number of Save-Data requests.

**Users on data-capped plans.** Even in markets with relatively affordable mobile data, many plans have monthly caps (1 GB, 2 GB, 5 GB). Users approaching their cap will enable data saving to stretch the remaining allocation. This means Save-Data traffic can spike at the end of billing cycles -- a pattern that is invisible in aggregate monthly analytics but significant for per-user experience.

**Corporate and educational networks.** Some enterprise proxy configurations add the `Save-Data` header to all outgoing requests to reduce bandwidth costs on shared network infrastructure. In these environments, the user may not have individually opted in, but the network administrator has decided that bandwidth efficiency matters more than maximum image quality.

## Configuring quality levels

The default quality settings are:

```
Normal:    JPEG 85, WebP 75, AVIF 25
Save-Data: JPEG 60, WebP 50, AVIF 35
```

These can be overridden at runtime with CLI flags:

```bash
factory_worker --cache-path /data/cache.vol \
  --jpeg-quality 80 --webp-quality 70 --avif-quality 30 \
  --savedata-jpeg-quality 55 --savedata-webp-quality 40 --savedata-avif-quality 40
```

The quality gap between normal and Save-Data modes (25 points for JPEG, 25 for WebP, 10 for AVIF) was tuned for broad applicability. Sites with primarily photographic content might prefer a narrower gap to preserve detail. Sites with predominantly UI screenshots or text-heavy imagery could widen the gap because these image types compress well even at low quality.

## Monitoring Save-Data impact

The worker's health endpoint and management socket report aggregate statistics that include Save-Data variants. The `proactive_variants_written` counter increments for each sibling variant written, including Save-Data siblings. The per-format counters (`webp_generated`, `avif_generated`, `jpeg_optimized`, `png_optimized`) count all variants regardless of Save-Data status.

To measure the bandwidth impact of Save-Data support specifically, compare the average response size for requests with and without the `Save-Data: on` header in your nginx access logs. The expected difference for image responses is 30-50% on top of whatever format and viewport optimization is already in effect.

A useful nginx log format addition for Save-Data analysis:

```
log_format savedata '$remote_addr - $request_uri '
                    '$http_save_data $body_bytes_sent '
                    '$upstream_http_content_type';
```

This lets you filter access logs by Save-Data presence and compare `body_bytes_sent` distributions between Save-Data=on and Save-Data=off requests for image content types.

## Cache storage impact

Save-Data doubles the number of variants per format/viewport/density combination. Without Save-Data, each image has 3 formats x 3 viewports x 2 densities = 18 variants. With Save-Data, it is 18 x 2 = 36.

However, the storage impact is less than 2x because Save-Data variants are smaller than their normal counterparts. A rough estimate for a typical 200 KB JPEG:

- Normal variants total: ~900 KB (18 variants across all formats/viewports/densities)
- Save-Data variants total: ~600 KB (18 variants at reduced quality)
- Combined total: ~1.5 MB

The Save-Data variants add roughly 67% more storage, not 100%, because they are smaller by definition. For a Cyclone cache sized at 1 GB, this means approximately 667 unique images with full variant matrices, or 1,000 images without Save-Data variants. Operators with tight cache budgets who do not see significant Save-Data traffic can reclaim that space with `--no-proactive-savedata-variants`.

## Enabling and disabling

Save-Data handling is on by default. No configuration is required -- the nginx interceptor reads the header, encodes it in the mask, and the worker generates the appropriate variant when a Save-Data user requests an image. Proactive Save-Data sibling generation (pre-encoding the siblings ahead of demand) rides along with `proactive_image_variants`, which is off by default.

When proactive variants are enabled, you can still suppress just the Save-Data siblings (variants are then generated on demand when a Save-Data user arrives):

```
factory_worker --no-proactive-savedata-variants ...
```

To disable Save-Data entirely (treat all requests as Save-Data=off, ignoring the header), the header parsing would need to be modified in the nginx interceptor. This is not recommended -- the header exists because the user asked for it, and the marginal cost of respecting it is small.

The combination of Save-Data with viewport resizing, modern codecs, and proactive variant generation means that ModPageSpeed 2.0 serves the right image to every user -- full quality for those who want it, reduced quality for those who asked for less, and the right resolution for every screen size. The 32-bit capability mask makes this possible without content negotiation logic in the application, without URL rewriting, and without any changes to the origin server.
