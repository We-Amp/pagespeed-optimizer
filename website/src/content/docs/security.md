---
title: 'Security'
description: 'Security guidance for mod_pagespeed 2.1: restricting admin pages, domain authorization, untrusted content, CSS/XSS, cache poisoning, HTTPS, and CVE patching.'
order: 38
group: 'Operate'
lastUpdated: 2026-09-06
---

## Overview

mod_pagespeed rewrites your HTML and the resources it references. That rewriting touches a few areas with security implications in production: admin-page access, domain authorization, and the handling of untrusted content. Each is covered below.

## Restrict admin page access

The admin pages at `/pagespeed_admin/` can purge caches and reveal configuration details. The process-wide `/pagespeed_global_admin/` endpoint is at least as sensitive. Always restrict access in production. See [Admin Console](/docs/admin-console/) for setup.

### Hardening admin endpoints

Gate `/pagespeed_admin` and `/pagespeed_global_admin` at the web server's normalized location/handler layer — `location =` / `^~` on nginx, `<Location>` on Apache, the handler-bound check on IIS. A WAF or upstream filter that string-matches the literal URL is bypassable via path-normalization tricks (`//`, `/./`, `%2e`, mixed case, trailing slash, `;param`). Copy-pasteable snippets and the full bypass-class list live in [Admin Console — URL-path ACLs are brittle](/docs/admin-console/#url-path-acls-are-brittle).

## Domain authorization

mod_pagespeed only fetches resources from explicitly authorized domains. This prevents it from being used as an open proxy. Authorize only domains you control. See [Domain Configuration](/docs/domain-configuration/).

## Untrusted content

mod_pagespeed rewrites URLs and inlines content. If your site serves user-generated HTML (forums, CMSes), consider:

- Inline filters (`inline_css`, `inline_javascript`) will inline resources from authorized domains into the page. If a user can inject `<link>` or `<script>` tags pointing to authorized domains, the inlined content appears in the HTML response.
- Use `ForbidFilters` to disable inlining in sections serving untrusted content.

## CSS and XSS

CSS can contain `url()` references and, in older browsers, expressions. mod_pagespeed's CSS rewriting preserves these constructs. If untrusted users can inject CSS into your pages, the rewritten CSS will still contain the injected content.

## Cache poisoning

mod_pagespeed caches optimized resources keyed by URL. If an attacker can manipulate request parameters that affect page content (but not the cache key), they could potentially poison the cache. mod_pagespeed mitigates this by including relevant request properties in cache keys.

## HTTPS

mod_pagespeed verifies SSL certificates when fetching HTTPS resources. Do not disable certificate verification in production. See [HTTPS Configuration](/docs/https-configuration/).

## Security patches

mod_pagespeed 2.1 includes patches for all known CVEs from the open-source project. We-Amp maintains an ongoing security review process. If you discover a security issue, report it to [security@modpagespeed.com](mailto:security@modpagespeed.com) or through the [security policy](/security/).

## See also

- [Admin Console](/docs/admin-console/) — restricting admin access
- [Domain Configuration](/docs/domain-configuration/) — domain authorization
- [HTTPS Configuration](/docs/https-configuration/) — SSL/TLS setup
- [Configuring Content-Security-Policy with mod_pagespeed](/blog/mod-pagespeed-content-security-policy/) — keeping optimization CSP-safe
