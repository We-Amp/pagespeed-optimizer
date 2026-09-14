# WeAmp.PageSpeed for ASP.NET Core

Drop-in ASP.NET Core middleware that improves Core Web Vitals without touching
your app code. It adds critical CSS inlining, LCP preload injection, lazy
loading, and on-demand WebP/AVIF image transcoding to every HTML response. The
C++23 PageSpeed engine runs in-process via P/Invoke and serves cache hits
zero-copy.

Single-package install. Hot-reloadable config. Optimization runs out of the
box.

## Quick Start

**1. Install the package**

```shell
dotnet add package WeAmp.PageSpeed.AspNetCore
```

The matching native binaries for your runtime (`linux-x64`, `linux-arm64`,
`osx-arm64`, or `win-x64`) come in transitively: no separate NativeAssets
package reference required.

**2. Register the middleware in `Program.cs`**

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddPageSpeed();

var app = builder.Build();

app.UsePageSpeed();    // before anything that writes the response body
app.UseStaticFiles();

app.Run();
```

No extra wiring required: with no `Worker` section, the worker process starts
automatically and coordinates over an auto-resolved per-process socket. Add a
`Worker` section only to change that — see [Configuration](#configuration).
Optimization is on by default.

**3. How do I know it's working?**

Three checks, from quickest to most thorough. These examples assume your app
listens on `:5050` (set `ASPNETCORE_URLS=http://localhost:5050` or adjust the
URLs to your port) and serves at least one HTML page and one image.

Hit a content route and look for the `X-PageSpeed` header:

```shell
curl -i http://localhost:5050/
```

```
HTTP/1.1 200 OK
X-PageSpeed: HIT
```

`HIT` means the response was served from the optimized cache; `MISS` means the
worker is building the variant and you'll see `HIT` on the next request. (The
`/console/*` routes are short-circuited before the middleware, so they
intentionally do not carry `X-PageSpeed` — only content routes like `/` and
your assets do.)

