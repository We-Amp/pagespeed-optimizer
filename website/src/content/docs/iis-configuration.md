---
title: 'IIS Configuration'
description: 'Configure mod_pagespeed on IIS via pagespeed.config — the native IIS module and successor to IISpeed. Directive syntax, path-based regex matching, server and site-level config, and environment-variable expansion.'
order: 26
group: 'Configure'
lastUpdated: 2026-07-12
---

mod_pagespeed runs on IIS as a native, in-process module — the successor to IISpeed. This page documents the `pagespeed.config` file format it reads. For the directives that apply across every platform (Apache, nginx, IIS), see [Configuration](/docs/configuration/); to install the module first, see [Requirements](#requirements) below.

## Requirements

- Windows Server 2019 or later (IIS 10+)
- 64-bit only
- Visual C++ Redistributable 2022
- IIS worker process identity needs write permissions on the cache directory

## Install the module

Download the [IIS MSI installer](/download/) — each binary has a `.asc` signature next to it — and run it on your Windows Server. The installer registers the module in IIS as a native HTTP module automatically and creates the default cache directory at `%ProgramData%\We-Amp\IISWebSpeed\Cache`.

### Migrating from IISpeed

If IISpeed is installed on this host, **uninstall it before running the MSI**. The two modules register the same handler in IIS and cannot coexist — leaving IISpeed in place will cause one or both to fail at site startup. Open _Apps & features_ (or _Programs and Features_), remove "IISpeed", `iisreset`, and then run the installer.

If you are migrating from the open-source module on IIS:

1. Uninstall the existing IISpeed or mod_pagespeed module from IIS Manager
2. Install the module
3. Restart IIS: `iisreset`

### IIS Express

For local development with IIS Express, add the module to `applicationhost.config` in `%userprofile%\Documents\IISExpress\config\`:

1. Copy the module DLL to a known location
2. Add the module registration under `<globalModules>` and `<modules>`
3. Restart IIS Express

### Disable optimization

To disable optimization for a site, either delete or rename its `pagespeed.config` file, or add:

```
pagespeed off
```

### Verify it works

On IIS the module emits `X-Page-Speed`, the same as nginx — check for it with PowerShell:

```powershell
(Invoke-WebRequest http://localhost/ -Method Head).Headers["X-Page-Speed"]
```

## File format

`pagespeed.config` is a flat text file with one directive per line. Lines starting with `#` are comments.

Since v1.15.0+r18, config parsing is more robust and more diagnosable: a malformed line no longer prevents module startup, unknown options are reported instead of silently ignored, and option scoping is enforced.

```
# Enable optimization
pagespeed on

# Set the cache path
pagespeed FileCachePath %ProgramData%\We-Amp\IISWebSpeed\Cache

# Enable specific filters
pagespeed EnableFilters collapse_whitespace,remove_comments
pagespeed DisableFilters combine_css
```

### Directive prefixes

Three directive prefixes are accepted:

| Prefix         | Example                         |
| -------------- | ------------------------------- |
| `pagespeed`    | `pagespeed EnableFilters ...`   |
| `ModPagespeed` | `ModPagespeedEnableFilters ...` |
| `iispeed`      | `iispeed EnableFilters ...`     |

The `pagespeed` prefix is recommended. `ModPagespeed` and `iispeed` are accepted for compatibility with Apache configurations and legacy IISpeed installations.

## Config file locations

The module searches for configuration files in two locations:

### Server-level config

```
%ProgramData%\We-Amp\PageSpeed\pagespeed.config
```

The server-level file is `%ProgramData%\We-Amp\PageSpeed\pagespeed.config`. Installs upgraded from IISpeed also read `%ProgramData%\We-Amp\IISWebSpeed\pagespeed.config`. Settings here apply to all websites on the server.

### Site-level config

```
<website root>\pagespeed.config
```

Place a `pagespeed.config` file in the website's physical root directory (e.g., `C:\inetpub\wwwroot\pagespeed.config`). Site-level settings override server-level settings. The `FileCachePath` must be included in per-site configs if not set in the server-level config.

### Lookup order

1. Check for `pagespeed.config` in the website root
2. If not found, check for `iiswebspeed.config` in the website root (the filename used by IISpeed installs)
3. Load the server-level `pagespeed.config` (or `iiswebspeed.config`) as the base configuration
4. Merge site-level settings on top of server-level settings

### Migration note

If you are migrating from IISpeed, the module accepts both `pagespeed.config` (preferred) and `iiswebspeed.config` (the filename used by IISpeed installs). When both files exist in the same directory, `pagespeed.config` takes priority. Rename your `iiswebspeed.config` to `pagespeed.config` when convenient — no content changes are needed.

## Cache directory

The IIS worker process needs write access to the cache directory set by `FileCachePath`. The installer creates the default cache directory and grants the worker identity the access it needs, so a standard install requires no manual setup.

Set the path explicitly with `FileCachePath`:

```
pagespeed FileCachePath %ProgramData%\We-Amp\IISWebSpeed\Cache
```

To validate the cache path, make a request to any page on your server and check that files appear in the cache directory.

### Automatic cache-directory creation

From **v1.1.0+r11** onward, the module creates each website's cache subdirectory on first request and grants the worker identity write access — no manual `mkdir` or permission step. This works whether `FileCachePath` is under the `%ProgramData%\We-Amp\PageSpeed\` tree or the `%ProgramData%\We-Amp\IISWebSpeed\` tree used by installs upgraded from IISpeed, so a cache path inherited from an IISpeed install keeps working after an upgrade with no configuration change.

To require that the cache directory already exist instead, turn auto-creation off:

```
pagespeed AutoCreateCachePath off
```

### "FileCachePath does not exist"

A site that serves a local-only diagnostic page reporting that the configured `FileCachePath` does not exist is running a build from before automatic cache-directory creation (earlier than v1.1.0+r11). Upgrade to the current release and the directory is created for you. To fix it in place on an older build, create the directory and grant the worker identity — the app pool identity, or `IIS_IUSRS` — Modify access, then recycle the app pool.

## Environment variable expansion

Windows environment variables are expanded in path values:

```
pagespeed FileCachePath %ProgramData%\We-Amp\IISWebSpeed\Cache
pagespeed LogDir %ProgramData%\We-Amp\IISWebSpeed\Logs
```

Common variables:

| Variable        | Typical value              |
| --------------- | -------------------------- |
| `%ProgramData%` | `C:\ProgramData`           |
| `%SystemRoot%`  | `C:\Windows`               |
| `%TEMP%`        | `C:\Windows\TEMP` (system) |

Path separators: backslashes (`\`) are converted to forward slashes internally. Both formats work in the config file.

## Path-based matching with regex

Use match rules to apply different settings based on the request hostname or URL path. Match rules use [RE2 regular expressions](https://github.com/google/re2/wiki/Syntax).

### hostname

Match by hostname:

```
# Only optimize requests for this domain
hostname: ^www\.example\.com$

pagespeed on
pagespeed EnableFilters rewrite_images,rewrite_css
```

### path

Match by URL path:

```
# Aggressive optimization for the blog
path: ^/blog/

pagespeed EnableFilters prioritize_critical_css,defer_javascript

# Conservative optimization for the checkout
path: ^/checkout/

pagespeed RewriteLevel OptimizeForBandwidth
```

### Combining match rules

Match rules apply sequentially. Each rule sets a match context; directives following a rule apply only when that rule matches. A new match rule starts a new context.

```
# Server-wide defaults
pagespeed on
pagespeed RewriteLevel CoreFilters

# Site A: full optimization
hostname: ^site-a\.example\.com$
pagespeed EnableFilters collapse_whitespace,remove_comments

# Site B: bandwidth-only optimization
hostname: ^site-b\.example\.com$
pagespeed RewriteLevel OptimizeForBandwidth

# Blog section on any site: add critical CSS
path: ^/blog/
pagespeed EnableFilters prioritize_critical_css
```

### Clearing inherited settings

Use `clear` to reset accumulated options before applying new ones:

```
hostname: ^special\.example\.com$
clear
pagespeed on
pagespeed RewriteLevel PassThrough
pagespeed EnableFilters rewrite_images
```

The `clear` directive discards all previously accumulated options for this request, starting fresh with only the directives that follow.

## Custom fetch headers

Add custom headers to resource fetch requests using the `header_` prefix:

```
pagespeed header_X-PageSpeed-Fetch true
pagespeed header_Authorization "Bearer token123"
```

The `header_` prefix is stripped; the remaining text becomes the header name.

## Complete examples

### Minimal configuration

```
pagespeed on
pagespeed FileCachePath %ProgramData%\We-Amp\IISWebSpeed\Cache
```

### Production configuration

```
# Enable optimization with CoreFilters
pagespeed on
pagespeed FileCachePath %ProgramData%\We-Amp\IISWebSpeed\Cache
pagespeed FileCacheSizeKb 2097152

# Add image optimization and critical CSS
pagespeed EnableFilters prioritize_critical_css
pagespeed EnableFilters lazyload_images

# Disable combining (breaks some sites)
pagespeed DisableFilters combine_css,combine_javascript

# HTTPS resource fetching
pagespeed FetchHttps enable

# Admin interface (local access only)
pagespeed AdminPath /pagespeed_admin
pagespeed GlobalAdminPath /pagespeed_global_admin
pagespeed MessageBufferSize 100000
pagespeed StatisticsLogging on
pagespeed LogDir %ProgramData%\We-Amp\IISWebSpeed\Logs
```

### Multi-site configuration

```
# Server-wide defaults
pagespeed on
pagespeed FileCachePath %ProgramData%\We-Amp\IISWebSpeed\Cache
pagespeed RewriteLevel CoreFilters

# Marketing site: aggressive optimization
hostname: ^www\.example\.com$
pagespeed EnableFilters prioritize_critical_css,defer_javascript
pagespeed EnableFilters lazyload_images,collapse_whitespace

# API: no HTML optimization, bandwidth only
hostname: ^api\.example\.com$
pagespeed RewriteLevel OptimizeForBandwidth

# Admin portal: disable optimization
hostname: ^admin\.example\.com$
pagespeed off
```

## Configuration reload

The module checks the modification timestamp of `pagespeed.config` periodically (every 1000 ms). When a change is detected, the configuration is reloaded automatically without restarting IIS or recycling the app pool.

## See also

- [Configuration](/docs/configuration/) — general configuration reference (all platforms)
- [Filter Selection](/docs/filter-selection/) — choosing and tuning filters
- [IIS Tuning](/docs/iis-configuration/#iis-tuning) — IIS-specific web server tuning
- [Getting Started](/docs/getting-started/) — installation guide

## IIS tuning

IIS has its own performance controls that sit alongside mod_pagespeed 2.1. Configure them through IIS Manager or `web.config` — not through the [`pagespeed.config` file](/docs/iis-configuration/) that controls mod_pagespeed itself. This guide covers the settings worth changing when mod_pagespeed is active: request filtering for combined resources, gzip compression, CDN integration, Application Request Routing, and TCP slow start.

### Request filtering for combined resources

If you enable the [`combine_css`](/docs/css-filters/#combine_css) or [`combine_javascript`](/docs/javascript-filters/#combine_javascript) filters, the combined resource URLs contain a `+` character — for example `a.css+b.css.pagespeed.cc.HASH.css`. IIS request filtering rejects these as double-escaped and returns **HTTP 404.11** unless you allow them in the site's `web.config`:

```xml
<system.webServer>
  <security>
    <requestFiltering allowDoubleEscaping="true" />
  </security>
</system.webServer>
```

The installer ships this snippet as a `web.config.sample` file in the installation folder. If you do not use the combine filters, this setting is not required.

### Gzip compression

#### Static vs dynamic compression

IIS supports two types of gzip compression: static (pre-compressed files cached to disk) and dynamic (compressed on the fly). When mod_pagespeed is active, disable static compression and enable dynamic compression:

```xml
<system.webServer>
  <urlCompression doStaticCompression="false" doDynamicCompression="true" />
</system.webServer>
```

**Why disable static compression?** mod_pagespeed rewrites resource URLs with content hashes. Static compression caches compressed files by URL, but mod_pagespeed's URL rewriting means the compressed cache is rarely hit. Dynamic compression handles the constantly changing URLs more efficiently.

#### Gzip MIME types

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

### CDN integration

#### Gzip through proxies

By default, IIS disables compression for HTTP/1.0 clients and proxied requests. CDNs typically use HTTP/1.1 but may present a `Via` header that triggers IIS's proxy detection. Override this behavior:

```xml
<system.webServer>
  <httpCompression noCompressionForHttp10="false" noCompressionForProxies="false" />
</system.webServer>
```

#### CORS headers for CDN-served fonts

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

### Application Request Routing (ARR)

ARR turns IIS into a reverse proxy with disk-based caching. When combined with mod_pagespeed, ARR can cache optimized resources at the edge and reduce load on the backend.

#### How it works with mod_pagespeed

1. mod_pagespeed's [`extend_cache`](/docs/cache-control/#extend_cache) filter rewrites cacheable resource URLs to include content hashes, extending their cache lifetime to one year.
2. ARR caches these long-lived resources. After the first request, subsequent requests are served directly from ARR's disk cache without reaching the backend.
3. The backend handles only dynamic HTML responses and cache misses.

#### Setup

Download ARR from Microsoft. Key configuration:

- Enable disk caching in ARR's proxy settings
- Set the cache drive to an SSD for best performance
- Configure the backend server farm to point to your mod_pagespeed-enabled application servers

ARR respects standard `Cache-Control` headers. mod_pagespeed sets appropriate caching headers on optimized resources automatically.

### TCP slow start

TCP slow start controls how quickly a new connection ramps up its sending rate. A larger initial congestion window (ICW) allows more data in the first round trip, which benefits page load times.

All supported Windows Server versions (2019 and later) default to an initial congestion window of 10 (ICW10). No configuration change is needed.

### See also

- [Getting Started](/docs/getting-started/) — IIS installation guide
- [Configuration](/docs/configuration/) — general configuration reference
- [IIS Configuration](/docs/iis-configuration/) — pagespeed.config format reference
