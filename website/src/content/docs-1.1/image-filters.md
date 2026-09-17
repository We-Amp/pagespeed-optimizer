---
title: 'Image Filters'
description: 'Configure image optimization filters in mod_pagespeed 1.15 for Apache, nginx, and IIS: recompression, WebP and opt-in AVIF conversion, resizing, lazy loading, and responsive images.'
order: 21
group: 'Filters'
lastUpdated: 2026-07-12
---

## Overview

mod_pagespeed 1.15 includes image filters for recompression, format conversion, resizing, and inlining that cut page weight without touching the original files on disk. The master filter `rewrite_images` is a CoreFilter and enables several sub-filters by default. Enable additional image filters individually for finer control. The same filter names and directives apply across the in-process module on Apache, nginx, and IIS.

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```
pagespeed EnableFilters rewrite_images
pagespeed ImageRecompressionQuality 75
```

See [IIS Configuration](/1.1/docs/iis-configuration/) for the full file format reference.

## Quick reference

| Filter                                                            | Core                 | OFB | Description                                                   | Safe       |
| ----------------------------------------------------------------- | -------------------- | --- | ------------------------------------------------------------- | ---------- |
| [`rewrite_images`](#rewrite_images)                               | Yes                  | -   | Master filter; enables recompress, resize, inline sub-filters | Yes        |
| [`recompress_images`](#recompress_images)                         | via `rewrite_images` | Yes | Recompress and convert images (lossy re-encode)               | Yes        |
| [`recompress_jpeg`](#recompress_images)                           | via `rewrite_images` | Yes | Recompress JPEG images                                        | Yes        |
| [`recompress_png`](#recompress_images)                            | via `rewrite_images` | Yes | Recompress PNG images                                         | Yes        |
| [`recompress_webp`](#recompress_images)                           | via `rewrite_images` | Yes | Recompress WebP images                                        | Yes        |
| [`convert_jpeg_to_progressive`](#recompress_images)               | via `rewrite_images` | Yes | Convert large JPEGs to progressive encoding                   | Yes        |
| [`convert_jpeg_to_webp`](#convert_formats)                        | via `rewrite_images` | Yes | Serve WebP to capable browsers                                | Yes        |
| [`convert_png_to_jpeg`](#convert_formats)                         | via `rewrite_images` | Yes | Convert opaque PNGs to JPEG                                   | Yes        |
| [`convert_gif_to_png`](#convert_formats)                          | via `rewrite_images` | Yes | Convert GIF to PNG                                            | Yes        |
| [`convert_to_webp_animated`](#convert_formats)                    | No                   | No  | Convert animated GIF to animated WebP                         | Test first |
| [`convert_to_webp_lossless`](#convert_formats)                    | via `rewrite_images` | No  | Use lossless WebP instead of lossy                            | Test first |
| [`convert_jpeg_to_avif`](#avif)                                   | No                   | No  | Serve AVIF for photographic JPEG sources                      | Test first |
| [`convert_to_avif_lossless`](#avif)                               | No                   | No  | Serve lossless AVIF for PNG/GIF sources                       | Test first |
| [`convert_to_avif_animated`](#avif)                               | No                   | No  | Serve AVIF for animated sources                               | Test first |
| [`recompress_avif`](#avif)                                        | No                   | No  | Re-encode AVIF images you already serve                       | Test first |
| [`strip_image_color_profile`](#strip_metadata)                    | via `rewrite_images` | Yes | Remove ICC color profiles                                     | Yes        |
| [`strip_image_meta_data`](#strip_metadata)                        | via `rewrite_images` | Yes | Remove EXIF and other metadata                                | Yes        |
| [`jpeg_subsampling`](#recompress_images)                          | via `rewrite_images` | Yes | Downsample JPEG color channels                                | Yes        |
| [`resize_images`](#resize_images)                                 | via `rewrite_images` | -   | Resize images to declared `width`/`height`                    | Yes        |
| [`resize_rendered_image_dimensions`](#resize_images)              | No                   | -   | Resize images to rendered dimensions via JS                   | Test first |
| [`inline_images`](#inline_images)                                 | via `rewrite_images` | -   | Inline small images as data: URIs                             | Yes        |
| [`responsive_images`](#responsive_images)                         | No                   | No  | Generate `srcset` attributes                                  | Test first |
| [`lazyload_images`](#lazyload_images)                             | No                   | No  | Defer offscreen image loading                                 | Test first |
| [`inline_preview_images`](#inline_images)                         | No                   | No  | Show low-quality placeholder before full load                 | Test first |
| [`resize_mobile_images`](#resize_images)                          | No                   | No  | Serve smaller images to mobile devices                        | Test first |
| [`dedup_inlined_images`](#inline_images)                          | No                   | No  | Deduplicate repeated inlined images                           | Yes        |
| [`sprite_images`](#sprite_images)                                 | No                   | No  | Combine CSS background images into sprites                    | Test first |
| [`insert_image_dimensions`](#resize_images)                       | No                   | No  | Add `width`/`height` to `<img>` tags                          | Test first |
| [`in_place_optimize_for_browser`](#in_place_optimize_for_browser) | No                   | Yes | Serve browser-specific formats via Vary: Accept               | Test first |

**Core** = enabled by default in the CoreFilters set.
**OFB** = enabled by OptimizeForBandwidth mode.

## Master filter: rewrite_images {#rewrite_images}

### What it does

`rewrite_images` is the master image optimization CoreFilter. Enabling it activates a family of sub-filters that recompress images, convert formats where beneficial, resize to declared dimensions, and inline small images as data: URIs.

### Sub-filters enabled by default

When `rewrite_images` is active, the following sub-filters are enabled automatically:

- `recompress_images` (and its sub-filters: `recompress_jpeg`, `recompress_png`, `recompress_webp`)
- `convert_jpeg_to_progressive`
- `convert_jpeg_to_webp`
- `convert_png_to_jpeg`
- `convert_gif_to_png`
- `convert_to_webp_lossless`
- `strip_image_color_profile`
- `strip_image_meta_data`
- `jpeg_subsampling`
- `resize_images`
- `inline_images`

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters rewrite_images
```

**nginx:**

```nginx
pagespeed EnableFilters rewrite_images;
```

To disable a specific sub-filter while keeping `rewrite_images` active:

**Apache:**

```apache
ModPagespeedEnableFilters rewrite_images
ModPagespeedDisableFilters convert_jpeg_to_webp
```

**nginx:**

```nginx
pagespeed EnableFilters rewrite_images;
pagespeed DisableFilters convert_jpeg_to_webp;
```

## Recompression filters {#recompress_images}

### What they do

The recompression filters (`recompress_images`, `recompress_jpeg`, `recompress_png`, `recompress_webp`) reduce image file size by re-encoding images at an optimal quality level. These filters preserve visual quality while removing encoding inefficiencies.

`recompress_images` is a convenience filter that enables `recompress_jpeg`, `recompress_png`, and `recompress_webp` together with `convert_gif_to_png`, `convert_jpeg_to_progressive`, `convert_jpeg_to_webp`, `convert_png_to_jpeg`, `jpeg_subsampling`, `strip_image_color_profile`, and `strip_image_meta_data`.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters recompress_images
# Or individually:
ModPagespeedEnableFilters recompress_jpeg,recompress_png,recompress_webp
```

**nginx:**

```nginx
pagespeed EnableFilters recompress_images;
# Or individually:
pagespeed EnableFilters recompress_jpeg,recompress_png,recompress_webp;
```

### Configuration

| Parameter                   | Default | Description                                                |
| --------------------------- | ------- | ---------------------------------------------------------- |
| `ImageRecompressionQuality` | 85      | General quality level for recompressed images (-1 to 100; -1 uses source quality) |
| `JpegRecompressionQuality`  | -1      | JPEG-specific quality; -1 uses `ImageRecompressionQuality` |
| `WebpRecompressionQuality`  | 80      | WebP quality level                                         |

**Apache:**

```apache
ModPagespeedImageRecompressionQuality 85
ModPagespeedJpegRecompressionQuality 75
```

**nginx:**

```nginx
pagespeed ImageRecompressionQuality 85;
pagespeed JpegRecompressionQuality 75;
```

### Risks

Recompression is lossy. Setting quality too low produces visible artifacts. The default values are conservative and safe for most content. Photographic content can tolerate lower quality than screenshots or text-heavy images.

## Format conversion filters {#convert_formats}

### What they do

Format conversion filters serve images in the most efficient format for each browser and image type:

- **`convert_jpeg_to_progressive`** converts large baseline JPEGs to progressive encoding. Progressive JPEGs render incrementally and are often smaller for images above ~10 KB.
- **`convert_jpeg_to_webp`** serves WebP versions of JPEG images to browsers that send `Accept: image/webp`. The original JPEG is preserved for other browsers.
- **`convert_png_to_jpeg`** converts PNG images with no alpha channel (fully opaque) to JPEG, which is typically much smaller for photographic content.
- **`convert_gif_to_png`** converts non-animated GIF images to PNG, which uses better compression.
- **`convert_to_webp_animated`** converts animated GIF images to animated WebP. Not a CoreFilter.
- **`convert_to_webp_lossless`** uses lossless WebP encoding instead of lossy. Produces larger files than lossy WebP but preserves every pixel. A CoreFilter, enabled by default through `rewrite_images`.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters convert_jpeg_to_progressive
ModPagespeedEnableFilters convert_jpeg_to_webp
ModPagespeedEnableFilters convert_png_to_jpeg
ModPagespeedEnableFilters convert_gif_to_png
ModPagespeedEnableFilters convert_to_webp_animated
ModPagespeedEnableFilters convert_to_webp_lossless
```

**nginx:**

```nginx
pagespeed EnableFilters convert_jpeg_to_progressive;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters convert_png_to_jpeg;
pagespeed EnableFilters convert_gif_to_png;
pagespeed EnableFilters convert_to_webp_animated;
pagespeed EnableFilters convert_to_webp_lossless;
```

### Configuration

| Parameter                          | Default | Description                                        |
| ---------------------------------- | ------- | -------------------------------------------------- |
| `ProgressiveJpegMinBytes`          | 10240   | Minimum JPEG size before converting to progressive |
| `WebpRecompressionQuality`         | 80      | Quality for lossy WebP conversion                  |
| `WebpAnimatedRecompressionQuality` | 70      | Quality for animated WebP conversion               |

### Risks

- `convert_png_to_jpeg` drops the alpha channel on opaque PNGs. mod_pagespeed checks for alpha before converting, but semi-transparent pixels at the boundary may cause subtle edge artifacts.
- `convert_jpeg_to_webp` relies on the browser's `Accept` header. Some CDNs strip or ignore `Vary: Accept`, which can cause incorrect format delivery. Test with your CDN configuration.
- `convert_to_webp_animated` can produce large files for complex animations. Verify output sizes.

## AVIF filters {#avif}

### What they do

Four filters transcode images to AVIF. Each one targets a different kind of source:

- **`convert_jpeg_to_avif`** encodes photographic JPEG sources as AVIF.
- **`convert_to_avif_lossless`** encodes flat-palette and screenshot-style sources (PNG, GIF) as lossless AVIF.
- **`convert_to_avif_animated`** encodes animated sources as AVIF.
- **`recompress_avif`** re-encodes AVIF images you already serve.

All four are opt-in. They sit outside `rewrite_images` and outside the CoreFilters set, so enabling `rewrite_images` alone produces no AVIF output — you have to name the AVIF filters you want.

AVIF is served only when the request advertises `Accept: image/avif`. There is no User-Agent sniffing. A client that does not advertise AVIF falls through the existing format order unchanged. The cache key folds in the client's AVIF capability, so variants are stored and served per capability exactly as WebP variants are.

The AV1 encoder is statically linked into the module. There is no separate package or codec library to install.

### Caveat: `RewriteLevel AllFilters` enables all four

"Opt-in" holds for filters you name explicitly. The four AVIF filters are not in the dangerous-filter set, so `RewriteLevel AllFilters` switches all of them on. If you run `AllFilters`, you are running the AVIF filters whether or not you listed them.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters convert_jpeg_to_avif
ModPagespeedEnableFilters convert_to_avif_lossless
ModPagespeedEnableFilters convert_to_avif_animated
ModPagespeedEnableFilters recompress_avif
```

**nginx:**

```nginx
pagespeed EnableFilters convert_jpeg_to_avif;
pagespeed EnableFilters convert_to_avif_lossless;
pagespeed EnableFilters convert_to_avif_animated;
pagespeed EnableFilters recompress_avif;
```

On IIS, use the same filter names in `pagespeed.config` without the trailing semicolon — see [IIS Configuration](/1.1/docs/iis-configuration/).

### Configuration

| Parameter                                  | Default | Description                                                                       |
| ------------------------------------------ | ------- | --------------------------------------------------------------------------------- |
| `AvifRecompressionQuality`                 | 60      | AVIF quality level; -1 uses `ImageRecompressionQuality`                            |
| `AvifRecompressionQualityForSmallScreens`  | 50      | AVIF quality for small-screen clients; -1 uses `AvifRecompressionQuality`          |
| `AvifAnimatedRecompressionQuality`         | 50      | Quality for animated AVIF output                                                  |
| `AvifQualityForSaveData`                   | 45      | AVIF quality for clients sending `Save-Data: on`                                   |
| `AvifTimeoutMs`                            | 5000    | Wall-clock budget for one AVIF encode; the encode is abandoned when it is exceeded |

These mirror the `Webp*` quality parameters: a value of -1 falls back to the more general
setting, so you can set `ImageRecompressionQuality` alone and let AVIF follow it.

`AvifTimeoutMs` exists because AV1 still-image encoding is materially slower than WebP. The
budget stops one expensive image from occupying a rewrite slot indefinitely; when it is hit
the AVIF candidate is abandoned and the existing WebP or optimized-original output is served,
so a timeout costs CPU but never a broken image. Raise it if large photographic sources are
being skipped, lower it if AVIF encodes are crowding out other rewrites.

**Apache:**

```apache
ModPagespeedAvifRecompressionQuality 60
ModPagespeedAvifTimeoutMs 5000
```

**nginx:**

```nginx
pagespeed AvifRecompressionQuality 60;
pagespeed AvifTimeoutMs 5000;
```

### Risks

- **An AVIF candidate costs a full extra decode and encode per image.** The module encodes an AVIF candidate and keeps it only if it beats the WebP or optimized-original candidate. Each candidate is produced from its own decode; the decode is not shared between formats. AV1 encoding is also slower than WebP encoding at comparable quality, so enabling AVIF measurably raises the CPU cost of the first rewrite of every image. Watch `ImageMaxRewritesAtOnce` and the rewrite deadline on busy origins.
- **Pick-smaller means some of that work is discarded.** When the AVIF candidate is not smaller, it is dropped and the existing variant is served. The CPU is spent either way.
- **Images with C2PA content credentials are not converted.** A source carrying a C2PA manifest is left alone by all four filters, so its provenance chain survives the pipeline.
- **Delivery depends on the `Accept` header reaching the module.** As with `convert_jpeg_to_webp`, a CDN or proxy that rewrites `Accept` or ignores `Vary: Accept` can cross-serve formats. Test with your CDN configuration before rolling out.

## Image stripping filters {#strip_metadata}

### What they do

- **`strip_image_color_profile`** removes embedded ICC color profiles from images. Most web browsers ignore ICC profiles, and they can add tens of kilobytes to a file.
- **`strip_image_meta_data`** removes EXIF, XMP, and other metadata from images. This includes camera information, GPS coordinates, thumbnails, and editing history.
- **`jpeg_subsampling`** downsamples JPEG chroma channels (4:2:0 subsampling), reducing file size with minimal visual impact on photographic content.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters strip_image_color_profile
ModPagespeedEnableFilters strip_image_meta_data
ModPagespeedEnableFilters jpeg_subsampling
```

**nginx:**

```nginx
pagespeed EnableFilters strip_image_color_profile;
pagespeed EnableFilters strip_image_meta_data;
pagespeed EnableFilters jpeg_subsampling;
```

### Risks

- Stripping color profiles can cause subtle color shifts on wide-gamut displays or for images authored in non-sRGB color spaces. This affects a small fraction of web images.
- Stripping metadata removes copyright and attribution information embedded in the file. The metadata is removed from the served variant only; original files on disk are not modified.

## Resizing filters {#resize_images}

### What they do

- **`resize_images`** resizes images on the server to match the `width` and `height` attributes declared in the `<img>` tag. If an image is 2000x1500 but displayed at 400x300, mod_pagespeed serves a 400x300 variant.
- **`resize_rendered_image_dimensions`** injects JavaScript that reports each image's actual rendered dimensions on the client. On subsequent requests, mod_pagespeed resizes to the rendered size. Requires two page loads to take effect.
- **`insert_image_dimensions`** adds explicit `width` and `height` attributes to `<img>` tags that lack them. This prevents layout shifts (CLS) but does not resize the image file itself.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters resize_images
ModPagespeedEnableFilters resize_rendered_image_dimensions
ModPagespeedEnableFilters insert_image_dimensions
```

**nginx:**

```nginx
pagespeed EnableFilters resize_images;
pagespeed EnableFilters resize_rendered_image_dimensions;
pagespeed EnableFilters insert_image_dimensions;
```

### Configuration

| Parameter                     | Default | Description                                                               |
| ----------------------------- | ------- | ------------------------------------------------------------------------- |
| `ImageLimitResizeAreaPercent` | 100     | Only resize if the result is this percentage of the original area or less |

### Risks

- `resize_images` only works when `width` and `height` are present on the `<img>` tag. Images sized purely by CSS are not resized.
- `resize_rendered_image_dimensions` injects JavaScript and requires two page loads. The first load serves the original image.
- `insert_image_dimensions` can break responsive layouts that rely on the absence of explicit dimensions. Test with your CSS.

## Inline and preview filters {#inline_images}

### What they do

- **`inline_images`** replaces small image references with `data:` URIs, eliminating the HTTP request. Only images below `ImageInlineMaxBytes` are inlined.
- **`inline_preview_images`** replaces full-size images with a low-quality inline placeholder that loads instantly, then swaps in the full image via JavaScript.
- **`dedup_inlined_images`** replaces repeated inline `data:` URIs on the same page with JavaScript references to the first occurrence, reducing HTML size.
- **`resize_mobile_images`** serves smaller images to mobile devices based on the User-Agent header. Enabling it also enables `inline_preview_images`, which it depends on.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters inline_images
ModPagespeedEnableFilters inline_preview_images
ModPagespeedEnableFilters dedup_inlined_images
ModPagespeedEnableFilters resize_mobile_images
```

**nginx:**

```nginx
pagespeed EnableFilters inline_images;
pagespeed EnableFilters inline_preview_images;
pagespeed EnableFilters dedup_inlined_images;
pagespeed EnableFilters resize_mobile_images;
```

### Configuration

| Parameter             | Default | Description                                          |
| --------------------- | ------- | ---------------------------------------------------- |
| `ImageInlineMaxBytes` | 3072    | Maximum image size in bytes to inline as a data: URI |

**Apache:**

```apache
ModPagespeedImageInlineMaxBytes 4096
```

**nginx:**

```nginx
pagespeed ImageInlineMaxBytes 4096;
```

### Risks

- Inlining increases HTML size. Inlined images are not cached separately by the browser. Set `ImageInlineMaxBytes` conservatively.
- `inline_preview_images` adds JavaScript and a visible quality transition. Users see a blurry image before the full-resolution variant loads.
- `resize_mobile_images` relies on User-Agent detection. Incorrect UA classification can serve wrong-sized images. It also pulls in `inline_preview_images`, so expect that filter's placeholder-then-swap behavior when enabling it.

## Lazy loading: lazyload_images {#lazyload_images}

### What it does

`lazyload_images` defers loading of images that are below the fold, reducing initial page weight and request count.

In v1.15.0+r18 and later, on browsers that support it (the large majority), the filter adds the native `loading="lazy"` and `decoding="async"` attributes and leaves image URLs untouched. Images known to be above the fold are not deferred and instead get `fetchpriority="high"`. On older browsers — and in v1.15.0+r17 and earlier — the filter uses a JavaScript loader that loads images as the user scrolls them into view.

The `LazyloadImagesMode` directive controls this behavior: `auto` (the default) selects native or JavaScript per browser, `native` always uses the attributes, and `js` always uses the JavaScript loader (the behavior of v1.15.0+r17 and earlier).

When the filter has no data yet about which images are above the fold, it leaves the first images of the page alone so the largest visible image is not deferred. `LazyloadImagesSkipFirst` sets how many (default 1).

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters lazyload_images
```

**nginx:**

```nginx
pagespeed EnableFilters lazyload_images;
```

### Configuration

Select the mechanism and tune the above-the-fold protection:

**Apache:**

```apache
ModPagespeedLazyloadImagesMode auto
ModPagespeedLazyloadImagesSkipFirst 1
```

**nginx:**

```nginx
pagespeed LazyloadImagesMode auto;
pagespeed LazyloadImagesSkipFirst 1;
```

To disable lazy loading for a specific image, add the [`data-pagespeed-no-defer`](/pagespeed-markers/#data-pagespeed-no-defer) attribute:

```html
<img src="hero.jpg" data-pagespeed-no-defer />
```

Images that already carry a `loading` attribute are always left untouched, so hand-tuned markup keeps working.

### Risks

- In `js` mode (or `auto` on older browsers), JavaScript-based lazy loading can interfere with image-dependent layout calculations, print rendering, and automated testing tools. The JavaScript mechanism also stands down on pages whose `Content-Security-Policy` disallows inline scripts; native mode is unaffected by CSP.
- Images that are immediately visible (above the fold) should not be lazy loaded. Above-fold detection is data-driven and imperfect; `LazyloadImagesSkipFirst` bounds the damage when no data is available, but pages with unusual layouts may still see a deferred visible image. Use `data-pagespeed-no-defer` on known-critical images.

## Responsive images {#responsive_images}

### What it does

`responsive_images` generates multiple resized versions of each image and adds `srcset` attributes to `<img>` tags, allowing the browser to select the optimal resolution for the current viewport and device pixel ratio.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters responsive_images
```

**nginx:**

```nginx
pagespeed EnableFilters responsive_images;
```

### Configuration

To control which pixel densities are generated (default `1.5,2,3`), use `ResponsiveImageDensities` with a comma-separated list of numbers:

**Apache:**

```apache
ModPagespeedResponsiveImageDensities 1.5,2,3
```

**nginx:**

```nginx
pagespeed ResponsiveImageDensities 1.5,2,3;
```

### Risks

- Generating multiple variants per image increases storage and cache requirements on the server.
- If images have many density variants, the total bytes served across all variants can exceed the original single image. Monitor cache size and bandwidth.
- Requires `width` and `height` attributes on `<img>` tags to calculate variant dimensions.

## Sprite images {#sprite_images}

### What it does

`sprite_images` combines multiple CSS background images into a single sprite sheet and rewrites the CSS `background-position` values to reference the correct region within the sprite.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters sprite_images
```

**nginx:**

```nginx
pagespeed EnableFilters sprite_images;
```

### Risks

- With HTTP/2 multiplexing, the latency benefit of spriting is minimal. Multiple small requests over a single connection are often as fast as one large sprite request.
- Spriting increases cache invalidation scope: changing one image invalidates the entire sprite.
- Only CSS `background-image` references are sprited. Inline `<img>` tags are not affected.

## In-place browser optimization {#in_place_optimize_for_browser}

### What it does

`in_place_optimize_for_browser` optimizes images served from their original URL (without rewriting the HTML) by using the browser's `Accept` header to serve the best format. For example, a `.jpg` URL can serve WebP content to browsers that support it. The response includes `Vary: Accept` to ensure correct caching.

This filter is enabled by the `OptimizeForBandwidth` mode but is not a CoreFilter.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters in_place_optimize_for_browser
```

**nginx:**

```nginx
pagespeed EnableFilters in_place_optimize_for_browser;
```

### Risks

- The `Vary: Accept` header can reduce CDN cache hit rates. Many CDNs handle `Vary` correctly, but verify with your provider.
- Some proxy servers do not respect `Vary` headers and may serve the wrong format to clients.

## Prioritize critical images {#prioritize_critical_images}

### What it does

`prioritize_critical_images` sets `fetchpriority="high"` on the images the critical-images beacon has reported above the fold, so the browser front-loads the fetches that most influence Largest Contentful Paint. It is beacon-driven: the page is instrumented, real browsers report which images render above the fold, and the attribute is applied on subsequent responses. It rewrites attributes only and injects no scripts. Enabling the filter also turns on critical-images beaconing.

The filter is a strict no-op until beacon data is available — with no data it would have to guess, and a wrong guess would prioritize a below-the-fold image at the real LCP image's expense. An author-supplied `fetchpriority` always wins, so hand-tuned markup is left untouched.

This filter is opt-in and is not part of any rewrite level. Enable it by name.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters prioritize_critical_images
```

**nginx:**

```nginx
pagespeed EnableFilters prioritize_critical_images;
```

### Risks

- Above-fold detection is data-driven and imperfect. The strict no-op-without-data behavior avoids the worst case, but on unusual layouts the beacon may still flag an image that is not truly the largest one.
- The filter backs off on `Save-Data` requests, AMP documents, and disallowed URLs.

## Tuning parameters

| Parameter                          | Default  | Description                                                        |
| ---------------------------------- | -------- | ------------------------------------------------------------------ |
| `ImageRecompressionQuality`        | 85       | General quality for recompressed images (-1 to 100; -1 uses source quality) |
| `JpegRecompressionQuality`         | -1       | JPEG-specific quality; -1 uses `ImageRecompressionQuality`         |
| `WebpRecompressionQuality`         | 80       | WebP quality level                                                 |
| `WebpAnimatedRecompressionQuality` | 70       | Animated WebP quality level                                        |
| `ImageInlineMaxBytes`              | 3072     | Maximum image size in bytes to inline as a data: URI               |
| `ImageLimitOptimizedPercent`       | 100      | Only serve the optimized image if it is smaller by this percentage |
| `ImageLimitResizeAreaPercent`      | 100      | Limit on resize area relative to original                          |
| `ImageResolutionLimitBytes`        | 33554432 | Maximum image resolution in bytes to attempt to optimize           |
| `ImageMaxRewritesAtOnce`           | 8        | Server-wide limit on parallel image optimizations                  |

**Apache:**

```apache
ModPagespeedImageRecompressionQuality 85
ModPagespeedJpegRecompressionQuality 75
ModPagespeedWebpRecompressionQuality 80
ModPagespeedWebpAnimatedRecompressionQuality 70
ModPagespeedImageInlineMaxBytes 3072
ModPagespeedImageLimitOptimizedPercent 100
ModPagespeedImageLimitResizeAreaPercent 100
ModPagespeedImageResolutionLimitBytes 33554432
ModPagespeedImageMaxRewritesAtOnce 8
```

**nginx:**

```nginx
pagespeed ImageRecompressionQuality 85;
pagespeed JpegRecompressionQuality 75;
pagespeed WebpRecompressionQuality 80;
pagespeed WebpAnimatedRecompressionQuality 70;
pagespeed ImageInlineMaxBytes 3072;
pagespeed ImageLimitOptimizedPercent 100;
pagespeed ImageLimitResizeAreaPercent 100;
pagespeed ImageResolutionLimitBytes 33554432;
pagespeed ImageMaxRewritesAtOnce 8;
```

## Design background: how server-side image rewriting works

> **Historical context.** This summarizes a design decision from the original mod_pagespeed project (Google, 2010), included as background. The filter behavior documented above is the current reference for mod_pagespeed 1.1.

The image filters above descend from a 2010 design for server-side image rewriting. Four invariants from that design still shape how `rewrite_images` works:

1. **Fetch and cache the original bytes asynchronously.** A rewrite never blocks the response. On the first request the original image is served and the optimization runs in the background (bounded by `RewriteDeadlinePerFlushMs`); the optimized variant is served from cache on subsequent requests.
2. **Compare displayed dimensions to natural dimensions.** When the page uses an image smaller than the source, `resize_images` rescales it to the size actually rendered instead of shipping full-resolution pixels the browser will only shrink.
3. **Recompress per format, and only keep a smaller result.** Each format has its own codec strategy (`recompress_jpeg`, `recompress_png`, `recompress_webp`). A conversion (`convert_jpeg_to_webp`, `convert_png_to_jpeg`, `convert_gif_to_png`) or recompression is kept only when the output is smaller, which is what `ImageLimitOptimizedPercent` enforces.
4. **Cache the negative result too.** If no variant is meaningfully smaller, the original URL is kept, and mod_pagespeed remembers that so it does not re-attempt a losing rewrite on every request.

The output formats have changed since 2010: mod_pagespeed 1.15 transcodes to WebP through `rewrite_images`, and to AVIF through [opt-in filters](#avif) enabled separately from it. The separate ModPageSpeed 2.0 rebuild adds SVG auto-vectorization and Jpegli through its own independent optimization engine. The four invariants above describe how 1.15's `rewrite_images` pipeline works. For how this works in production, see [Automatic WebP/AVIF on nginx: One Decode, 37 Variants](/blog/viewport-aware-image-optimization/) and [Image optimization cost: self-hosted vs CDN](/blog/economics-of-image-optimization/).

_Adapted from the original mod_pagespeed image-rewriting design (Google, 2010), an open-source project now maintained by We-Amp B.V. Original material © Google Inc., released under the Apache License 2.0. mod_pagespeed and PageSpeed are trademarks of Google LLC. We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently._

## See also

- [Filter Selection](/1.1/docs/filter-selection/) -- how to enable and disable filters
- [Filter Reference](/1.1/docs/filter-reference/) -- all filters at a glance
- [How the metadata cache works](/how-it-works/metadata-cache/) -- how an image is optimized once and served from cache thereafter
