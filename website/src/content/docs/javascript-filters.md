---
title: 'JavaScript Filters'
description: 'Minify, combine, inline, and defer JavaScript in mod_pagespeed 2.1. Directives, defaults, and the trade-offs for each JS filter on Apache, nginx, and IIS.'
order: 45
group: 'Filters'
lastUpdated: 2026-07-27
---

## Overview

mod_pagespeed 2.1 includes filters for JavaScript minification, combining, inlining, and deferring execution. Three JS filters are CoreFilters: `rewrite_javascript`, `combine_javascript`, and `inline_javascript`.

## Quick reference

| Filter                                              | Core | OFB | Description                                          | Safe         |
| --------------------------------------------------- | ---- | --- | ---------------------------------------------------- | ------------ |
| [`rewrite_javascript`](#rewrite_javascript)         | Yes  | Yes | Minifies JS                                          | Yes          |
| `rewrite_javascript_external`                       | Yes  | Yes | Implied by `rewrite_javascript`, external files only | Yes          |
| `rewrite_javascript_inline`                         | Yes  | Yes | Implied by `rewrite_javascript`, inline scripts only | Yes          |
| [`combine_javascript`](#combine_javascript)         | Yes  | No  | Combines multiple scripts                            | Yes          |
| [`inline_javascript`](#inline_javascript)           | Yes  | No  | Inlines small scripts                                | Yes          |
| [`defer_javascript`](#defer_javascript)             | No   | No  | Defers execution                                     | Test first   |
| [`outline_javascript`](#outline_javascript)         | No   | No  | Externalizes large inline scripts                    | Experimental |
| [`include_js_source_maps`](#include_js_source_maps) | No   | No  | Preserves source maps                                | Yes          |

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```
pagespeed EnableFilters rewrite_javascript,combine_javascript
pagespeed JsInlineMaxBytes 2048
```

See [IIS Configuration](/docs/iis-configuration/) for the full file format reference.

## rewrite_javascript {#rewrite_javascript}

Core filter. Minifies JavaScript by removing whitespace, comments, and shortening variable names where safe. In OFB mode, minifies in-place. The sub-filters `rewrite_javascript_external` and `rewrite_javascript_inline` control scope but are implicitly enabled by the parent filter.

The minifier is conservative around edge cases that change behavior — see [how safe JavaScript minification handles automatic semicolon insertion](/blog/safe-javascript-minification-semicolon-insertion/). Since v1.15.0+r21 there is a single minifier — the tokenizer-based one — and files containing template literals (backtick strings) minify normally. The `UseExperimentalJsMinifier` directive that previously selected it is deprecated: it is accepted for compatibility but ignored, and logs a deprecation warning at configuration load (`ModPagespeedUseExperimentalJsMinifier` on Apache, `pagespeed UseExperimentalJsMinifier` on nginx). Remove it from your configuration.

**Apache:**

```apacheconf
ModPagespeedEnableFilters rewrite_javascript
```

**Nginx:**

```nginx
pagespeed EnableFilters rewrite_javascript;
```

## combine_javascript {#combine_javascript}

Core filter. Combines multiple `<script src>` elements into a single file. Like [`combine_css`](/docs/css-filters/#combine_css), combination boundaries are broken by inline scripts or other non-script elements between script tags.

**Apache:**

```apacheconf
ModPagespeedEnableFilters combine_javascript
```

**Nginx:**

```nginx
pagespeed EnableFilters combine_javascript;
```

## inline_javascript {#inline_javascript}

Core filter. Inlines small external JS files into the HTML. `JsInlineMaxBytes` (default: 2048) controls the threshold.

**Apache:**

```apacheconf
ModPagespeedEnableFilters inline_javascript
ModPagespeedJsInlineMaxBytes 2048
```

**Nginx:**

```nginx
pagespeed EnableFilters inline_javascript;
pagespeed JsInlineMaxBytes 2048;
```

## defer_javascript {#defer_javascript}

Not a core filter. Test thoroughly before enabling. Defers execution of all JavaScript until after the page finishes loading. This can dramatically improve initial render time but will break scripts that rely on executing during page parse (e.g., `document.write`).

**Apache:**

```apacheconf
ModPagespeedEnableFilters defer_javascript
```

**Nginx:**

```nginx
pagespeed EnableFilters defer_javascript;
```

### Risks

- Scripts using `document.write` will fail.
- Scripts that expect to run before `DOMContentLoaded` may break.
- Order-dependent scripts may execute in unexpected order.
- Inserts a `<noscript>` redirect by default. Disable with `SupportNoScriptEnabled false`.
- Since v1.15.0+r18, with `HonorCsp` (default on) the filter stands down entirely on pages whose `Content-Security-Policy` forbids inline scripts — deferral relies on injected inline JavaScript, which such a policy would block.
- Since v1.15.0+r21, `defer_javascript` — along with `disable_javascript`, `defer_iframe`, `fix_reflow`, and the shared `support_noscript` fallback — switches off for clients identified as automated, including clients that send no `User-Agent`; those clients receive the page's normal authored script markup. Search-engine crawlers are included deliberately. An automated client presenting a browser's exact user-agent string is indistinguishable from that browser and still receives the deferred form.

## outline_javascript {#outline_javascript}

Experimental. Externalizes large inline `<script>` blocks into separate files. `JsOutlineMinBytes` (default: 3000) controls the threshold. Rarely useful -- most sites benefit more from inlining.

**Apache:**

```apacheconf
ModPagespeedEnableFilters outline_javascript
ModPagespeedJsOutlineMinBytes 3000
```

**Nginx:**

```nginx
pagespeed EnableFilters outline_javascript;
pagespeed JsOutlineMinBytes 3000;
```

## include_js_source_maps {#include_js_source_maps}

Not a core filter. Preserves JavaScript source maps through minification by adding a `//# sourceMappingURL=` comment pointing to the original source map. Enable this if you need to debug minified JavaScript in production.

**Apache:**

```apacheconf
ModPagespeedEnableFilters include_js_source_maps
```

**Nginx:**

```nginx
pagespeed EnableFilters include_js_source_maps;
```

## Tuning parameters

| Parameter           | Default | Description                               |
| ------------------- | ------- | ----------------------------------------- |
| `JsInlineMaxBytes`  | 2048    | Max JS file size (bytes) to inline        |
| `JsOutlineMinBytes` | 3000    | Min inline JS size (bytes) to externalize |

**Apache:**

```apacheconf
ModPagespeedJsInlineMaxBytes 2048
ModPagespeedJsOutlineMinBytes 3000
```

**Nginx:**

```nginx
pagespeed JsInlineMaxBytes 2048;
pagespeed JsOutlineMinBytes 3000;
```

## See also

- [Filter Selection](/docs/filter-selection/)
- [Filter Reference](/docs/filter-reference/)
- [Safe JavaScript minification and semicolon insertion](/blog/safe-javascript-minification-semicolon-insertion/)
- [Remove unused JavaScript with Chrome coverage](/blog/remove-unused-javascript-chrome-coverage/)
