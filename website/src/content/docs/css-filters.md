---
title: 'CSS Filters'
description: 'Reference for CSS optimization filters in mod_pagespeed 2.1: minify, combine, inline, and flatten @import CSS, plus critical-CSS extraction. Apache, nginx, and IIS syntax with tuning parameters.'
order: 44
group: 'Filters'
lastUpdated: 2026-10-06
---

## Overview

mod_pagespeed 2.1 includes filters for CSS minification, combining, inlining, import flattening, and critical CSS extraction. Several CSS filters are CoreFilters and run by default. The remaining filters can be enabled individually or through OptimizeForBandwidth mode.

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```text
pagespeed EnableFilters rewrite_css,combine_css
pagespeed CssInlineMaxBytes 4096
```

See [IIS Configuration](/docs/iis-configuration/) for the full file format reference.

## Quick reference

| Filter                                                           | Core | OFB | Description                                           | Safe           |
| ---------------------------------------------------------------- | ---- | --- | ----------------------------------------------------- | -------------- |
| [`rewrite_css`](#rewrite_css)                                    | Yes  | Yes | Minifies CSS and rewrites embedded URLs               | Generally safe |
| [`fallback_rewrite_css_urls`](#fallback_rewrite_css_urls)        | Yes  | No  | Rewrites URLs in unparseable CSS                      | Generally safe |
| [`rewrite_style_attributes`](#rewrite_style_attributes)          | No   | No  | Rewrites inline `style` attributes                    | Generally safe |
| [`rewrite_style_attributes_with_url`](#rewrite_style_attributes) | Yes  | No  | Rewrites inline `style` attributes containing `url()` | Generally safe |
| [`combine_css`](#combine_css)                                    | Yes  | No  | Combines multiple CSS files into one                  | Generally safe |
| [`flatten_css_imports`](#flatten_css_imports)                    | Yes  | No  | Inlines `@import` rules                               | Generally safe |
| [`inline_css`](#inline_css)                                      | Yes  | No  | Inlines small external CSS into HTML                  | Generally safe |
| [`inline_import_to_link`](#inline_import_to_link)                | Yes  | No  | Converts `@import` in `<style>` to `<link>`           | Generally safe |
| [`inline_google_font_css`](#inline_google_font_css)              | No   | No  | Inlines Google Fonts CSS                              | Generally safe |
| [`outline_css`](#outline_css)                                    | No   | No  | Externalizes large inline CSS                         | Experimental   |
| [`prioritize_critical_css`](#prioritize_critical_css)            | No   | No  | Inlines the CSS a page uses, loads the rest async     | Test first     |
| [`move_css_above_scripts`](#move_css_above_scripts)              | No   | No  | Moves CSS `<link>` above `<script>` elements          | Generally safe |
| [`move_css_to_head`](#move_css_to_head)                          | No   | No  | Moves CSS `<link>` elements into `<head>`             | Generally safe |

## Filter details

### rewrite_css {#rewrite_css}

Core filter. Minifies CSS by removing whitespace, comments, and shortening property values. Also rewrites embedded image URLs so they go through mod_pagespeed's image optimization pipeline. In OptimizeForBandwidth mode, minifies CSS in-place without changing the URL.

Enable:

```apache
# Apache
ModPagespeedEnableFilters rewrite_css
```

```nginx
# Nginx
pagespeed EnableFilters rewrite_css;
```

### fallback_rewrite_css_urls {#fallback_rewrite_css_urls}

Core filter. Rewrites resource URLs embedded in CSS files even when CSS parsing fails. Acts as a safety net for non-standard CSS that the full parser cannot handle.

Enable:

```apache
# Apache
ModPagespeedEnableFilters fallback_rewrite_css_urls
```

```nginx
# Nginx
pagespeed EnableFilters fallback_rewrite_css_urls;
```

### rewrite_style_attributes / rewrite_style_attributes_with_url {#rewrite_style_attributes}

`rewrite_style_attributes` applies CSS rewriting (minification, URL rewriting) to inline `style=""` attributes on HTML elements. `rewrite_style_attributes_with_url` (Core filter) does the same but only when the style value contains a `url()` reference.

Enable:

```apache
# Apache
ModPagespeedEnableFilters rewrite_style_attributes
# or, for URL-only (enabled by default as a CoreFilter):
ModPagespeedEnableFilters rewrite_style_attributes_with_url
```

```nginx
# Nginx
pagespeed EnableFilters rewrite_style_attributes;
# or, for URL-only (enabled by default as a CoreFilter):
pagespeed EnableFilters rewrite_style_attributes_with_url;
```

### combine_css {#combine_css}

Core filter. Combines multiple `<link rel="stylesheet">` elements into a single CSS file, reducing HTTP requests. Each combined file groups stylesheets that appear consecutively in the HTML. A `<script>` tag or other non-CSS element between two `<link>` tags breaks the combination boundary.

Enable:

```apache
# Apache
ModPagespeedEnableFilters combine_css
```

```nginx
# Nginx
pagespeed EnableFilters combine_css;
```

### flatten_css_imports {#flatten_css_imports}

Core filter. Replaces CSS `@import` rules with the contents of the imported file. Eliminates round trips caused by import chains. The `CssFlattenMaxBytes` parameter (default: 1024000) limits the size of the resulting flattened CSS. Flattening is trickier than plain concatenation — media queries, charset rules, and relative URLs all have to survive the merge; see [flattening CSS @imports](/blog/flatten-css-imports-edge-cases/) for the edge cases.

Enable:

```apache
# Apache
ModPagespeedEnableFilters flatten_css_imports
```

```nginx
# Nginx
pagespeed EnableFilters flatten_css_imports;
```

### inline_css {#inline_css}

Core filter. Inlines small external CSS files directly into the HTML as `<style>` blocks. The `CssInlineMaxBytes` parameter (default: 2048) controls the size threshold.

Enable:

```apache
# Apache
ModPagespeedEnableFilters inline_css
```

```nginx
# Nginx
pagespeed EnableFilters inline_css;
```

### inline_import_to_link {#inline_import_to_link}

Core filter. Converts `<style>@import url(...);</style>` to `<link rel="stylesheet">`, enabling other CSS filters (combining, minification) to process the imported stylesheet.

Enable:

```apache
# Apache
ModPagespeedEnableFilters inline_import_to_link
```

```nginx
# Nginx
pagespeed EnableFilters inline_import_to_link;
```

### inline_google_font_css {#inline_google_font_css}

Not a core filter. Fetches the CSS from the Google Fonts API and inlines it directly into the HTML, eliminating one round trip. Requires HTTPS fetching to be enabled. GDPR considerations apply in the EU: inlining the CSS avoids the browser contacting Google Fonts servers directly, which can help with compliance.

Enable:

```apache
# Apache
ModPagespeedEnableFilters inline_google_font_css
```

```nginx
# Nginx
pagespeed EnableFilters inline_google_font_css;
```

### outline_css {#outline_css}

Experimental. The inverse of `inline_css`: externalizes large inline `<style>` blocks into separate CSS files that can be cached independently. Rarely useful in practice. The `CssOutlineMinBytes` parameter (default: 3000) sets the minimum inline CSS size to externalize.

Enable:

```apache
# Apache
ModPagespeedEnableFilters outline_css
```

```nginx
# Nginx
pagespeed EnableFilters outline_css;
```

### prioritize_critical_css {#prioritize_critical_css}

Not a core filter. Test before deploying. Inlines the CSS rules a page uses and loads each full stylesheet without blocking the first paint. The full stylesheet is preloaded from the place its `<link>` had in the page and takes effect there as soon as it has arrived, so the order in which your rules apply does not change; a `<noscript>` copy of the link covers visitors without scripts, and inline `<style>` blocks are left as they are. By default the inlined rules cover every element in the page as visitors' browsers last saw it, so content further down the page is styled from the first paint too. Uses a JavaScript beacon to collect critical CSS data from real user visits. The beacon endpoint must be accessible for data collection to work. Can cut perceived load time, but test it against your own page layouts first. In v1.15.0+r18 and later, the filter honors a restrictive `Content-Security-Policy` when `HonorCsp` is enabled: on pages whose policy disallows inline styles or scripts, it passes the page through unchanged instead of injecting content the policy would block. For the trade-offs behind critical-CSS extraction, see [how critical CSS is identified](/blog/critical-css-heuristics/).

Enable:

```apache
# Apache
ModPagespeedEnableFilters prioritize_critical_css
```

```nginx
# Nginx
pagespeed EnableFilters prioritize_critical_css;
```

If you prefer a smaller inline block and accept that content below the first screen may be partly styled until the full stylesheet arrives, turn on `CriticalCssAboveTheFoldOnly` (off by default): `ModPagespeedCriticalCssAboveTheFoldOnly on` on Apache, `pagespeed CriticalCssAboveTheFoldOnly on;` on nginx, `pagespeed CriticalCssAboveTheFoldOnly on` on IIS.

**What to expect.** After you deploy a changed stylesheet, the filter leaves that page's stylesheets blocking for a few page views, until visitors' browsers have reported on the new rules. After a page's markup changes without its stylesheets changing, rules that newly apply can be late until a visitor's browser reports on the new markup; the module asks for a report again after about a minute by default (twelve times `BeaconReinstrumentTimeSec`), and the wait is longer when the visitor who is asked does not report. The browser reports which rules the page uses once the page has loaded, so content that a script removes, hides or gives other class names before then can be painted without the rules that applied only to its earlier state, until the full stylesheet arrives. Content a script adds while the page is loading takes its rules from the full stylesheet. The full stylesheet is turned on by a small inline script: if something in front of your server delays inline scripts, the full styles arrive when that script runs. For pages whose markup differs from visitor to visitor under one URL, leave the filter off (`DisableFilters prioritize_critical_css` for that location).

A stylesheet keeps its ordinary blocking `<link>` when it uses an `@import` the server cannot merge into it, when nearly all of it would be inline anyway, or when its `<link>` carries an event-handler attribute, a `title` or `disabled`. A page with more matching selectors than one report can carry keeps its blocking stylesheets until a complete report arrives (see the `beacon_overflow_count` statistic). The filter needs the beacon and therefore does nothing on Envoy.

### move_css_above_scripts {#move_css_above_scripts}

Not a core filter. Moves `<link rel="stylesheet">` elements above `<script>` elements in the HTML to prevent CSS-blocking-JS render delays. Generally safe for most sites.

Enable:

```apache
# Apache
ModPagespeedEnableFilters move_css_above_scripts
```

```nginx
# Nginx
pagespeed EnableFilters move_css_above_scripts;
```

### move_css_to_head {#move_css_to_head}

Not a core filter. Moves `<link rel="stylesheet">` elements from the `<body>` into `<head>` for earlier browser discovery and faster rendering. Generally safe for most sites.

Enable:

```apache
# Apache
ModPagespeedEnableFilters move_css_to_head
```

```nginx
# Nginx
pagespeed EnableFilters move_css_to_head;
```

## Tuning parameters

| Parameter                | Default | Description                                                         |
| ------------------------ | ------- | ------------------------------------------------------------------- |
| `CssInlineMaxBytes`      | 2048    | Max CSS file size (bytes) to inline into HTML                       |
| `CssFlattenMaxBytes`     | 1024000 | Max size (bytes) of flattened CSS after resolving `@import`         |
| `CssOutlineMinBytes`     | 3000    | Min inline CSS size (bytes) to externalize                          |
| `CssImageInlineMaxBytes` | 0       | Max image size (bytes) to data-URI inline within CSS (0 = disabled) |

Apache syntax:

```apache
ModPagespeedCssInlineMaxBytes 4096
ModPagespeedCssFlattenMaxBytes 204800
ModPagespeedCssOutlineMinBytes 5000
ModPagespeedCssImageInlineMaxBytes 2048
```

Nginx syntax:

```nginx
pagespeed CssInlineMaxBytes 4096;
pagespeed CssFlattenMaxBytes 204800;
pagespeed CssOutlineMinBytes 5000;
pagespeed CssImageInlineMaxBytes 2048;
```

## See also

- [Filter Selection](/docs/filter-selection/)
- [Filter Reference](/docs/filter-reference/)
- [JavaScript Filters](/docs/javascript-filters/) — the matching minify, combine, and inline filters for JS
- [How CSS parsing works](/how-it-works/css-parsing/) — the syntax-tree layer beneath the CSS filters
