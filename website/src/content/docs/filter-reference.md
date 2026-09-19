---
title: 'PageSpeed Filter Reference'
description: 'Every PageSpeed filter in mod_pagespeed 2.1 — all 66, with category, CoreFilters and OptimizeForBandwidth status, a safety rating, and a one-line description. nginx, Apache, and IIS syntax included.'
order: 42
group: 'Filters'
lastUpdated: 2026-07-12
---

Directive-level reference for every filter in mod_pagespeed 2.1: name, category, whether it runs under CoreFilters or OptimizeForBandwidth (OFB), safety rating, and a one-line description. For how the filters group together conceptually, start with the [PageSpeed filters](/docs/filters/) overview; to turn them on and off, see [Filter Selection](/docs/filter-selection/).

## Platform syntax

Filter names are the same across all platforms. The directive syntax differs:

| Platform | Enable filter                              | Disable filter                              |
| -------- | ------------------------------------------ | ------------------------------------------- |
| nginx    | `pagespeed EnableFilters rewrite_images;`  | `pagespeed DisableFilters rewrite_images;`  |
| Apache   | `ModPagespeedEnableFilters rewrite_images` | `ModPagespeedDisableFilters rewrite_images` |
| IIS      | `pagespeed EnableFilters rewrite_images`   | `pagespeed DisableFilters rewrite_images`   |

IIS uses the same `pagespeed` prefix as nginx but without the trailing semicolon. All directives go in `pagespeed.config`. See [IIS Configuration](/docs/iis-configuration/) for the full file format reference.

## All filters

