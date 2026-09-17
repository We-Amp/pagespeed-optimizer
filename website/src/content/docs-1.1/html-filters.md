---
title: 'HTML Filters'
description: 'HTML optimization filters in mod_pagespeed 1.15: collapse whitespace, strip comments, elide attributes, DNS prefetch, and resource preload hints.'
order: 24
group: 'Filters'
lastUpdated: 2026-07-12
---

## Overview

mod_pagespeed 1.15 includes filters that optimize HTML structure, cut unnecessary bytes, and add performance hints. Two of them (`add_head`, `convert_meta_tags`) are CoreFilters and run by default; the rest are opt-in — see [Filter Selection](/1.1/docs/filter-selection/) to turn them on. For resources rather than markup, see the [CSS Filters](/1.1/docs/css-filters/), [JavaScript Filters](/1.1/docs/javascript-filters/), and [Image Filters](/1.1/docs/image-filters/) pages.

## Quick reference

| Filter                                                    | Core | Description                          | Safe           |
| --------------------------------------------------------- | ---- | ------------------------------------ | -------------- |
| [`add_head`](#add_head)                                   | Yes  | Adds `<head>` if missing             | Yes            |
| [`convert_meta_tags`](#convert_meta_tags)                 | Yes  | Converts meta http-equiv to headers  | Yes            |
| [`collapse_whitespace`](#collapse_whitespace)             | No   | Removes excess whitespace            | Generally safe |
| [`remove_comments`](#remove_comments)                     | No   | Strips HTML comments                 | Generally safe |
| [`elide_attributes`](#elide_attributes)                   | No   | Removes default-value attributes     | Generally safe |
| [`remove_quotes`](#remove_quotes)                         | No   | Removes unnecessary attribute quotes | Generally safe |
| [`trim_urls`](#trim_urls)                                 | No   | Shortens absolute URLs to relative   | Generally safe |
| [`combine_heads`](#combine_heads)                         | No   | Merges multiple `<head>` elements    | Generally safe |
| [`pedantic`](#pedantic)                                   | No   | Adds type attributes for HTML4       | Generally safe |
| [`insert_dns_prefetch`](#insert_dns_prefetch)             | No   | Adds DNS prefetch hints              | Generally safe |
| [`hint_preload_subresources`](#hint_preload_subresources) | No   | Adds preload headers                 | Generally safe |
| [`add_instrumentation`](#add_instrumentation)             | No   | Injects page load timing JS          | Test first     |

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```
pagespeed EnableFilters collapse_whitespace,remove_comments
```

See [IIS Configuration](/1.1/docs/iis-configuration/) for the full file format reference.

## CoreFilters

### add_head {#add_head}

Adds a `<head>` element if the HTML lacks one. Several other filters inject content into `<head>`, so this filter ensures one exists. Runs automatically as a CoreFilter.

### convert_meta_tags {#convert_meta_tags}

Reads `<meta http-equiv="Content-Type">` and similar tags and adds corresponding HTTP response headers. This helps browsers discover the content type and character encoding earlier in the response. Runs automatically as a CoreFilter.

## HTML minification filters

These filters reduce HTML payload size by removing unnecessary bytes.

### collapse_whitespace {#collapse_whitespace}

Removes excess whitespace from HTML. Preserves whitespace inside `<pre>`, `<script>`, `<style>`, and `<textarea>` elements. Never removes whitespace entirely between inline elements.

### remove_comments {#remove_comments}

Strips HTML comments from the page. Use `RetainComment` to keep specific comments matching a wildcard pattern.

Configuration for nginx:

```nginx
pagespeed RetainComment "*copyright*";
```

Configuration for Apache:

```apache
ModPagespeedRetainComment "*copyright*"
```

### elide_attributes {#elide_attributes}

Removes HTML attributes that are set to their default values. For example, `<form method="get">` becomes `<form>` because `get` is the default method.

### remove_quotes {#remove_quotes}

Removes unnecessary quotation marks around HTML attribute values when the value contains no special characters. Saves a few bytes per attribute.

### trim_urls {#trim_urls}

Shortens absolute URLs to relative URLs where the base URL matches the page URL. Reduces HTML payload at the cost of less portable HTML. Disable this filter if you serve the same HTML from multiple domains.

## Structural filters

### combine_heads {#combine_heads}

Merges multiple `<head>` elements into one. Only useful for pages that aggregate content from multiple sources, each contributing their own `<head>` section.

### pedantic {#pedantic}

Adds `type="text/javascript"` and `type="text/css"` attributes to `<script>` and `<style>` elements. This satisfies HTML4 validators. Not needed for HTML5, where these types are the defaults.

## Performance hint filters

### insert_dns_prefetch {#insert_dns_prefetch}

Adds `<link rel="dns-prefetch" href="//example.com">` tags for third-party domains referenced in the page. This allows the browser to resolve DNS for external domains in parallel with page loading, reducing latency for subsequent resource fetches.

### hint_preload_subresources {#hint_preload_subresources}

Adds `Link: rel=preload` HTTP headers for CSS and JavaScript files discovered on previous visits to the same page. Uses the beacon system to collect resource data, so it becomes effective after the first page view.

### insert_speculation_rules {#insert_speculation_rules}

Injects a same-origin prefetch `<script type="speculationrules">` block so that supporting browsers prefetch a link as the visitor starts interacting with it; browsers without speculation-rules support ignore the tag. Only same-origin links are eligible.

The filter stands down in several cases rather than injecting a ruleset that would be wasted or unsafe: it backs off when the page already carries its own speculation ruleset, when a `Content-Security-Policy` forbids inline scripts, on non-200 responses, on cookie-setting responses, on `no-store` responses, and on AMP documents.

Speculative prefetch spends origin bandwidth on navigations that may never happen, so treat it as a trade-off and test it against your own traffic. This filter is opt-in and is not part of any rewrite level. Enable it by name:

**Apache:**

```apache
ModPagespeedEnableFilters insert_speculation_rules
```

**nginx:**

```nginx
pagespeed EnableFilters insert_speculation_rules;
```

## add_instrumentation {#add_instrumentation}

Injects JavaScript that measures page load time and reports it back to the mod_pagespeed statistics system via [the beacon endpoint](/pagespeed-markers/#pagespeed-beacon) (`/mod_pagespeed_beacon` or `/ngx_pagespeed_beacon`). Enable this filter to get client-side performance data in the admin console histograms.

Test this filter before deploying to production. The injected JavaScript adds a small overhead and sends beacon requests on every page load.

Since v1.15.0+r18, with `HonorCsp` (default on) nothing is injected on pages whose `Content-Security-Policy` disallows the instrumentation script — no beacon fires for those pages, so they contribute no data to the console histograms.

## See also

- [Filters Overview](/1.1/docs/filters-overview/)
- [Filter Selection](/1.1/docs/filter-selection/)
- [Filter Reference](/1.1/docs/filter-reference/)