Open `http://localhost:5050/console/`. Once a few requests have run,
the Dashboard and Metrics show non-zero counts. If everything reads zero, see
[How do I know it's working?](https://modpagespeed.com/docs/aspnet-getting-started/#verify-the-install)
in the docs.

Confirm image transcoding via content negotiation. We don't rewrite URLs in
2.0: the same `/hero.jpg` URL serves WebP to WebP-capable clients and AVIF to
AVIF-capable ones, selected by the request `Accept` header:

```shell
curl -s -o /dev/null -D - http://localhost:5050/hero.jpg -H 'Accept: image/jpeg'
# Content-Length: 98230   Content-Type: image/jpeg   Vary: Accept, Save-Data, User-Agent

curl -s -o /dev/null -D - http://localhost:5050/hero.jpg -H 'Accept: image/webp'
# Content-Length: 2422    Content-Type: image/webp   Vary: Accept, Save-Data, User-Agent

curl -s -o /dev/null -D - http://localhost:5050/hero.jpg -H 'Accept: image/avif'
# Content-Length: 415     Content-Type: image/avif   Vary: Accept, Save-Data, User-Agent
```

Same URL, materially smaller bytes, and a `Vary: Accept, Save-Data, User-Agent`
header so caches keep the variants apart. (Byte counts are from one sample
image; yours will differ.)

## What It Does

- **HTML optimization:** critical CSS inlining, lazy loading, LCP preload
  injection, third-party preconnect hints
- **Image transcoding:** on-demand conversion to WebP and AVIF, viewport-aware
  resizing, Save-Data support
- **CSS/JS minification:** whitespace removal, comment stripping
- **Zero-copy caching:** cache hits served from the memory-mapped Cyclone cache
  with no copy, up to 36 optimized variants per resource (format × viewport ×
  density × Save-Data)

The middleware buffers HTML responses, passes them through the native
`libpagespeed` library, and notifies the worker process to generate optimized
asset variants asynchronously.

## Platform Support

| RID         | Status    | Notes                                                    |
| ----------- | --------- | -------------------------------------------------------- |
| linux-x64   | Supported | glibc 2.34+ (RHEL 9 / Ubuntu 22.04 / Debian 12 or newer) |
| linux-arm64 | Supported | glibc 2.34+                                              |
| osx-arm64   | Supported | macOS 13+ (Apple Silicon)                                |
| win-x64     | Supported | Windows 10/11, Server 2019+                              |

Native binaries (`libpagespeed`, `factory_worker`) are bundled with the
matching `WeAmp.PageSpeed.NativeAssets.*` package, pulled in transitively by
`WeAmp.PageSpeed.AspNetCore`. The worker process starts automatically on
application boot. The Linux binaries statically link the C++ runtime
(libc++/libc++abi/libunwind), so no additional shared libraries need to be
present on the host beyond the system glibc.

**On Linux and want a reverse proxy instead of in-process middleware?**
[`WeAmp.PageSpeed.Sidecar`](https://www.nuget.org/packages/WeAmp.PageSpeed.Sidecar)
runs mod_pagespeed 1.15 as a bundled nginx + ngx_pagespeed reverse proxy in front of
Kestrel (Linux-only). Use it when you want the classic nginx module in a sidecar; use
this package for cross-platform in-process optimization with WebP/AVIF.

Each NativeAssets package also ships a `BUILD_INFO.json` file at
`runtimes/<rid>/native/BUILD_INFO.json` with `git_sha`, `git_sha_short`,
`build_timestamp_utc`, and `rid`. It is intended for support correlation and
for verifying package provenance without running the worker.

## Configuration

Add a `PageSpeed` section to `appsettings.json`. Only `Cache` is shown below;
every key in the table is optional and falls back to its default.

```json
{
  "PageSpeed": {
    "Cache": {
      "VolumePath": "/var/cache/pagespeed/volume.dat",
      "VolumeSizeBytes": 1073741824
    }
  }
}
```

Set `Cache.VolumePath` to a location that is writable in your environment. The
default `/var/cache/pagespeed/` assumes a Linux host; on Windows, macOS, or
containers without that path, point it somewhere writable (for example
`./cache/volume.dat` or `%TEMP%`).

**Top-level keys:**

| Setting                  | Default                                               | Description                                                                                                          |
| ------------------------ | ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| `Enabled`                | `true`                                                | Enable/disable the middleware (supports hot-reload)                                                                  |
| `ExcludePaths`           | `["/api/", "/signalr/", "/_blazor/", "/_framework/"]` | URL prefixes the middleware leaves untouched (supports hot-reload)                                                   |
| `CacheMode`              | `Safe`                                                | `Safe`: `must-revalidate` on assets. `Aggressive`: `public` + `stale-if-error` on assets. HTML is always `no-cache`. |
| `MaxResponseBufferBytes` | `5242880` (5 MB)                                      | Responses larger than this pass through unmodified                                                                   |
| `CssMaxAgeSeconds`       | `300`                                                 | `max-age` on CSS/JS cache HIT responses                                                                              |
| `ImageMaxAgeSeconds`     | `1800`                                                | `max-age` on image cache HIT responses                                                                               |

**`Cache` section:**

| Setting                 | Default                           | Description                                              |
| ----------------------- | --------------------------------- | -------------------------------------------------------- |
| `Cache.VolumePath`      | `/var/cache/pagespeed/volume.dat` | Path to the Cyclone cache volume file (must be writable) |
| `Cache.VolumeSizeBytes` | `1073741824` (1 GB)               | Maximum cache volume size                                |

**`Worker` section:**

| Setting             | Default                   | Description                                                                                                                                                                                                                                                                         |
| ------------------- | ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Worker.AutoStart`  | `true`                    | Launch and manage the worker process on startup. Set `false` to run no worker.                                                                                                                                                                                                      |
| `Worker.SocketPath` | unset                     | Worker-coordination socket. **Leave it unset** (the default) for an auto-resolved per-process socket with coordination on. Set to a concrete path to share one socket with an out-of-process worker. Setting it to `null` or `""` disables coordination and logs a startup warning. |
| `Worker.ApiPort`    | `0` (auto, loopback only) | Override to expose the worker HTTP API on a fixed port                                                                                                                                                                                                                              |

**`Console` section:**

| Setting                | Default    | Description                                                         |
| ---------------------- | ---------- | ------------------------------------------------------------------- |
| `Console.MountPath`    | `/console` | URL prefix for the in-app console                                   |
| `Console.RequireHttps` | `false`    | Reject non-HTTPS requests to the console (set `true` in production) |

Options support hot-reload via `IOptionsMonitor<PageSpeedOptions>`.

## Worker IPC

The middleware ↔ worker channel uses Unix domain sockets on Linux and macOS,
and Windows Named Pipes on win-x64. Selection is automatic; no configuration
required. The console and its worker-API proxy are served on the app port; the
worker's HTTP API is bound to `127.0.0.1` on an ephemeral port by default and
is not exposed externally unless you set `Worker.ApiPort` explicitly.

## Security

The `/console/` admin console and its `/v1/*` worker-API proxy are served on
the same origin as your app. The worker requires
`X-Requested-With: XMLHttpRequest` on state-changing (POST/PATCH) requests,
which blocks cross-origin form posts. It does not protect against same-origin
scripts: a third-party script loaded into your app (via XSS or a supply-chain
dependency) can drive the console API — for example purge the cache or change
the worker configuration — because the in-process worker API is reachable
through the proxy without a credential.

Treat `/console/` as an admin surface:

- Set `Console.RequireHttps = true` in production.
- Don't expose `/console/` to untrusted networks. Gate it at your reverse
  proxy, or use `Console.MountPath` to move the path off a guessable location.
- Avoid loading untrusted third-party scripts into apps with this middleware
  enabled.

## License

Apache License 2.0, see the LICENSE file in the package.

## Links

- [modpagespeed.com/pricing/](https://modpagespeed.com/pricing/) — plans, pricing, and purchase
- [modpagespeed.com/features/#aspnet-core](https://modpagespeed.com/features/#aspnet-core) — ASP.NET Core overview
- [WeAmp.PageSpeed.Sidecar](https://www.nuget.org/packages/WeAmp.PageSpeed.Sidecar) — mod_pagespeed 1.15 as a bundled-nginx sidecar (Linux)
- [modpagespeed.com](https://www.modpagespeed.com) — product documentation and filter reference
- [we-amp.com](https://www.we-amp.com) — We-Amp B.V.
