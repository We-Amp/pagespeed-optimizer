---
title: 'ASP.NET Core Sidecar (NuGet)'
description: 'Add mod_pagespeed 1.15 to an ASP.NET Core app with two lines of middleware. WeAmp.PageSpeed.Sidecar runs a bundled nginx + ngx_pagespeed optimizer on loopback behind your Kestrel app. Linux-only (linux-x64, linux-arm64), with AddPageSpeed()/UsePageSpeed() wiring and a full options reference.'
order: 8
group: 'ASP.NET Core'
lastUpdated: 2026-09-06
---

`WeAmp.PageSpeed.Sidecar` adds **mod_pagespeed 1.15** to your ASP.NET Core app with two lines of
middleware. Your Kestrel app stays the public front door; a bundled, matched (nginx + ngx_pagespeed)
optimizer runs on loopback behind it, recompressing images (jpeg/png/webp), minifying CSS and JS,
inlining and extracting critical CSS, and rewriting HTML for Core Web Vitals on the way out.
Requests that can't be optimized — and any request while the optimizer isn't running — pass straight
through your app un-optimized.

```csharp
builder.Services.AddPageSpeed(builder.Configuration);
// ...
app.UsePageSpeed();
```

That's the whole setup. No domain list, no separate front-proxy to configure. This is the default
**Inverse** topology; a classic front-proxy mode (bundled nginx as the public front door) is also
available — see [Sidecar modes](#configuration-options).

:::note
This is the **1.15 (1.x engine) ASP.NET Core option** and is **Linux-only** (`linux-x64`,
`linux-arm64`). If you want cross-platform, in-process optimization (no bundled nginx, with the
newest image pipeline — **SVG auto-vectorization, Jpegli, and ML-predicted quality**), use the
ModPageSpeed 2.0 middleware
[`WeAmp.PageSpeed.AspNetCore`](https://www.nuget.org/packages/WeAmp.PageSpeed.AspNetCore)
instead — see its [ASP.NET Core setup guide](/docs/aspnet-getting-started/). On Windows, use the IIS module. The bundled nginx module
transcodes AVIF through the same opt-in filters as the native 1.15 modules — see
[image filters](/1.1/docs/image-filters/) for how to enable them.
:::

## How it works

In the default **Inverse** mode, your ASP.NET Core app is the public front door. When the
`UsePageSpeed()` middleware runs, it streams an optimizable response over loopback to the bundled
nginx, which applies the ngx_pagespeed optimizations and proxies back to a second, private
raw-origin Kestrel endpoint where the middleware bypasses itself. The loop break is automatic and
internal. Non-optimizable requests, and any request received while the sidecar isn't running, pass
straight through un-optimized — optimization is always additive.

The package binds the public port (`Sidecar.ListenPort`, `8080` by default), generates its own
`nginx.conf`, manages the nginx process lifecycle (start, health, auto-restart, graceful stop), and
registers an ASP.NET Core health check. It serves plain HTTP; for HTTPS, front it with a TLS
terminator or load balancer (or set `Sidecar.OwnPublicPort = false` and configure your own endpoint
via `Kestrel:Endpoints`).

## Install

```bash
dotnet add package WeAmp.PageSpeed.Sidecar
```

This automatically pulls the matched native engine package
(`WeAmp.PageSpeed.Sidecar.NativeAssets.Linux`, which bundles the nginx + ngx_pagespeed binaries
for `linux-x64` and `linux-arm64`). Publish with a Linux runtime identifier so the bundled nginx
ships next to your app — a framework-dependent publish with no `-r` omits the nginx binary and the
sidecar fails to start:

```bash
dotnet publish -c Release -r linux-x64    # or: -r linux-arm64
```

## Wire it up

```csharp
var builder = WebApplication.CreateBuilder(args);

// Reads the "PageSpeed" configuration section. Mode defaults to Inverse; the package
// binds the public port (Sidecar.ListenPort, 8080) and runs the optimizer behind it.
builder.Services.AddPageSpeed(builder.Configuration);

var app = builder.Build();
app.UsePageSpeed();
app.MapHealthChecks("/health");
app.Run();
```

Or configure in code:

```csharp
builder.Services.AddPageSpeed(options =>
{
    options.RewriteLevel = "CoreFilters";
});
```

You do **not** list domains to make optimization work: by default the optimizer rewrites
same-origin resources for any host your app serves, with zero configuration (`localhost` included,
for local dev). `Domains.AuthorizedDomains` is only for the advanced cases below.

`AddPageSpeed` registers a health check named `pagespeed` (tagged `pagespeed`, `sidecar`), so the
`app.MapHealthChecks("/health")` endpoint above reflects bundled-nginx liveness. For a
PageSpeed-only endpoint, call `app.MapPageSpeedHealthCheck("/health/pagespeed")` (it filters on the
`pagespeed` tag).

## Configuration options

Bind a `PageSpeed` section in `appsettings.json`. Every key is optional and falls back to its
default (the defaults below are the Inverse defaults):

```json
{
  "PageSpeed": {
    "Enabled": true,
    "RewriteLevel": "CoreFilters",
    "Sidecar": {
      "OriginPort": 0
    },
    "Cache": {
      "FileCachePath": "/var/cache/pagespeed",
      "FileCacheSizeKb": 10240000
    },
    "AdminAuth": { "Enabled": true }
  }
}
```

| Key | Default | Notes |
|---|---|---|
| `Enabled` | `true` | Master switch for optimization. |
| `RewriteLevel` | `CoreFilters` | One of `PassThrough`, `CoreFilters`, `OptimizeForBandwidth`, `MobilizeFilters`, `TestingCoreFilters`, `AllFilters`. |
| `Sidecar.Mode` | `Inverse` | `Inverse` (default — your middleware is the public front door, nginx optimizes on loopback), `Process` (classic front-proxy — bundled nginx is the public front door, Kestrel the private origin), or `External` (operator-managed nginx). `Docker` is reserved and fails config validation. |
| `Sidecar.ListenPort` | `8080` | The public port the package binds in Inverse (the default) and the public nginx port in Process. |
| `Sidecar.OwnPublicPort` | `true` | Inverse only. When `true` (default), the package binds the public endpoint on `0.0.0.0:Sidecar.ListenPort`. Set `false` to own the bind yourself via `Kestrel:Endpoints` (`--urls`/`UseUrls`/`ASPNETCORE_URLS` are overridden once the loopback endpoint is added and won't bind publicly). |
| `Sidecar.OriginPort` | `0` | Private raw-origin loopback port. `0` = auto. In Inverse it is always loopback-TCP (never a Unix socket); in Process/External, `0` prefers a private Unix-domain socket. You normally leave this at `0`. |
| `Sidecar.NginxLoopbackPort` | `0` | Inverse only. The loopback-only port nginx listens on. `0` = auto. |
| `Sidecar.RestrictToAuthorizedHosts` | `false` | Inverse only, defense-in-depth. When `true`, optimizes only hosts in `Domains.AuthorizedDomains` (loopback always allowed); every other host is served un-optimized. Default `false` optimizes all hosts (forward-all). |
| `Sidecar.AllowPublicAdmin` | `false` | Inverse only. When `true`, exposes the `/pagespeed_*` admin endpoints from the public front door; requires `AdminAuth.Enabled`. Default `false` returns 404 for them from the public endpoint. |
| `Sidecar.UseLaunchShim` | `true` | On by default: launches the bundled nginx through the native PR_SET_PDEATHSIG shim so a hard SIGKILL/OOM/container-hard-stop can't orphan nginx (and leave it holding its loopback port). Degrades gracefully on packages built without the shim binary; set `false` to force a direct launch. |
| `Domains.AuthorizedDomains` | `["localhost", "127.0.0.1"]` | Hosts authorized for **cross-origin** rewriting (e.g. a CDN). A request's own same-origin resources are always authorized without listing the host here, so this is **not** required for normal optimization; it is the strict allowlist only when `Sidecar.RestrictToAuthorizedHosts = true`. |
| `Cache.FileCachePath` | system temp dir | On-disk cache location for optimized resources. |
| `Cache.FileCacheSizeKb` | `10240000` (10 GB) | File-cache size cap, in KB. |
| `AdminAuth.Enabled` | `true` | Gates the `/pagespeed_*` admin endpoints behind a bearer token. Required to be `true` if `Sidecar.AllowPublicAdmin = true`. |

Source `AdminAuth.Token` from a secret store or environment variable — do not put it in
`appsettings.json`.

## Container & runtime requirements

The bundled nginx + module are glibc ELF binaries built for a glibc ≥ 2.31 floor, so they run on
Debian 11/12, Ubuntu 20.04+, RHEL/Alma 9, and Amazon Linux 2023. `libstdc++` is statically linked
and the build has no OpenSSL dependency, so there is no libstdc++ or libssl/libcrypto host
requirement. The binaries link a small set of host shared libraries:

| Host library | Provided by | Notes |
|---|---|---|
| `libpcre.so.3` | `libpcre3` | nginx PCRE — missing from `-chiseled`/`-distroless` images |
| `libz.so.1` | `zlib1g` | present on standard .NET images |
| `libcrypt.so.1` | `libcrypt1` | present on standard .NET images |

- **Recommended base image:** `mcr.microsoft.com/dotnet/aspnet:8.0` (Debian); add the one missing
  lib with `apt-get install -y libpcre3`.
- **Not supported:** `-chiseled` / `-distroless` .NET images (no `libpcre3`) and Alpine/musl images
  (the binaries are glibc-linked). Use a Debian/Ubuntu glibc base. nginx still runs in Inverse mode
  — on loopback — so the same base-image requirements apply.

Launch your app as `dotnet YourApp.dll` (the default `ENTRYPOINT` for `dotnet publish` container
tooling and `mcr.microsoft.com/dotnet/aspnet` images) so `SIGTERM` from `docker stop` / Kubernetes /
systemd reaches the host's graceful shutdown and stops the bundled nginx cleanly. Launching the
apphost directly (`./YourApp`) does not reliably propagate `SIGTERM`, so the bundled nginx can
survive shutdown and keep holding its loopback port.

## Verify it's working

```bash
curl -sI http://localhost:8080/ | grep -i x-page-speed
```

A present `X-Page-Speed` header confirms the optimizer is in the path. For per-filter rewrite counts, check `/pagespeed_statistics` (loopback-only by
default; the middleware returns 404 for the `/pagespeed_*` admin paths from the public front door
unless `Sidecar.AllowPublicAdmin` is enabled).

## License

mod_pagespeed 1.15 is licensed under Apache-2.0 and free to run in development and in production.
Support subscriptions are available separately — see [License](/docs/license/).

## When to use the sidecar vs. ModPageSpeed 2.0

| | `WeAmp.PageSpeed.Sidecar` (1.15) | `WeAmp.PageSpeed.AspNetCore` (2.0) |
|---|---|---|
| Model | Middleware front door; bundled nginx optimizes on loopback | In-process middleware (P/Invoke) |
| Platforms | Linux only (`linux-x64`, `linux-arm64`) | linux-x64/arm64, osx-arm64, win-x64 |
| Image formats | JPEG/PNG recompression, WebP, opt-in AVIF | WebP and AVIF, **plus** SVG auto-vectorization and Jpegli |
| Best fit | The proven 1.x nginx engine on Linux | Cross-platform, in-process, newest pipeline |

Both are licensed under Apache-2.0 and free to run in development and in production. For the 2.0
middleware, start with its [ASP.NET Core getting-started guide](/docs/aspnet-getting-started/).
