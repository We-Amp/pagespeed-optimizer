---
title: 'Domain Configuration'
description: 'Authorize and map domains in mod_pagespeed 1.15 on nginx, Apache, and IIS. Reference for the Domain, MapOriginDomain, MapRewriteDomain, MapProxyDomain, ShardDomain, and LoadFromFile directives.'
order: 12
group: 'Configuration'
lastUpdated: 2026-07-04
---

mod_pagespeed rewrites and optimizes resources referenced by HTML pages. To do this safely, it must know which domains it is allowed to fetch from and how domain names map between the public-facing URL and the actual origin.

## Domain authorization

mod_pagespeed only fetches and optimizes resources from authorized domains. By default, the domain that serves the HTML page is authorized automatically. Resources hosted on any other domain are left untouched unless that domain is explicitly authorized.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed Domain cdn.example.com;
pagespeed Domain *.example.com;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedDomain cdn.example.com
ModPagespeedDomain *.example.com
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed Domain cdn.example.com
pagespeed Domain *.example.com
```

</div>

Wildcards are supported. A wildcard entry like `*.example.com` authorizes all subdomains under `example.com`. Without authorization, mod_pagespeed ignores resources from unlisted domains entirely — it does not fetch, rewrite, or cache them.

## MapOriginDomain

Maps a public-facing domain to a different origin server for fetching. Use this when mod_pagespeed sits behind a load balancer, CDN, or reverse proxy and the public hostname does not resolve to the machine running mod_pagespeed.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MapOriginDomain localhost www.example.com;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMapOriginDomain localhost www.example.com
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MapOriginDomain localhost www.example.com
```

</div>

This directive tells mod_pagespeed to fetch resources for `www.example.com` from `localhost` instead. The rewritten URLs in the HTML still reference `www.example.com` — only the fetch target changes. This implicitly authorizes the origin domain.

## MapRewriteDomain

Rewrites resource URLs in the HTML output to use a different domain. Use this to serve optimized resources from a CDN or a cookie-free domain.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MapRewriteDomain cdn.example.com www.example.com;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMapRewriteDomain cdn.example.com www.example.com
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MapRewriteDomain cdn.example.com www.example.com
```

</div>

Resources originally referenced on `www.example.com` will have their URLs rewritten to `cdn.example.com` in the HTML output. The target domain (`cdn.example.com`) must serve the same optimized resources — typically by proxying back to the mod_pagespeed server or by using `MapOriginDomain` on the CDN's origin.

## ShardDomain

Distributes optimized resource URLs across multiple domains. Browsers enforce a per-domain connection limit under HTTP/1.1, so spreading resources across shards increases parallel downloads. This technique is less relevant with HTTP/2, which multiplexes requests over a single connection.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed ShardDomain www.example.com shard1.example.com,shard2.example.com;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedShardDomain www.example.com shard1.example.com,shard2.example.com
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed ShardDomain www.example.com shard1.example.com,shard2.example.com
```

</div>

All shard domains must be authorized (explicitly or via `MapOriginDomain`) and must serve the same content as the primary domain. mod_pagespeed assigns resources to shards deterministically based on the resource URL, so a given resource always maps to the same shard.

## MapProxyDomain

Proxies and optimizes content from an entirely separate origin through the mod_pagespeed server. This effectively creates a reverse proxy for the mapped domain.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MapProxyDomain www.example.com/external https://other-site.example.com;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMapProxyDomain www.example.com/external https://other-site.example.com
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MapProxyDomain www.example.com/external https://other-site.example.com
```

</div>

Requests to `www.example.com/external/` are fetched from `https://other-site.example.com/`, optimized, and served through the local server. Use this directive with caution — it exposes the mod_pagespeed server as a proxy for external content.

## LoadFromFile

Loads resources directly from the local filesystem instead of fetching them over HTTP. This eliminates fetch latency and HTTP overhead for resources that are stored on the same machine.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed LoadFromFile "http://www.example.com/static/" "/var/www/static/";
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedLoadFromFile "http://www.example.com/static/" "/var/www/static/"
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed LoadFromFile "http://www.example.com/static/" "C:\inetpub\wwwroot\static\"
```

Windows-style backslash paths are converted to forward slashes internally.

</div>

The first argument is the URL prefix. The second is the filesystem path that corresponds to it. Resources loaded from files bypass the HTTP cache entirely. mod_pagespeed watches the filesystem for changes and re-optimizes when files are modified.

## See also

- [Configuration](/1.1/docs/configuration/) — general configuration reference
- [Caching](/1.1/docs/caching/) — how domain configuration affects caching behavior
- [Directive Index](/1.1/docs/directive-index/) — the full list of mod_pagespeed 1.15 directives
