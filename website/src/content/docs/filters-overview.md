---
title: 'Filters Overview'
description: 'The complete mod_pagespeed 2.1 filter set, grouped by category — image, CSS, JavaScript, HTML, and caching filters — with what each one does and how to enable it.'
order: 40
group: 'Filters'
lastUpdated: 2026-07-04
---

mod_pagespeed 2.1 organizes its optimizations as filters. Each filter performs a specific transformation on HTML responses as they pass through the web server. Enable filters individually with directives, or activate them in groups via `RewriteLevel` presets (`CoreFilters`, `OptimizeForBandwidth`). See [filter selection](/docs/filter-selection/) for how to configure them, or the [complete filter reference](/docs/filter-reference/) for every filter and its default level.

> **See these filters live.** The [optimization examples gallery](/examples/) runs each filter on a small page through Apache + mod_pagespeed 1.15 — original vs optimized, side by side, with the source diff and measured byte/request savings.

## Image filters

Image filters reduce image payload by recompressing, converting to more efficient formats, resizing to rendered dimensions, and inlining small images as data URIs. On image-heavy pages, these filters usually account for the bulk of the bytes saved.

- [`rewrite_images`](/docs/image-filters/#rewrite_images) (core) — master image optimization that combines recompression, format conversion, and resizing
- [`convert_jpeg_to_progressive`](/docs/image-filters/#convert_formats), [`convert_jpeg_to_webp`](/docs/image-filters/#convert_formats), [`convert_png_to_jpeg`](/docs/image-filters/#convert_formats) — format conversion for smaller file sizes
- [`recompress_images`](/docs/image-filters/#recompress_images) — lossless recompression without changing format
- [`resize_images`](/docs/image-filters/#resize_images), [`responsive_images`](/docs/image-filters/#responsive_images) — dimension-aware optimization based on rendered size or `srcset`
- [`inline_images`](/docs/image-filters/#inline_images), [`lazyload_images`](/docs/image-filters/#lazyload_images) — delivery optimization: inline small images as data URIs or defer offscreen image loading

Full reference: [Image filters](/docs/image-filters/)

## CSS filters

CSS filters reduce stylesheet size and the number of HTTP requests required to load them. They minify CSS, combine separate stylesheets into fewer files, inline small stylesheets directly into the HTML, and flatten `@import` chains.

- [`rewrite_css`](/docs/css-filters/#rewrite_css) (core) — minification and whitespace removal
- [`combine_css`](/docs/css-filters/#combine_css) (core) — merge multiple `<link>` elements into a single request
- [`inline_css`](/docs/css-filters/#inline_css) (core) — inline small stylesheets into `<style>` elements
- [`flatten_css_imports`](/docs/css-filters/#flatten_css_imports) (core) — resolve `@import` rules into a single stylesheet
- [`prioritize_critical_css`](/docs/css-filters/#prioritize_critical_css) — extract and inline above-the-fold CSS, deferring the rest

Full reference: [CSS filters](/docs/css-filters/)

## JavaScript filters

JavaScript filters reduce script size and control when scripts execute. They minify source, combine separate scripts to reduce round trips, and defer execution until the page has loaded.

- [`rewrite_javascript`](/docs/javascript-filters/#rewrite_javascript) (core) — minification and whitespace removal
- [`combine_javascript`](/docs/javascript-filters/#combine_javascript) (core) — merge multiple `<script>` elements into a single request
- [`inline_javascript`](/docs/javascript-filters/#inline_javascript) (core) — inline small scripts directly into the HTML
- [`defer_javascript`](/docs/javascript-filters/#defer_javascript) — defer script execution until after the page finishes loading

Full reference: [JavaScript filters](/docs/javascript-filters/)

## HTML filters

HTML filters optimize the HTML document itself. They remove unnecessary whitespace and comments, add performance hints such as DNS prefetch and preload headers, and fix structural issues in the document.

- [`collapse_whitespace`](/docs/html-filters/#collapse_whitespace), [`remove_comments`](/docs/html-filters/#remove_comments) — HTML minification by removing non-significant whitespace and comments
- [`insert_dns_prefetch`](/docs/html-filters/#insert_dns_prefetch), [`hint_preload_subresources`](/docs/html-filters/#hint_preload_subresources) — performance hints that enable early resource discovery
- [`add_head`](/docs/html-filters/#add_head), [`convert_meta_tags`](/docs/html-filters/#convert_meta_tags) (core) — structural fixes that ensure a well-formed document

Full reference: [HTML filters](/docs/html-filters/)

## Caching and URL filters

Caching filters extend browser cache lifetimes by rewriting resource URLs to include content hashes. This allows setting long `Cache-Control` max-age values while ensuring that browsers fetch updated resources when content changes.

- [`extend_cache`](/1.1/docs/caching-url-filters/#extend_cache) (core) — rewrite CSS, JavaScript, and image URLs with content-based hashes and set long TTLs
- [`extend_cache_pdfs`](/1.1/docs/caching-url-filters/#extend_cache_pdfs) — apply the same content-hashed URL strategy to PDF links

Full reference: [Caching and URL filters](/1.1/docs/caching-url-filters/)

## Quick reference

For a complete table of all 66 filters with their default levels and descriptions, see the [filter reference](/docs/filter-reference/).

## Deprecated filters

Certain filters that targeted Google-specific services remain available for backward compatibility but are not recommended:

- `insert_ga` — Google Analytics snippet injection
- `make_google_analytics_async` — asynchronous GA conversion
- `canonicalize_javascript_libraries` — rewrite to Google Hosted Libraries CDN

The services these filters target have been retired or significantly changed. New deployments should not enable them.
