---
title: 'Configuration'
description: 'Configure mod_pagespeed 1.15 for nginx, Apache, IIS, and Envoy: enabling the module, module states, core directives, per-location and virtual-host scoping, and reverse-proxy setup.'
order: 10
group: 'Configuration'
lastUpdated: 2026-07-25
---

mod_pagespeed 1.15 is the in-process native module for nginx, Apache, and IIS, with an experimental Envoy filter. This page covers enabling it, the three module states, the core directives, per-location and virtual-host scoping, and running behind a reverse proxy. To choose which optimizations to run, see [filter selection](/1.1/docs/filter-selection/).

## Enabling the module

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

Load the module and enable it in your server block:

```nginx
load_module modules/ngx_pagespeed_module.so;

http {
    server {
        pagespeed on;

        # Required location blocks
        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
        }

        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon$" { }
    }
}
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

The distribution package loads the module automatically on install. Enable it in `pagespeed.conf` or your virtual host configuration:

```apache
ModPagespeed on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

The installer registers the module in IIS automatically. Configure it through `pagespeed.config`, a flat-text configuration file with one directive per line.

The module looks for configuration files in this order:

1. **Site-level:** `pagespeed.config` in the website root directory
2. **Server-level:** `%ProgramData%\We-Amp\PageSpeed\pagespeed.config`

Site-level settings override server-level settings. If `pagespeed.config` is not found, the module falls back to `iiswebspeed.config` (legacy filename).

Enable optimization by adding to your `pagespeed.config`:

```
pagespeed on
```

</div>

### Envoy

mod_pagespeed 1.15 is available as an HTTP filter for Envoy. This integration is experimental. Contact us for configuration guidance.

## Module states

mod_pagespeed 1.15 supports three operational states:

| State       | Behavior                                                                                                                                                                                                  |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `on`        | Full optimization. The module rewrites HTML and serves optimized resources.                                                                                                                               |
| `standby`   | Serves previously optimized `.pagespeed.` resources and responds to query-parameter requests, but does not optimize new traffic. Use this to drain optimized resources before fully disabling the module. |
| `unplugged` | Fully disabled. The module does not intercept any requests. This state can only be set at the top level or within a virtual host block, not in directory-level configuration.                             |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed standby;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeed standby
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed standby
```

</div>

## Configuration directives

The table below lists key directives with their defaults. For the complete list, see the [directive index](/1.1/docs/directive-index/).

In v1.15.0+r18 and later, configuration validation is stricter: out-of-range values for bounded options (image quality levels, progressive JPEG scan counts, `HttpCacheCompressionLevel`, `RewriteRandomDropPercentage`, `CentralControllerPort`) fail configuration load instead of being silently accepted, and an invalid filter name in `?PageSpeedFilters=` rejects the whole query. In addition, the `AddResourceHeader` limit of 20 headers is now enforced exactly, and directive and option-scope matching is case-consistent. A configuration that loaded on an earlier revision may need its values corrected when upgrading.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

In nginx, prefix each directive with `pagespeed` and terminate with a semicolon.

```nginx
pagespeed HonorCsp on;
pagespeed RespectVary on;
pagespeed DisableRewriteOnNoTransform on;
pagespeed LowercaseHtmlNames off;
pagespeed ModifyCachingHeaders on;
pagespeed XHeaderValue "Powered by mod_pagespeed";
pagespeed PreserveUrlRelativity off;
pagespeed StaticAssetPrefix "/pagespeed_static/";
pagespeed AddResourceHeader "X-Custom" "value";
pagespeed ListOutstandingUrlsOnError off;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

In Apache, prefix each directive with `ModPagespeed`.

