---
title: 'Configure ASP.NET Core middleware'
description: 'Configuration reference for the mod_pagespeed 2.1 ASP.NET Core middleware: appsettings.json PageSpeed options, cache and worker settings, and hot reload.'
order: 51
group: 'ASP.NET Core'
lastUpdated: 2026-09-19
---

The mod_pagespeed 2.1 ASP.NET Core middleware (`WeAmp.PageSpeed.AspNetCore`) is
configured from the standard ASP.NET Core sources: appsettings.json, environment
variables, or any `IConfiguration` provider. It binds its options to the
`PageSpeed` section via `IServiceCollection.AddPageSpeed(IConfiguration)`.

## Basic configuration

Add a `PageSpeed` section to your appsettings.json:

```json
{
  "PageSpeed": {
    "Enabled": true,
    "CacheMode": "Safe",
    "Cache": {
      "VolumePath": "/var/cache/pagespeed/volume.dat",
      "VolumeSizeBytes": 1073741824
    },
    "Html": {
      "EnableCriticalCss": true,
      "EnableLazyLoad": true,
      "EnableAsyncCss": false
    },
    "Worker": {
      "AutoStart": true
      // "ApiPort": 9191  // Worker management API, loopback only. The middleware
      // starts the worker with --api-no-auth, so never publish this port.
    }
  }
}
```

## Options

The middleware binds to a `PageSpeedOptions` instance with three nested sections:
`Cache`, `Html`, and `Worker`. All options support hot reload via
`IOptionsMonitor<PageSpeedOptions>`.

### Top-level (`PageSpeed`)

