---
title: 'HTML filters'
description: 'HTML optimization filters in mod_pagespeed 2.1: collapse whitespace, strip comments, elide attributes, DNS prefetch, and resource preload hints.'
order: 46
group: 'Filters'
lastUpdated: 2026-09-19
---

## Overview

mod_pagespeed 2.1 includes filters that optimize HTML structure, cut unnecessary bytes, and add performance hints. Two of them (`add_head`, `convert_meta_tags`) are CoreFilters and run by default; the rest are opt-in — see [Filter selection](/docs/filter-selection/) to turn them on. For resources rather than markup, see the [CSS filters](/docs/css-filters/), [JavaScript filters](/docs/javascript-filters/), and [Image filters](/docs/image-filters/) pages.

## Quick reference

| Filter                                                    | Core | Description                          | Safe               |
| --------------------------------------------------------- | ---- | ------------------------------------ | ------------------ |
| [`add_head`](#add_head)                                   | Yes  | Adds `<head>` if missing             | Yes                |
| [`convert_meta_tags`](#convert_meta_tags)                 | Yes  | Converts meta http-equiv to headers  | Yes                |
| [`collapse_whitespace`](#collapse_whitespace)             | No   | Removes excess whitespace            | Generally safe     |
| [`remove_comments`](#remove_comments)                     | No   | Strips HTML comments                 | Generally safe     |
| [`elide_attributes`](#elide_attributes)                   | No   | Removes default-value attributes     | Generally safe     |
| [`remove_quotes`](#remove_quotes)                         | No   | Removes unnecessary attribute quotes | Generally safe     |
| [`trim_urls`](#trim_urls)                                 | No   | Shortens absolute URLs to relative   | Generally safe     |
| [`combine_heads`](#combine_heads)                         | No   | Merges multiple `<head>` elements    | Generally safe     |
| [`pedantic`](#pedantic)                                   | No   | Adds type attributes for HTML4       | Generally safe     |
| [`insert_dns_prefetch`](#insert_dns_prefetch)             | No   | Adds DNS prefetch hints              | Generally safe     |
| [`hint_preload_subresources`](#hint_preload_subresources) | No   | Adds preload headers                 | Generally safe     |
| [`add_instrumentation`](#add_instrumentation)             | No   | Injects page load timing JS          | Test first         |
| [`add_base_tag`](#add_base_tag)                           | No   | Adds a `<base href>` for the page    | Test first         |
| [`add_ids`](#add_ids)                                     | No   | Adds ids to elements without one     | Test first         |
| [`insert_amp_link`](#insert_amp_link)                     | No   | Adds a `<link rel=amphtml>`          | Generally safe     |
| [`debug`](#debug)                                         | No   | Explains rewriting in comments       | Not for production |
| [`decode_rewritten_urls`](#decode_rewritten_urls)         | No   | Restores original resource URLs      | Test first         |
| [`compute_statistics`](#compute_statistics)               | No   | HTML statistics for the console      | Test first         |
| [`experiment_http2`](#experiment_http2)                   | No   | HTTP/2 features in development       | Experimental       |
| [`insert_ga`](#insert_ga)                                 | No   | Inserts the retired ga.js snippet    | Deprecated         |

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```text
pagespeed EnableFilters collapse_whitespace,remove_comments
```

See [IIS configuration](/docs/iis-configuration/) for the full file format reference.

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

<a id="left_trim_urls"></a>

Shortens absolute URLs to relative URLs where the base URL matches the page URL. Reduces HTML payload at the cost of less portable HTML. Disable this filter if you serve the same HTML from multiple domains.

## Structural filters

### add_base_tag {#add_base_tag}

Adds a `<base href="…">` element with the page's own URL to `<head>`, so relative URLs in the page resolve against the URL the module rewrote them for. Use it when the HTML is served at a URL other than the one it was authored for (proxy setups, `MapProxyDomain`). Risk: a page that already relies on a different base, or on the absence of one, resolves its relative links differently; test navigation and form actions.

```nginx
pagespeed EnableFilters add_base_tag;
```

### add_ids {#add_ids}

Adds an `id` attribute to elements that have none, so that beacon-driven filters can refer to individual elements across page loads. Rarely needed on its own: the filters that need ids enable it themselves. Risk: scripts or styles that count on the exact set of ids in the page see extra ones.

```nginx
pagespeed EnableFilters add_ids;
```

### combine_heads {#combine_heads}

Merges multiple `<head>` elements into one. Only useful for pages that aggregate content from multiple sources, each contributing their own `<head>` section.

### pedantic {#pedantic}

Adds `type="text/javascript"` and `type="text/css"` attributes to `<script>` and `<style>` elements. This satisfies HTML4 validators. Not needed for HTML5, where these types are the defaults.

## Performance hint filters

### insert_dns_prefetch {#insert_dns_prefetch}

Adds `<link rel="dns-prefetch" href="//example.com">` tags for third-party domains referenced in the page. This allows the browser to resolve DNS for external domains in parallel with page loading, reducing latency for subsequent resource fetches.

### hint_preload_subresources {#hint_preload_subresources}

Adds `Link: rel=preload` HTTP headers for CSS and JavaScript files discovered on previous visits to the same page. Uses the beacon system to collect resource data, so it becomes effective after the first page view.

Since v1.15.0+r21, `<script type="module">` subresources are hinted with `rel=modulepreload` in the `Link` response header instead of `rel=preload`; modules carrying `integrity` or `crossorigin="use-credentials"` are left unhinted.

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

### insert_amp_link {#insert_amp_link}

Adds a `<link rel="amphtml">` to `<head>` pointing at the page's AMP version, built from the `AmpLinkPattern` directive. Use it when you publish AMP pages at a predictable URL pattern and want every canonical page to announce its AMP twin. Risk: a pattern that produces URLs that do not exist advertises broken AMP pages to crawlers.

```nginx
pagespeed AmpLinkPattern "https://amp.example.com${url}";
pagespeed EnableFilters insert_amp_link;
```

## Debugging and measurement filters

### debug {#debug}

Annotates the page with HTML comments that say which filters ran and why a resource was or was not rewritten (for example why an image was not inlined or a stylesheet not combined). Enable it per request with `?PageSpeedFilters=+debug` while troubleshooting instead of in the configuration: it exposes internals, enlarges every page, and is not meant for production traffic.

```text
https://www.example.com/?PageSpeedFilters=+debug
```

### decode_rewritten_urls {#decode_rewritten_urls}

Turns `.pagespeed.` resource URLs in the page back into the original resource URLs, undoing the URL rewriting of the other filters. Useful in a proxy chain or when debugging what the page referenced before optimization. Risk: the page then references unoptimized resources, which defeats the filters that depend on rewritten URLs.

```nginx
pagespeed EnableFilters decode_rewritten_urls;
```

### compute_statistics {#compute_statistics}

Computes statistics about the HTML (element counts and sizes) for the [admin console](/docs/admin-console/). It adds a parsing pass per page and changes nothing in the output; enable it while you need the numbers.

```nginx
pagespeed EnableFilters compute_statistics;
```

### experiment_http2 {#experiment_http2}

Switches on HTTP/2-specific behavior that is still in development. Experimental: what it does can change between releases, and it is not covered by the compatibility promises of the other filters.

```nginx
pagespeed EnableFilters experiment_http2;
```

## Analytics filters

### insert_ga {#insert_ga}

Inserts the Google Analytics snippet for the account in `AnalyticsID` into every page. Deprecated: the snippet it inserts is the retired `ga.js`, and `AnalyticsID` itself only targets Universal Analytics, which was discontinued. Add your analytics in your templates instead.

## Deprecated and dangerous filters

The names below are still accepted so that an existing configuration keeps loading, with a warning. The deprecated ones do nothing at all; `fix_reflows` and `mobilize` are in the dangerous set, which `RewriteLevel AllFilters` never enables, and exist for experiments rather than production. Remove them from your configuration.

### cache_partial_html {#cache_partial_html}

Deprecated no-op.

### defer_iframe {#defer_iframe}

Deprecated no-op: iframe deferral is built into [`defer_javascript`](/docs/javascript-filters/#defer_javascript); enabling this name alone never did anything.

### div_structure {#div_structure}

Deprecated no-op.

### explicit_close_tags {#explicit_close_tags}

Deprecated no-op.

### flush_subresources {#flush_subresources}

Deprecated no-op.

### fix_reflows {#fix_reflows}

Experimental fix for layout reflows caused by deferred JavaScript. In the dangerous set; not for production.

### mobilize {#mobilize}

The retired page-mobilization experiment. In the dangerous set; not for production.

### mobilize_precompute {#mobilize_precompute}

Deprecated no-op.

### split_html {#split_html}

Deprecated no-op.

### split_html_helper {#split_html_helper}

Deprecated no-op.

## add_instrumentation {#add_instrumentation}

Injects JavaScript that measures page load time and reports it back to the mod_pagespeed statistics system via [the beacon endpoint](/pagespeed-markers/#pagespeed-beacon) (`/mod_pagespeed_beacon` or `/ngx_pagespeed_beacon`). Enable this filter to get client-side performance data in the admin console histograms.

Test this filter before deploying to production. The injected JavaScript adds a small overhead and sends beacon requests on every page load.

Since v1.15.0+r18, with `HonorCsp` (default on) nothing is injected on pages whose `Content-Security-Policy` disallows the instrumentation script — no beacon fires for those pages, so they contribute no data to the console histograms.

## See also

- [PageSpeed filters](/docs/filters/) — every filter in one table
- [Filter selection](/docs/filter-selection/)