```apache
ModPagespeedHonorCsp on
ModPagespeedRespectVary on
ModPagespeedDisableRewriteOnNoTransform on
ModPagespeedLowercaseHtmlNames off
ModPagespeedModifyCachingHeaders on
ModPagespeedXHeaderValue "Powered by mod_pagespeed"
ModPagespeedPreserveUrlRelativity off
ModPagespeedStaticAssetPrefix "/pagespeed_static/"
ModPagespeedAddResourceHeader "X-Custom" "value"
ModPagespeedListOutstandingUrlsOnError off
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

In IIS, use `pagespeed.config` with one directive per line. Prefix each directive with `pagespeed`.

```
pagespeed HonorCsp on
pagespeed RespectVary on
pagespeed DisableRewriteOnNoTransform on
pagespeed LowercaseHtmlNames off
pagespeed ModifyCachingHeaders on
pagespeed XHeaderValue "Powered by mod_pagespeed"
pagespeed PreserveUrlRelativity off
pagespeed StaticAssetPrefix "/pagespeed_static/"
pagespeed AddResourceHeader "X-Custom" "value"
pagespeed ListOutstandingUrlsOnError off
```

The `pagespeed.config` format also accepts `ModPagespeed` and `iispeed` as directive prefixes for compatibility. The `pagespeed` prefix is recommended.

</div>

| Directive                     | Default              | Description                                                                                                                                                                                                                 |
| ----------------------------- | -------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `HonorCsp`                    | `on`                 | Enabled by default. mod_pagespeed reads `Content-Security-Policy` response headers and meta tags and suppresses optimizations the policy would block, such as inlining resources or injecting scripts. Set to `off` to optimize without consulting CSP. Verify pages that rely on strict policies. In v1.15.0+r18 and later, filters that inject inline scripts also back off cleanly when the policy disallows inline script.          |
| `RespectVary`                 | `off`                | When enabled, mod_pagespeed respects `Vary` headers on resources. Resources with `Vary` headers that indicate per-request variation are not rewritten.                                                                      |
| `DisableRewriteOnNoTransform` | `on`                 | When enabled, mod_pagespeed does not optimize resources served with `Cache-Control: no-transform`.                                                                                                                          |
| `LowercaseHtmlNames`          | `off`                | When enabled, mod_pagespeed lowercases all HTML tag and attribute names during parsing.                                                                                                                                     |
| `ModifyCachingHeaders`        | `on`                 | Controls whether mod_pagespeed sets caching headers on HTML responses. Do not disable this unless you fully understand the interaction with downstream caches. Disabling it can cause stale optimized content to be served. |
| `XHeaderValue`                | `(version string)`   | Sets the value of the `X-Mod-Pagespeed` (Apache) or `X-Page-Speed` (nginx/IIS) [response header](/pagespeed-markers/#x-page-speed).                                                                                          |
| `PreserveUrlRelativity`       | `on`                 | When enabled, mod_pagespeed preserves the relativity of URLs in rewritten HTML. Relative URLs remain relative rather than being converted to absolute URLs.                                                                 |
| `StaticAssetPrefix`           | `/pagespeed_static/` | Sets the URL prefix for mod_pagespeed's static assets (JavaScript libraries, images used by filters).                                                                                                                       |
| `AddResourceHeader`           | _(none)_             | Adds a custom HTTP header to all optimized resources. This directive is repeatable: specify it multiple times to add multiple headers (up to 20).                                                                                      |
| `ListOutstandingUrlsOnError`  | `off`                | When enabled, mod_pagespeed includes a list of outstanding resource fetch URLs in error responses. Enable this only for debugging; do not use in production.                                                                |

## Optimization threads {#optimization-threads}

Optimization work runs on two thread pools, separate from the threads that serve
requests. The *rewrite* pool handles short, latency-sensitive bookkeeping; the
*expensive rewrite* pool handles heavy CPU work such as image transcoding, so a
large image cannot hold up everything else.

In v1.15.0+r21 and later both pools size themselves. `NumRewriteThreads` and
`NumExpensiveRewriteThreads` default to `auto`, and mod_pagespeed resolves them
at startup by:

1. Taking the CPUs the process is **actually permitted to use** — not the
   host's core count. A CPU quota (a container limit or a systemd unit) and a
   CPU affinity mask both count. A 2-CPU container on a 64-core host sees 2.
2. Allowing optimization at most **half** of those.
3. Dividing by the number of **peer processes** the server is configured to run,
   because each process builds its own pools. Without this step a server with
   many children would multiply its thread count by the number of children.
4. Giving the result to **each** pool, never fewer than one thread.

The resolved counts, and the numbers they were derived from, are written to the
error log at startup.

### What this means per server

<div data-platform="apache" data-platform-label="Apache">

Apache reports its configured child-process ceiling, so the divisor is real:
`MaxRequestWorkers / ThreadsPerChild` on `worker` and `event`, and
`MaxRequestWorkers` on `prefork` — in both cases capped by `ServerLimit`. If you
have lowered `ServerLimit`, that is the number in effect, not the division.

A stock `event` configuration allows 400 workers at 25 threads per child, so the
divisor is 16 and a server resolves to **one thread per pool** unless it has a
great many cores. That is the intended answer: the ceiling counts children the
server is *allowed* to start, and sizing each child as though it were alone on
the machine is what oversubscribes it. A server deliberately configured with few
children on a many-core machine gets proportionally more.

`prefork` is not threaded and resolves to one thread per pool, as it always has.

</div>

<div data-platform="nginx" data-platform-label="nginx">

nginx does not yet report a process count to the module, so it resolves to
**one thread per pool** and logs that it fell back. It never assumes a larger
divisor: guessing high would silently oversubscribe the machine, which is worse
than being conservative.

This is unchanged from earlier releases. If you have set `NumRewriteThreads` or
`NumExpensiveRewriteThreads` explicitly, your values continue to apply exactly
as before.

</div>

<div data-platform="iis" data-platform-label="IIS">

The IIS module sizes its own pools and does not yet consult these directives.
Its behaviour is unchanged in this release. See
[IIS tuning](/1.1/docs/iis-tuning/).

</div>

### Setting the counts explicitly

An explicit positive value overrides the computed one entirely.

| Value              | Effect                                                              |
| ------------------ | ------------------------------------------------------------------- |
| `auto`             | Resolve as described above. This is the default.                    |
| `0`                | An alias for `auto`.                                                |
| positive integer   | Use exactly this many threads per process, ignoring the computation. |
| negative           | Rejected when the configuration is read.                             |

A negative value is rejected at configuration-read time rather than being
clamped, which on Apache means the server does not start. Very large explicit
values are capped, with a warning naming both the value you asked for and the
value in effect.

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedNumRewriteThreads 4
ModPagespeedNumExpensiveRewriteThreads 4
```