| Filter                                                                                                                               | Category   | Core | OFB | Description                                            | Safe           |
| ------------------------------------------------------------------------------------------------------------------------------------ | ---------- | ---- | --- | ------------------------------------------------------ | -------------- |
| <a id="add_head"></a>[`add_head`](/docs/html-filters/#add_head)                                                                  | HTML       | Yes  | No  | Adds `<head>` element if missing                       | Generally safe |
| <a id="add_instrumentation"></a>[`add_instrumentation`](/docs/html-filters/#add_instrumentation)                                 | HTML       | No   | No  | Injects JavaScript to measure page load time           | Test first     |
| <a id="collapse_whitespace"></a>[`collapse_whitespace`](/docs/html-filters/#collapse_whitespace)                                 | HTML       | No   | No  | Removes excess whitespace from HTML                    | Generally safe |
| <a id="combine_css"></a>[`combine_css`](/docs/css-filters/#combine_css)                                                          | CSS        | Yes  | No  | Combines multiple CSS files into one                   | Generally safe |
| <a id="combine_heads"></a>[`combine_heads`](/docs/html-filters/#combine_heads)                                                   | HTML       | No   | No  | Merges multiple `<head>` elements                      | Generally safe |
| <a id="combine_javascript"></a>[`combine_javascript`](/docs/javascript-filters/#combine_javascript)                              | JavaScript | Yes  | No  | Combines multiple JS files into one                    | Generally safe |
| <a id="convert_gif_to_png"></a>[`convert_gif_to_png`](/docs/image-filters/#convert_formats)                                      | Image      | Yes  | Yes | Converts GIF to PNG                                    | Generally safe |
| <a id="convert_jpeg_to_avif"></a>[`convert_jpeg_to_avif`](/docs/image-filters/#avif)                                             | Image      | No   | No  | Converts photographic JPEG to AVIF for capable browsers | Test first     |
| <a id="convert_jpeg_to_progressive"></a>[`convert_jpeg_to_progressive`](/docs/image-filters/#recompress_images)                  | Image      | Yes  | Yes | Converts large JPEGs to progressive format             | Generally safe |
| <a id="convert_jpeg_to_webp"></a>[`convert_jpeg_to_webp`](/docs/image-filters/#convert_formats)                                  | Image      | Yes  | Yes | Converts JPEG to WebP for capable browsers             | Generally safe |
| <a id="convert_meta_tags"></a>[`convert_meta_tags`](/docs/html-filters/#convert_meta_tags)                                       | HTML       | Yes  | No  | Adds HTTP headers from `<meta http-equiv>` tags        | Generally safe |
| <a id="convert_png_to_jpeg"></a>[`convert_png_to_jpeg`](/docs/image-filters/#convert_formats)                                    | Image      | Yes  | Yes | Converts PNG to JPEG when no transparency              | Generally safe |
| <a id="convert_to_avif_animated"></a>[`convert_to_avif_animated`](/docs/image-filters/#avif)                                     | Image      | No   | No  | Converts animated images to AVIF                       | Test first     |
| <a id="convert_to_avif_lossless"></a>[`convert_to_avif_lossless`](/docs/image-filters/#avif)                                     | Image      | No   | No  | Converts PNG/GIF to lossless AVIF                      | Test first     |
| <a id="convert_to_webp_animated"></a>[`convert_to_webp_animated`](/docs/image-filters/#convert_formats)                          | Image      | No   | No  | Converts animated GIF to WebP                          | Test first     |
| <a id="convert_to_webp_lossless"></a>[`convert_to_webp_lossless`](/docs/image-filters/#convert_formats)                          | Image      | Yes  | No  | Converts PNG/GIF to lossless WebP                      | Test first     |
| <a id="dedup_inlined_images"></a>[`dedup_inlined_images`](/docs/image-filters/#inline_images)                                    | Image      | No   | No  | Replaces repeated inlined images with JS reference     | Generally safe |
| <a id="defer_javascript"></a>[`defer_javascript`](/docs/javascript-filters/#defer_javascript)                                    | JavaScript | No   | No  | Defers JS execution until after page load              | Test first     |
| <a id="elide_attributes"></a>[`elide_attributes`](/docs/html-filters/#elide_attributes)                                          | HTML       | No   | No  | Removes default-value HTML attributes                  | Generally safe |
| <a id="extend_cache"></a>[`extend_cache`](/docs/cache-control/#extend_cache)                                               | Caching    | Yes  | No  | Content-hashed URLs with 1-year browser cache          | Generally safe |
| <a id="extend_cache_pdfs"></a>[`extend_cache_pdfs`](/docs/cache-control/#extend_cache_pdfs)                                | Caching    | No   | No  | Cache extension for PDF links                          | Generally safe |
| <a id="fallback_rewrite_css_urls"></a>[`fallback_rewrite_css_urls`](/docs/css-filters/#fallback_rewrite_css_urls)                | CSS        | Yes  | No  | Rewrites resource URLs in unparseable CSS              | Generally safe |
| <a id="flatten_css_imports"></a>[`flatten_css_imports`](/docs/css-filters/#flatten_css_imports)                                  | CSS        | Yes  | No  | Inlines CSS `@import` rules                            | Generally safe |
| <a id="hint_preload_subresources"></a>[`hint_preload_subresources`](/docs/html-filters/#hint_preload_subresources)               | HTML       | No   | No  | Adds `Link: rel=preload` headers                       | Generally safe |
| <a id="in_place_optimize_for_browser"></a>[`in_place_optimize_for_browser`](/docs/image-filters/#in_place_optimize_for_browser)  | Image      | No   | Yes | Browser-specific in-place optimization                 | Test first     |
| <a id="include_js_source_maps"></a>[`include_js_source_maps`](/docs/javascript-filters/#include_js_source_maps)                  | JavaScript | No   | No  | Preserves JavaScript source maps                       | Generally safe |
| <a id="inline_css"></a>[`inline_css`](/docs/css-filters/#inline_css)                                                             | CSS        | Yes  | No  | Inlines small external CSS into HTML                   | Generally safe |
| <a id="inline_google_font_css"></a>[`inline_google_font_css`](/docs/css-filters/#inline_google_font_css)                         | CSS        | No   | No  | Inlines Google Fonts CSS                               | Generally safe |
| <a id="inline_images"></a>[`inline_images`](/docs/image-filters/#inline_images)                                                  | Image      | Yes  | No  | Inlines small images as data: URIs                     | Generally safe |
| <a id="inline_import_to_link"></a>[`inline_import_to_link`](/docs/css-filters/#inline_import_to_link)                            | CSS        | Yes  | No  | Converts `<style>@import</style>` to `<link>`          | Generally safe |
| <a id="inline_javascript"></a>[`inline_javascript`](/docs/javascript-filters/#inline_javascript)                                 | JavaScript | Yes  | No  | Inlines small external JS into HTML                    | Generally safe |
| <a id="inline_preview_images"></a>[`inline_preview_images`](/docs/image-filters/#inline_images)                                  | Image      | No   | No  | Inserts low-quality image placeholders                 | Test first     |
| <a id="insert_dns_prefetch"></a>[`insert_dns_prefetch`](/docs/html-filters/#insert_dns_prefetch)                                 | HTML       | No   | No  | Adds `<link rel=dns-prefetch>` for third-party domains | Generally safe |
| <a id="insert_image_dimensions"></a>[`insert_image_dimensions`](/docs/image-filters/#resize_images)                              | Image      | No   | No  | Adds width and height attributes to `<img>` tags       | Generally safe |
| <a id="insert_speculation_rules"></a>[`insert_speculation_rules`](/docs/html-filters/#insert_speculation_rules)                  | HTML       | No   | No  | Injects a same-origin prefetch speculation-rules script| Test first     |
| <a id="jpeg_subsampling"></a>[`jpeg_subsampling`](/docs/image-filters/#recompress_images)                                              | Image      | Yes  | Yes | Reduces chroma sampling to 4:2:0                       | Generally safe |
| <a id="lazyload_images"></a>[`lazyload_images`](/docs/image-filters/#lazyload_images)                                            | Image      | No   | No  | Defers offscreen image loading                         | Generally safe in native mode (r18+); test `js` mode |
| <a id="local_storage_cache"></a>[`local_storage_cache`](/docs/cache-control/#local_storage_cache)                          | Caching    | No   | No  | Caches inlined resources in localStorage               | Experimental   |
| <a id="move_css_above_scripts"></a>[`move_css_above_scripts`](/docs/css-filters/#move_css_above_scripts)                         | CSS        | No   | No  | Moves CSS `<link>` above `<script>` tags               | Generally safe |
| <a id="move_css_to_head"></a>[`move_css_to_head`](/docs/css-filters/#move_css_to_head)                                           | CSS        | No   | No  | Moves CSS `<link>` into `<head>`                       | Generally safe |
| <a id="outline_css"></a>[`outline_css`](/docs/css-filters/#outline_css)                                                          | CSS        | No   | No  | Externalizes large inline CSS blocks                   | Experimental   |
| <a id="outline_javascript"></a>[`outline_javascript`](/docs/javascript-filters/#outline_javascript)                              | JavaScript | No   | No  | Externalizes large inline JS blocks                    | Experimental   |
| <a id="pedantic"></a>[`pedantic`](/docs/html-filters/#pedantic)                                                                  | HTML       | No   | No  | Adds `type` attributes for HTML4 validation            | Generally safe |
| <a id="prioritize_critical_css"></a>[`prioritize_critical_css`](/docs/css-filters/#prioritize_critical_css)                      | CSS        | No   | No  | Inlines above-fold CSS, defers the rest                | Test first     |
| <a id="prioritize_critical_images"></a>[`prioritize_critical_images`](/docs/image-filters/#prioritize_critical_images)           | Image      | No   | No  | Sets `fetchpriority=high` on the LCP image             | Test first     |
| <a id="recompress_avif"></a>[`recompress_avif`](/docs/image-filters/#avif)                                                       | Image      | No   | No  | AVIF-specific recompression                            | Test first     |
| <a id="recompress_images"></a>[`recompress_images`](/docs/image-filters/#recompress_images)                                      | Image      | Yes  | Yes | Recompresses and converts images (lossy re-encode)     | Generally safe |
| <a id="recompress_jpeg"></a>[`recompress_jpeg`](/docs/image-filters/#recompress_images)                                          | Image      | Yes  | Yes | JPEG-specific recompression                            | Generally safe |
| <a id="recompress_png"></a>[`recompress_png`](/docs/image-filters/#recompress_images)                                            | Image      | Yes  | Yes | PNG-specific recompression                             | Generally safe |
| <a id="recompress_webp"></a>[`recompress_webp`](/docs/image-filters/#recompress_images)                                          | Image      | Yes  | Yes | WebP-specific recompression                            | Generally safe |
| <a id="remove_comments"></a>[`remove_comments`](/docs/html-filters/#remove_comments)                                             | HTML       | No   | No  | Strips HTML comments                                   | Generally safe |
| <a id="remove_quotes"></a>[`remove_quotes`](/docs/html-filters/#remove_quotes)                                                   | HTML       | No   | No  | Removes unnecessary attribute quotes                   | Generally safe |
| <a id="resize_images"></a>[`resize_images`](/docs/image-filters/#resize_images)                                                  | Image      | Yes  | No  | Resizes images to match `<img>` dimensions             | Generally safe |
| <a id="resize_mobile_images"></a>[`resize_mobile_images`](/docs/image-filters/#resize_images)                                    | Image      | No   | No  | Smaller placeholders for mobile                        | Test first     |
| <a id="resize_rendered_image_dimensions"></a>[`resize_rendered_image_dimensions`](/docs/image-filters/#resize_images)            | Image      | No   | No  | Resizes to rendered dimensions                         | Test first     |
| <a id="responsive_images"></a>[`responsive_images`](/docs/image-filters/#responsive_images)                                      | Image      | No   | No  | Generates `srcset` for multiple resolutions            | Test first     |
| <a id="rewrite_css"></a>[`rewrite_css`](/docs/css-filters/#rewrite_css)                                                          | CSS        | Yes  | Yes | Minifies CSS, rewrites embedded URLs                   | Generally safe |
| <a id="rewrite_domains"></a>[`rewrite_domains`](/docs/cache-control/#rewrite_domains)                                      | Caching    | No   | No  | Applies domain mappings to original resources          | Test first     |
| <a id="rewrite_images"></a>[`rewrite_images`](/docs/image-filters/#rewrite_images)                                               | Image      | Yes  | No  | Master image optimization (enables sub-filters)        | Generally safe |
| <a id="rewrite_javascript"></a>[`rewrite_javascript`](/docs/javascript-filters/#rewrite_javascript)                              | JavaScript | Yes  | Yes | Minifies JavaScript                                    | Generally safe |
| <a id="rewrite_javascript_external"></a>`rewrite_javascript_external`                                                                | JavaScript | Yes  | Yes | Minifies external JavaScript files                     | Generally safe |
| <a id="rewrite_javascript_inline"></a>`rewrite_javascript_inline`                                                                    | JavaScript | Yes  | Yes | Minifies inline JavaScript                             | Generally safe |
| <a id="rewrite_style_attributes"></a>[`rewrite_style_attributes`](/docs/css-filters/#rewrite_style_attributes)                   | CSS        | No   | No  | Applies CSS rewriting to inline `style` attributes     | Generally safe |
| <a id="rewrite_style_attributes_with_url"></a>[`rewrite_style_attributes_with_url`](/docs/css-filters/#rewrite_style_attributes) | CSS        | Yes  | No  | Same, only for styles containing `url()`               | Generally safe |
| <a id="sprite_images"></a>[`sprite_images`](/docs/image-filters/#sprite_images)                                                  | Image      | No   | No  | Combines CSS background images into sprites            | Test first     |
| <a id="strip_image_color_profile"></a>[`strip_image_color_profile`](/docs/image-filters/#strip_metadata)                         | Image      | Yes  | Yes | Removes ICC color profiles                             | Generally safe |
| <a id="strip_image_meta_data"></a>[`strip_image_meta_data`](/docs/image-filters/#strip_metadata)                                 | Image      | Yes  | Yes | Removes EXIF and other metadata                        | Generally safe |
| <a id="trim_urls"></a>[`trim_urls`](/docs/html-filters/#trim_urls)                                                               | HTML       | No   | No  | Shortens URLs relative to base URL                     | Generally safe |

## Notes

- **Core** filters are enabled by default with `RewriteLevel CoreFilters`. Some image sub-filters (e.g., `recompress_images`, `jpeg_subsampling`, `strip_image_meta_data`) are implicitly enabled by their parent filter `rewrite_images` and are marked Core for that reason. See [Filter Selection](/docs/filter-selection/) for the explicit CoreFilters list.
- **OFB** filters are active under `RewriteLevel OptimizeForBandwidth`, which optimizes resources in-place without rewriting URLs.
- The four **AVIF** filters (`convert_jpeg_to_avif`, `convert_to_avif_lossless`, `convert_to_avif_animated`, `recompress_avif`) are outside both sets and outside `rewrite_images`, so you enable them by name. `RewriteLevel AllFilters` does switch all four on. See [AVIF filters](/docs/image-filters/#avif).
- **"Test first"** filters are safe for most sites but can cause issues with specific JavaScript frameworks or CSS patterns. Test on a staging environment before enabling in production.
- **"Experimental"** filters are available but rarely needed. Use only if you have a specific reason.

## Deprecated filters

These filters remain available for backward compatibility but target retired or deprecated services:

- <a id="canonicalize_javascript_libraries"></a>`canonicalize_javascript_libraries` — redirects to Google Hosted Libraries CDN
- <a id="insert_ga"></a>`insert_ga` — inserts Google Analytics snippet (ga.js is retired)
- <a id="make_google_analytics_async"></a>`make_google_analytics_async` — targets retired ga.js
- <a id="make_show_ads_async"></a>`make_show_ads_async` — targets deprecated AdSense show_ads.js

## See also

- [Filters Overview](/docs/filters-overview/) — filters organized by category with descriptions
- [Filter Selection](/docs/filter-selection/) — how to enable and disable filters