| Option                   | Type     | Default                                               | Description                                                                                                                                                                                                  |
| ------------------------ | -------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Enabled`                | bool     | `true`                                                | Master switch. When false, the middleware passes through all responses unchanged.                                                                                                                            |
| `CacheMode`              | enum     | `Safe`                                                | Cache-Control header strategy on cache HIT. `Safe` adds `must-revalidate` on assets; `Aggressive` adds `public, stale-if-error`. HTML always gets `no-cache`. See [Choose a cache mode](/docs/cache-modes/). |
| `CssMaxAgeSeconds`       | int      | `300`                                                 | Max-age for CSS and JS cache HIT responses, in seconds.                                                                                                                                                      |
| `ImageMaxAgeSeconds`     | int      | `1800`                                                | Max-age for image cache HIT responses, in seconds.                                                                                                                                                           |
| `HtmlMaxAgeSeconds`      | int      | `0`                                                   | Reserved. HTML always receives `no-cache` regardless of this value.                                                                                                                                          |
| `MaxResponseBufferBytes` | int      | `5242880` (5 MB)                                      | Max response body size to buffer. Larger responses pass through unmodified.                                                                                                                                  |
| `MaxAssetCacheBytes`     | int      | `10485760` (10 MB)                                    | Max asset (image/CSS/JS) size to cache. Larger assets pass through without caching or worker notification.                                                                                                   |
| `ExcludePaths`           | string[] | `["/api/", "/signalr/", "/_blazor/", "/_framework/"]` | URL path prefixes the middleware skips entirely. Override to add your own prefixes.                                                                                                                          |

### Cache (`PageSpeed:Cache`)

The cache is a single mmap'd Cyclone volume file — not a directory of fragments.

| Option                 | Type   | Default                           | Description                                                             |
| ---------------------- | ------ | --------------------------------- | ----------------------------------------------------------------------- |
| `VolumePath`           | string | `/var/cache/pagespeed/volume.dat` | Required. Path to the Cyclone cache volume file (created on first run). |
| `VolumeSizeBytes`      | ulong  | `1073741824` (1 GiB)              | Volume size in bytes.                                                   |
| `EnableChecksum`       | bool   | `true`                            | Verify checksums on cache reads.                                        |
| `RamCacheSizeBytes`    | long   | `67108864` (64 MiB)               | In-process RAM cache size in bytes. Set to `0` to disable.              |
| `MaxMetadataSizeBytes` | long   | `8192` (8 KiB)                    | Per-entry metadata size cap, in bytes.                                  |

### HTML processing (`PageSpeed:Html`)

Each rewrite is a boolean toggle — no rewrite-level abstraction, no filter strings.
Toggles default to "on" except `EnableSpeculationRules` and `EnableAsyncCss`,
which default to `false` in the middleware. `EnableAsyncCss` is opt-in: the
in-process middleware emits the async-CSS loader only when you set it to **true**
**and** critical-CSS inlining actually produces inlined rules for the page. The
toggle is a required precondition, not a label on behavior that happens anyway
(see the row below). The "on by default whenever critical CSS is present"
behavior belongs to the nginx/worker front-end, which is a different code path —
the in-process .NET middleware does not default async-CSS on.

| Option                   | Type | Default          | What it does                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| ------------------------ | ---- | ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `EnableCriticalCss`      | bool | `true`           | Extract above-the-fold CSS and inline it; defer the rest.                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `EnableLazyLoad`         | bool | `true`           | Add `loading="lazy"` to below-the-fold images. "Below the fold" is by DOM source order, not CSS layout, and how many leading body images are exempt depends on LCP detection. When a valid LCP candidate is found, that image gets `fetchpriority="high"` and body images #1–#3 are exempt as an above-fold safety guard (lazy starts at the 4th image). When there is no LCP candidate, only the first body image is protected (it gets `fetchpriority="high"`) and lazy-loading starts at the 2nd image. |
| `EnableImageDimensions`  | bool | `true`           | Add explicit `width`/`height` to images that omit them.                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `EnableLcpPreload`       | bool | `true`           | Emit a `<link rel="preload">` hint for the detected LCP image.                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `EnablePreconnect`       | bool | `true`           | Emit `<link rel="preconnect">` hints for referenced third-party origins.                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `EnableSpeculationRules` | bool | `false`          | Emit a speculation-rules script for in-page prefetching.                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `EnableAsyncCss`         | bool | `false`          | Defer render-blocking stylesheets via a content-hashed async loader. Defaults `false` (opt-in), because the host must also serve the loader path. The middleware emits the loader only when this is `true` **and** critical-CSS inlining produced inlined rules for the page — both conditions are required. (The nginx/worker front-end is a separate code path with its own, stricter condition: it defers only for a page whose above-the-fold appearance it has confirmed unchanged against the stylesheet being served. The in-process middleware has no browser and so cannot make that confirmation; its byte-based check plus this opt-in flag are what gate it.)                                                  |
| `EnableScriptDeferral`   | bool | `true`           | Defer scripts based on browser-coverage analysis. See [Browser analysis](/docs/browser-analysis/).                                                                                                                                                                                                                                                                                                                                                                                                         |
| `Viewport`               | enum | `Desktop`        | Viewport class used for capability-mask classification. `Mobile`, `Tablet`, or `Desktop`.                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `MaxHtmlSizeBytes`       | int  | `5242880` (5 MB) | Max HTML size to process. Larger documents pass through.                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `MaxCssSizeBytes`        | int  | `2097152` (2 MB) | Max CSS size to process. Larger stylesheets pass through.                                                                                                                                                                                                                                                                                                                                                                                                                                                  |

### Worker (`PageSpeed:Worker`)

The middleware optimizes inline; the worker process runs out-of-process for
expensive jobs (image transcoding, critical-CSS extraction) and exposes a
diagnostic console.

| Option       | Type    | Default | Description                                                                                                                                                                                                                                                                                                                                                            |
| ------------ | ------- | ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `AutoStart`  | bool    | `true`  | Resolve the `factory_worker` binary from the NuGet package and launch it as a child process on startup. Set `false` to launch no worker process at all.                                                                                                                                                                                                                |
| `SocketPath` | string? | unset   | Worker-coordination socket. **Leave it unset** (the default) and the middleware auto-resolves a per-process socket with coordination on — no configuration needed. Set it to a concrete path to share one socket with an out-of-process worker (split-process deployments). Setting it to `null` or `""` disables worker coordination and logs a loud startup warning. |
| `ApiPort`    | int     | `0`     | TCP port for the worker's HTTP management API. `0` disables the API. **Do not expose this port to the public internet — there is no authentication on this endpoint.**                                                                                                                                                                                                 |
| `LogLevel`   | string? | `null`  | Worker log level: `debug`, `info`, `warning`, or `error`. `null` keeps the worker's default (`info`).                                                                                                                                                                                                                                                                  |
| `ConsoleDir` | string? | `null`  | Filesystem path to the diagnostic-console SPA assets. When `null` and `AutoStart` is true, the middleware auto-discovers a `console` directory next to `factory_worker`. Set to `""` to disable.                                                                                                                                                                       |

The worker process exposes its diagnostic console out-of-process. Set
`Worker.ApiPort` to a port reachable from your ops team — but never from the
public internet — to enable it. The console SPA is served from `Worker.ConsoleDir`.

### Worker coordination and `SocketPath`

For the default in-process deployment, leave the `Worker` section empty. With
`AutoStart` `true` and `SocketPath` unset, the middleware launches the worker
and connects to it over an auto-resolved per-process socket — image and
critical-CSS optimization are on with no extra configuration.

There are two reasons to set `SocketPath` explicitly:

- **A concrete path** points the middleware at a socket you manage, so it can
  talk to a worker you run as a separate process (a split-process deployment
  where the worker lives in its own container or unit). The path is used
  verbatim.
- **`null` or an empty string** disables worker coordination entirely. The
  middleware still optimizes inline, but no out-of-process variants
  (image transcoding, critical-CSS extraction) are built, and the Dashboard
  stays at zero. The worker logs a startup warning so this state is never
  silent. Only set this if you specifically want inline-only behavior.

To run the worker as a fully separate process, pair a concrete `SocketPath`
with `AutoStart` `false` so the middleware connects to your externally managed
worker instead of launching its own.

## Hot reload

Configuration changes are picked up automatically through ASP.NET Core's
`IOptionsMonitor<PageSpeedOptions>` pattern. Edit appsettings.json while the
application is running — changes take effect on the next request. No restart
required.

This includes toggling the master switch, flipping any `Html.Enable*` toggle,
adjusting cache TTLs, and switching cache modes. Cache contents are preserved
across configuration changes. Worker-process options (`AutoStart`, `SocketPath`,
`ApiPort`) apply at the next worker (re)start.

Validation runs on startup and on every reload via `IValidateOptions<PageSpeedOptions>`.
Negative TTLs or unknown `CacheMode` values cause the application to fail fast
on startup, and reload errors are logged.

## Environment-specific configuration

Use ASP.NET Core's standard environment layering. Create
`appsettings.Development.json` to override settings during development:

```json
{
  "PageSpeed": {
    "Enabled": false
  }
}
```

This disables optimization in development while leaving the rest of the
pipeline intact. Production uses the base appsettings.json.

## Environment variables

All options can be set via environment variables using the standard ASP.NET Core
double-underscore convention. Nested keys use `__` as the separator:

```bash
PageSpeed__Enabled=true
PageSpeed__CacheMode=Aggressive
PageSpeed__Cache__VolumePath=/data/pagespeed/volume.dat
PageSpeed__Cache__VolumeSizeBytes=2147483648
PageSpeed__Html__EnableCriticalCss=true
PageSpeed__Html__EnableSpeculationRules=false
PageSpeed__Worker__ApiPort=0
```

Environment variables take precedence over appsettings.json, following ASP.NET
Core's standard configuration precedence.

## Rewrites you control

There is no rewrite-level abstraction and no filter-name list. Every rewrite is
a boolean toggle on `PageSpeed:Html` — see the table above. To match the
behaviour of a `PassThrough` configuration in 1.1, set `Enabled` to `false` (or
flip the individual `Enable*` toggles you want off). To bias toward
bandwidth-only rewrites, leave image and CSS toggles on and turn off
`EnableCriticalCss`, `EnableLcpPreload`, and `EnableSpeculationRules`. Turning
off `EnableCriticalCss` also suppresses async-CSS, since the middleware only
emits the async-CSS loader when critical-CSS inlining produces inlined rules
(and `EnableAsyncCss` already defaults `false`, so it is off unless you opt in).

Image transcoding (WebP, AVIF, resizing) and CSS/JS minification are handled by
the worker process and are not gated by `Html.Enable*` toggles. They run for
every cached asset.

## Cache volume

Optimized resources are stored in a single memory-mapped Cyclone volume file
defined by `Cache.VolumePath`. By default the volume lives at
`/var/cache/pagespeed/volume.dat` and is sized to 1 GiB. In containers or a
[production deployment](/docs/production-deployment/), point it at a persistent volume:

```json
{
  "PageSpeed": {
    "Cache": {
      "VolumePath": "/data/pagespeed/volume.dat",
      "VolumeSizeBytes": 4294967296
    }
  }
}
```

The parent directory must exist and be writable by the application process. The
volume file is created on first run and grown to `VolumeSizeBytes`. Reusing the
same file across restarts preserves the cache; pointing at a fresh path starts
cold.

## Health checks

The middleware participates in ASP.NET Core health checks. Expose the standard
endpoint to surface its status:

```csharp
app.MapHealthChecks("/healthz");
```

The check reports `Healthy`, with the cache entry count and the middleware
version, while the cache is operational, and `Unhealthy` when the cache cannot
be opened. Use this endpoint for container orchestration liveness and readiness
probes.

## Common patterns

### Disable optimization for specific paths

The middleware skips any request whose path starts with a prefix in
`ExcludePaths`. The defaults already cover `/api/`, `/signalr/`, `/_blazor/`,
and `/_framework/`. Add your own:

```json
{
  "PageSpeed": {
    "ExcludePaths": ["/api/", "/signalr/", "/_blazor/", "/_framework/", "/webhooks/", "/healthz"]
  }
}
```

You can also exclude paths with standard ASP.NET Core middleware ordering —
register `UsePageSpeed()` after the routes that should bypass it, or use
`IApplicationBuilder.MapWhen` to conditionally skip the middleware.

### Bandwidth-only rewrites

Disable HTML-mutating toggles while leaving the worker's image and CSS
minification untouched:

```json
{
  "PageSpeed": {
    "Html": {
      "EnableCriticalCss": false,
      "EnableLcpPreload": false,
      "EnableAsyncCss": false,
      "EnableSpeculationRules": false,
      "EnableScriptDeferral": false
    }
  }
}
```

This is the closest equivalent to 1.1's `OptimizeForBandwidth` — safe for sites
with strict CSP policies that disallow inlined CSS.

## See also

- [Install ASP.NET Core middleware](/docs/aspnet-getting-started/) — installation and first setup
- [Production deployment](/docs/production-deployment/) — hardening, persistent cache volumes, and worker placement
- [Choose a cache mode](/docs/cache-modes/) — `Safe` vs `Aggressive` semantics
- [Troubleshoot common issues](/docs/troubleshooting/) — worker startup and cache
- [Software license](/license/) — Apache License 2.0
