---
title: 'IIS Tuning'
description: 'IIS performance tuning for mod_pagespeed 1.15 on Windows Server: request filtering for combined resources, static vs dynamic gzip, MIME types, CDN and ARR caching, and TCP slow start — all via web.config.'
order: 35
group: 'IIS'
lastUpdated: 2026-07-12
---

IIS has its own performance controls that sit alongside mod_pagespeed 1.15. Configure them through IIS Manager or `web.config` — not through the [`pagespeed.config` file](/1.1/docs/iis-configuration/) that controls mod_pagespeed itself. This guide covers the settings worth changing when mod_pagespeed is active: request filtering for combined resources, gzip compression, CDN integration, Application Request Routing, and TCP slow start.

## Request filtering for combined resources

If you enable the [`combine_css`](/1.1/docs/css-filters/#combine_css) or [`combine_javascript`](/1.1/docs/javascript-filters/#combine_javascript) filters, the combined resource URLs contain a `+` character — for example `a.css+b.css.pagespeed.cc.HASH.css`. IIS request filtering rejects these as double-escaped and returns **HTTP 404.11** unless you allow them in the site's `web.config`:

```xml
<system.webServer>
  <security>
    <requestFiltering allowDoubleEscaping="true" />
  </security>
</system.webServer>
```

The installer ships this snippet as a `web.config.sample` file in the installation folder. If you do not use the combine filters, this setting is not required.

## Gzip compression

### Static vs dynamic compression

IIS supports two types of gzip compression: static (pre-compressed files cached to disk) and dynamic (compressed on the fly). When mod_pagespeed is active, disable static compression and enable dynamic compression:

```xml
<system.webServer>
  <urlCompression doStaticCompression="false" doDynamicCompression="true" />
</system.webServer>
```

**Why disable static compression?** mod_pagespeed rewrites resource URLs with content hashes. Static compression caches compressed files by URL, but mod_pagespeed's URL rewriting means the compressed cache is rarely hit. Dynamic compression handles the constantly changing URLs more efficiently.

### Gzip MIME types

Add MIME types for file formats that benefit from compression but are not compressed by IIS by default:

```xml
<system.webServer>
  <staticContent>
    <remove fileExtension=".woff" />
    <mimeMap fileExtension=".woff" mimeType="application/x-woff" />
    <remove fileExtension=".svg" />
    <mimeMap fileExtension=".svg" mimeType="image/svg+xml" />
    <remove fileExtension=".svgz" />
    <mimeMap fileExtension=".svgz" mimeType="image/svg+xml" />
    <remove fileExtension=".json" />
    <mimeMap fileExtension=".json" mimeType="application/json" />
  </staticContent>
</system.webServer>
```

## CDN integration

### Gzip through proxies

By default, IIS disables compression for HTTP/1.0 clients and proxied requests. CDNs typically use HTTP/1.1 but may present a `Via` header that triggers IIS's proxy detection. Override this behavior:

```xml
<system.webServer>
  <httpCompression noCompressionForHttp10="false" noCompressionForProxies="false" />
</system.webServer>
```

### CORS headers for CDN-served fonts

If your CDN serves web fonts from a different domain, browsers require CORS headers. Add the `Access-Control-Allow-Origin` header:

```xml
<system.webServer>
  <httpProtocol>
    <customHeaders>
      <add name="Access-Control-Allow-Origin" value="*" />
    </customHeaders>
  </httpProtocol>
</system.webServer>
```

Restrict the `value` to your specific domain instead of `*` if your security policy requires it.

## Application Request Routing (ARR)

ARR turns IIS into a reverse proxy with disk-based caching. When combined with mod_pagespeed, ARR can cache optimized resources at the edge and reduce load on the backend.

### How it works with mod_pagespeed

1. mod_pagespeed's [`extend_cache`](/1.1/docs/caching-url-filters/#extend_cache) filter rewrites cacheable resource URLs to include content hashes, extending their cache lifetime to one year.
2. ARR caches these long-lived resources. After the first request, subsequent requests are served directly from ARR's disk cache without reaching the backend.
3. The backend handles only dynamic HTML responses and cache misses.

### Setup

Download ARR from Microsoft. Key configuration:

- Enable disk caching in ARR's proxy settings
- Set the cache drive to an SSD for best performance
- Configure the backend server farm to point to your mod_pagespeed-enabled application servers

ARR respects standard `Cache-Control` headers. mod_pagespeed sets appropriate caching headers on optimized resources automatically.

## TCP slow start

TCP slow start controls how quickly a new connection ramps up its sending rate. A larger initial congestion window (ICW) allows more data in the first round trip, which benefits page load times.

All supported Windows Server versions (2019 and later) default to an initial congestion window of 10 (ICW10). No configuration change is needed.

## See also

- [Getting Started](/1.1/docs/getting-started/) — IIS installation guide
- [Configuration](/1.1/docs/configuration/) — general configuration reference
- [IIS Configuration](/1.1/docs/iis-configuration/) — pagespeed.config format reference
