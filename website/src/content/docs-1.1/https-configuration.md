---
title: 'HTTPS Configuration'
description: 'Fetch HTTPS resources in mod_pagespeed 1.15 on nginx, Apache, and IIS: FetchHttps with SSL certificate verification, MapOriginDomain to an HTTP backend, or LoadFromFile.'
order: 13
group: 'Configuration'
lastUpdated: 2026-07-12
---

mod_pagespeed fetches resources (CSS, JavaScript, images) from your site to optimize them. When your site serves pages over HTTPS, mod_pagespeed must be able to make HTTPS connections to fetch those resources.

Three approaches are available, from simplest to most flexible.

## Approach 1: FetchHttps

The simplest option. mod_pagespeed fetches HTTPS resources directly using its built-in HTTP client. `FetchHttps` defaults to `enable`, so direct HTTPS fetching works out of the box; set it explicitly if you need one of the other keywords (`disable`, `allow_self_signed`, `allow_unknown_certificate_authority`, `allow_certificate_not_yet_valid`).

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed FetchHttps enable;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedFetchHttps enable
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed FetchHttps enable
```

The 1.15 IIS module uses WinHTTP for HTTPS fetching. SSL certificate verification uses the Windows certificate store automatically — no `SslCertDirectory` or `SslCertFile` directives are needed.

</div>

By default, mod_pagespeed verifies SSL certificates. If certificate verification fails, configure the certificate directory or file explicitly:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed SslCertDirectory /etc/ssl/certs;
pagespeed SslCertFile /etc/ssl/certs/ca-certificates.crt;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedSslCertDirectory /etc/ssl/certs
ModPagespeedSslCertFile /etc/ssl/certs/ca-certificates.crt
```

</div>

Do not disable certificate verification in production. For development environments with self-signed certificates, set the certificate directory to the location of your self-signed CA.

On nginx, since v1.15.0+r18 the native fetcher (`pagespeed UseNativeFetcher on;` in the `http` block) also fetches HTTPS resources directly, using nginx's own event loop and TLS stack; `NativeFetcherMaxKeepaliveRequests` (default 100) caps requests per keepalive connection.

## Approach 2: MapOriginDomain with HTTP backend

If mod_pagespeed runs on the same server as the origin, use [`MapOriginDomain`](/1.1/docs/domain-configuration/) to map the HTTPS domain to a local HTTP backend:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MapOriginDomain "http://localhost" "https://www.example.com";
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMapOriginDomain "http://localhost" "https://www.example.com"
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MapOriginDomain "http://localhost" "https://www.example.com"
```

</div>

This avoids HTTPS fetch overhead entirely. mod_pagespeed fetches from `http://localhost` instead of making an HTTPS connection to the public domain. The rewritten resource URLs still use `https://www.example.com` as seen by the browser.

## Approach 3: LoadFromFile

Load resources directly from the filesystem, bypassing network fetches altogether:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed LoadFromFile "https://www.example.com/static/" "/var/www/static/";
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedLoadFromFile "https://www.example.com/static/" "/var/www/static/"
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed LoadFromFile "https://www.example.com/static/" "C:\inetpub\wwwroot\static\"
```

Note the Windows-style path. The module converts backslashes to forward slashes internally.

</div>

No network fetch occurs. mod_pagespeed reads the files from disk. This is the fastest option for static assets that are available on the local filesystem.

## Mixed content

mod_pagespeed rewrites resource URLs to match the scheme of the page. Pages served over HTTPS will have their optimized resources served over HTTPS as well. This prevents mixed-content warnings in browsers.

## See also

- [Domain Configuration](/1.1/docs/domain-configuration/) — domain authorization and mapping
- [Configuration](/1.1/docs/configuration/) — general configuration reference