</div>

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed NumRewriteThreads 4;
pagespeed NumExpensiveRewriteThreads 4;
```

</div>

Both directives are **global**. They cannot be set per virtual host or per
location: the pools are built once per process, before any virtual host is
consulted.

### When to change them

Most servers should leave these alone. Reach for them when the log line at
startup shows a resolved count that does not match your deployment — most often
a single-process server on a many-core machine, or a server whose configured
child ceiling is far above the number of children it will really run.

If you raise them, watch request latency rather than only optimization
throughput. The reason optimization is capped at half the machine is that the
remaining half is serving traffic.

## Location-specific configuration

mod_pagespeed directives can be scoped to specific parts of your site. The available scoping mechanisms vary by platform.

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

Apache supports configuration at several levels:

- **`pagespeed.conf`** — Global defaults that apply to all virtual hosts.
- **`<VirtualHost>`** — Per-site overrides.
- **`<Directory>` and `<Location>`** — Target specific filesystem paths or URL paths.
- **`.htaccess`** — Per-directory configuration placed in the document root or subdirectories.

`.htaccess` configuration is re-read on every request, which adds per-request overhead. For high-traffic sites, prefer `<Directory>` or `<Location>` blocks in the server configuration.

Example using `<Location>`:

```apache
<Location /images/>
    ModPagespeedDisableFilters convert_jpeg_to_webp
