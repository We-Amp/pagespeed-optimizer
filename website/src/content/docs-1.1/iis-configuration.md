---
title: 'IIS Configuration'
description: 'Configure mod_pagespeed 1.15 on IIS via pagespeed.config — the native IIS module and successor to IISpeed. Directive syntax, path-based regex matching, server and site-level config, and environment-variable expansion.'
order: 36
group: 'IIS'
lastUpdated: 2026-07-12
---

mod_pagespeed 1.15 runs on IIS as a native, in-process module — the successor to IISpeed. This page documents the `pagespeed.config` file format it reads. For the directives that apply across every platform (Apache, nginx, IIS), see [Configuration](/1.1/docs/configuration/); to install the module first, see [Getting Started](/1.1/docs/getting-started/).

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
%ProgramData%\We-Amp\IISWebSpeed\pagespeed.config
```

This is typically `C:\ProgramData\We-Amp\IISWebSpeed\pagespeed.config`. Settings here apply to all websites on the server.

### Site-level config

```
<website root>\pagespeed.config
```

Place a `pagespeed.config` file in the website's physical root directory (e.g., `C:\inetpub\wwwroot\pagespeed.config`). Site-level settings override server-level settings.

### Lookup order

1. Check for `pagespeed.config` in the website root
2. If not found, check for `iiswebspeed.config` in the website root (legacy fallback)
3. Load the server-level `pagespeed.config` (or `iiswebspeed.config`) as the base configuration
4. Merge site-level settings on top of server-level settings

### Migration note

If you are migrating from IISpeed, the module accepts both `pagespeed.config` (preferred) and `iiswebspeed.config` (legacy). When both files exist in the same directory, `pagespeed.config` takes priority. Rename your `iiswebspeed.config` to `pagespeed.config` when convenient — no content changes are needed.

## Cache directory

The IIS worker process needs write access to the cache directory set by `FileCachePath`. The installer creates the default cache directory and grants the worker identity the access it needs, so a standard install requires no manual setup.

### Automatic cache-directory creation

From **v1.1.0+r11** onward, the module creates each website's cache subdirectory on first request and grants the worker identity write access — no manual `mkdir` or permission step. This works whether `FileCachePath` is under the `%ProgramData%\We-Amp\PageSpeed\` tree or the legacy `%ProgramData%\We-Amp\IISWebSpeed\` tree, so a cache path inherited from an IISpeed install keeps working after an upgrade with no configuration change.

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

- [Configuration](/1.1/docs/configuration/) — general configuration reference (all platforms)
- [Filter Selection](/1.1/docs/filter-selection/) — choosing and tuning filters
- [IIS Tuning](/1.1/docs/iis-tuning/) — IIS-specific web server tuning
- [Getting Started](/1.1/docs/getting-started/) — installation guide