</Location>
```

</div>

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

In nginx, use `server` and `location` blocks to scope directives:

```nginx
server {
    pagespeed on;

    location /static/ {
        pagespeed off;
    }
}
```

Directives set in a `server` block apply to all locations within that server unless overridden by a more specific `location` block.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

IIS supports configuration at two levels:

- **Server-level:** `%ProgramData%\We-Amp\PageSpeed\pagespeed.config` — applies to all websites on the server.
- **Site-level:** `pagespeed.config` in the website root directory — overrides server-level settings for that site.

Within a configuration file, use match rules to scope directives to specific URLs or hostnames:

```
# Only optimize requests matching this host
hostname: ^www\.example\.com$

pagespeed on
pagespeed EnableFilters rewrite_images

# Match a specific URL path
path: ^/blog/

pagespeed EnableFilters prioritize_critical_css
```

Match rules use RE2 regular expressions. The `hostname` and `path` keys match against the request hostname and URL path respectively.

</div>

## Virtual hosts

Each virtual host can carry its own mod_pagespeed configuration. Directives set at the global level serve as defaults; virtual host configuration overrides them.

One requirement: HTML pages and the optimized resources they reference must share the same configuration options. If a virtual host serves HTML that references resources optimized under a different set of options, the resources may not be found or may be re-optimized unnecessarily.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
server {
    server_name site-a.example.com;
    pagespeed on;
    pagespeed EnableFilters rewrite_images;
}

server {
    server_name site-b.example.com;
    pagespeed on;
    pagespeed EnableFilters collapse_whitespace;
}
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
<VirtualHost *:80>
    ServerName site-a.example.com
    ModPagespeed on
    ModPagespeedEnableFilters rewrite_images
</VirtualHost>

<VirtualHost *:80>
    ServerName site-b.example.com
    ModPagespeed on
    ModPagespeedEnableFilters collapse_whitespace
</VirtualHost>
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

Place a `pagespeed.config` file in each website's root directory. Each file can carry its own set of directives:

**Site A** (`C:\inetpub\site-a\pagespeed.config`):

```
pagespeed on
pagespeed EnableFilters rewrite_images
```

**Site B** (`C:\inetpub\site-b\pagespeed.config`):

```
pagespeed on
pagespeed EnableFilters collapse_whitespace
```

Alternatively, use match rules in the server-level config to scope directives by hostname:

```
hostname: ^site-a\.example\.com$
pagespeed EnableFilters rewrite_images

hostname: ^site-b\.example\.com$
pagespeed EnableFilters collapse_whitespace
```

</div>

## URL segment length limits {#max-url-segments}

Set the maximum length (in characters) of any single URL segment — the text
between two `/` separators — that mod_pagespeed will produce when it combines
or rewrites resources. The default is `1024`.

Apache servers historically capped URL segments at about 250 characters per
segment. mod_pagespeed circumvents that limit when running under Apache, but
intermediate proxies or CDNs in front of your origin may re-impose it. If a
downstream component rejects long `.pagespeed.` URLs, lower this value so
mod_pagespeed produces shorter combined URLs.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MaxSegmentLength 250;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMaxSegmentLength 250
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MaxSegmentLength 250
```

</div>

This directive applies only to the URLs mod_pagespeed generates (for example,
when combining CSS or JavaScript). It does not limit which inbound request
URLs the module will rewrite.

## Reverse proxy configuration

When mod_pagespeed runs behind a reverse proxy (such as nginx, Varnish, or a CDN), keep the following in mind:

- The proxy must forward the original `Host` header to the backend so that mod_pagespeed generates correct URLs for optimized resources.
- If the proxy terminates TLS, configure mod_pagespeed to recognize the `X-Forwarded-Proto` header so that it generates `https://` URLs for optimized resources. See [HTTPS configuration](/1.1/docs/https-configuration/) for the details.
- Ensure that `.pagespeed.` resource URLs are routed to the backend running mod_pagespeed. The proxy must not cache these resources independently unless you configure cache lifetimes carefully.
- If the proxy strips or modifies response headers, verify that mod_pagespeed's `X-Mod-Pagespeed` or `X-Page-Speed` header and `Cache-Control` directives pass through intact.
