---
title: 'Getting started'
description: 'Pick an integration and get mod_pagespeed 2.1 serving optimized pages: Apache or nginx module, Docker, IIS or ASP.NET Core, then run the same check.'
order: 1
group: 'Start here'
lastUpdated: 2026-10-08
faq:
  - q: 'Which mod_pagespeed 2.1 integration should I pick?'
    a: 'The native module when you run Apache or nginx on Debian, Ubuntu or Enterprise Linux; the Docker reverse proxy when you are container-native or your origin is something else; the IIS module on Windows Server; the NuGet middleware inside an ASP.NET Core app. All four run the same optimization pipeline.'
  - q: 'How do I verify mod_pagespeed is working?'
    a: 'Request a page twice with `curl -I`. The native module adds `X-Mod-Pagespeed` (Apache) or `X-Page-Speed` (nginx and IIS) to the responses it handles. The Docker reverse proxy and the ASP.NET Core middleware answer `X-PageSpeed: MISS` first and `X-PageSpeed: HIT` once the worker has written the optimized variant.'
  - q: 'Does the native nginx module need the optimizer worker?'
    a: 'No. It optimizes on its own. To hand in-place optimization to the worker, install the `pagespeed-optimizer` package of the same release and set `pagespeed DaemonSocketPath` and `pagespeed DaemonVolumePath` in the server block. The Apache packages install that wiring for you.'
  - q: 'What are the prerequisites?'
    a: 'For the native module: Debian 12 or 13, Ubuntu 22.04 or 24.04, or RHEL, AlmaLinux or Rocky 9 or 10, with Apache 2.4 or newer or the stock nginx of the distribution. For Docker: Docker 24 or newer; nginx ships inside the image. For IIS: Windows Server 2019 or later. For ASP.NET Core: .NET 8 or .NET 10.'
  - q: 'What is safe cache mode and why is it the default?'
    a: 'Safe mode caches optimized resources for short periods (5 minutes for CSS/JS, 30 minutes for images) with mandatory revalidation, so misconfigurations self-correct quickly. It is the recommended mode while validating a new setup before switching to aggressive.'
---

mod_pagespeed 2.1 is one product in two parts: a module that runs inside your
web server and an optimizer worker that does the heavy work beside it. Pick the
integration that matches how you serve pages, run the few lines under it, then
check the result with the same two requests. Every path below is free to
install and run; the software is licensed under the Apache License 2.0.

## Pick an integration

| You run                                                        | Install this                                                                               | Guide                                                                                                       |
| -------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------- |
| Apache or nginx on Debian, Ubuntu or Enterprise Linux          | the native module and the `pagespeed-optimizer` worker from the signed package repository | [Install the module on Apache and nginx](/docs/installation-module/)                                        |
| cPanel / WHM with EasyApache 4                                 | the signed EA4 RPM                                                                         | [cPanel / EasyApache 4](/docs/cpanel/)                                                                      |
| Containers, Kubernetes, or an origin that is not Apache or nginx | the Docker reverse proxy, or the Helm chart, in front of your origin                     | [Install with Docker](/docs/installation-docker/) · [Deploy with Helm](/docs/helm-deployment/)              |
| IIS on Windows Server                                          | the native IIS module (MSI)                                                                | [Install on IIS](/docs/install-iis/)                                                                        |
| An ASP.NET Core application                                    | the `WeAmp.PageSpeed.AspNetCore` NuGet middleware                                          | [Install ASP.NET Core middleware](/docs/aspnet-getting-started/)                                            |

Already running a predecessor? The [upgrade and migration pages](/docs/upgrade/)
cover mod_pagespeed 1.15, ModPageSpeed 2.0, ngx_pagespeed, IISpeed and the
archived open-source module.

## Prerequisites

- **Native module:** Debian 12 or 13, Ubuntu 22.04 or 24.04, or RHEL, AlmaLinux
  or Rocky 9 or 10, with Apache 2.4 or newer or the distribution's stock nginx.
- **Docker reverse proxy:** Docker 24 or newer and Docker Compose v2; nginx
  ships inside the image, so you do not install it yourself.
- **IIS:** Windows Server 2019 or later, 64-bit, with the Visual C++
  Redistributable 2022.
- **ASP.NET Core:** .NET 8 or .NET 10.

:::note[One worker, two names]
The optimizer worker is one program under two names. The container images and
the NuGet package ship the binary as `factory_worker`. The deb and rpm packages
install it as `/usr/bin/pagespeed-optimizer`, with the
`pagespeed-optimizer.service` unit, host overrides in
`/etc/default/pagespeed-optimizer` and its cache under
`/var/cache/pagespeed-optimizer/v2`. A page that says `factory_worker` and a log
line that says `pagespeed-optimizer` describe the same process.
:::

## Quickstart: Apache or nginx module

```bash
# 1. Add the signed repository; the script detects apt or dnf
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
# 2. Install the module (apt-get on Debian/Ubuntu, dnf on Enterprise Linux)
sudo apt-get install mod-pagespeed             # Apache: also installs pagespeed-optimizer
sudo apt-get install nginx-module-pagespeed    # nginx
# 3. Restart the web server
sudo systemctl restart apache2                 # httpd on Enterprise Linux
sudo systemctl restart nginx
```

Apache optimizes immediately: the package enables the module, its default
`pagespeed.conf`, and the configuration that points it at the worker. On nginx,
make sure `load_module modules/ngx_pagespeed_module.so;` is at the top of
`nginx.conf`, then turn the module on in a `server` block:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
```

Then [verify](#verify-it-works). The
[module install guide](/docs/installation-module/) has the distribution matrix,
the nginx compatibility table and the two directives that hand nginx's in-place
optimization to the worker.

## Quickstart: Docker reverse proxy

```bash
docker run --rm -p 80:80 \
  -e BACKEND_HOST=host.docker.internal -e BACKEND_PORT=8081 \
  -e ACCEPT_EULA=Y \
  ghcr.io/we-amp/pagespeed-combined:latest
```

Point `BACKEND_HOST` and `BACKEND_PORT` at your origin; `ACCEPT_EULA=Y`
acknowledges the [Terms of Service](/terms/). The combined image runs nginx with
the module and the worker in one container, for evaluation and small single-host
deployments. Then [verify](#verify-it-works) on port 80. For production, run the
worker and nginx as separate services from the
[Docker install guide](/docs/installation-docker/), or on Kubernetes with the
[Helm chart](/docs/helm-deployment/).

## Quickstart: IIS

Download the signed MSI from the [download page](/download/), run it on the
Windows Server host, then `iisreset`. The installer registers the module as a
native HTTP module and creates the default cache directory. Then
[verify](#verify-it-works). Running IISpeed on that host? Uninstall it first;
the two register the same handler. The [IIS install guide](/docs/install-iis/)
has the requirements, IIS Express and the optional optimizer service. The IIS
package ships from the 1.15 packaging channel.

## Quickstart: ASP.NET Core

Add the `WeAmp.PageSpeed.AspNetCore` package to your project, then register the
middleware in `Program.cs`:

```csharp
builder.Services.AddPageSpeed();
app.UsePageSpeed();
```

Set `PageSpeed:Enabled` to `true` in `appsettings.json`, run the app and
[verify](#verify-it-works) on your app's own port. The
[ASP.NET Core install guide](/docs/aspnet-getting-started/) has the exact
`dotnet add package` command, the `appsettings.json` section and the console at
`/console/`.

## Verify it works

<a id="quick-verification"></a>Request any HTML page twice:

```bash
curl -I http://localhost/
curl -I http://localhost/
```

- **Apache module:** `X-Mod-Pagespeed: <version>` on both responses. nginx and
  IIS module: `X-Page-Speed: <version>`. The header's presence means the module
  is loaded and active for that site.
- **Docker reverse proxy and ASP.NET Core middleware:** `X-PageSpeed: MISS` on
  the first response, `X-PageSpeed: HIT` on the second. Optimization is
  asynchronous: the first request serves the original bytes and notifies the
  worker; the second serves what the worker wrote.

No header, or `MISS` on every request? [Is it working?](/docs/is-it-working/)
lists every marker and the first checks to run;
[Troubleshooting](/docs/troubleshooting/) has the fixes.

## How the parts fit together

<a id="how-the-docker--nginx-reverse-proxy-integration-works"></a>

1. The **module** runs inside the web server (Apache, nginx, IIS) or, in the
   Docker reverse proxy, inside the bundled nginx. It classifies each request,
   serves an optimized variant when the cache has one, and otherwise passes
   the original through.
2. The **optimizer worker** runs beside it as its own process. It reads
   originals from the shared cache, builds optimized variants (image
   transcoding, CSS and JavaScript minification, critical CSS) and writes them
   back.
3. The **Cyclone cache** is the shared, memory-mapped file both of them open.

The native nginx module optimizes on its own until you set
`pagespeed DaemonSocketPath` and `pagespeed DaemonVolumePath`; the Apache
packages install that wiring; the ASP.NET Core middleware starts the worker
itself as a child process. [How it works](/how-it-works/) covers the mechanics.

### Request flow (nginx integrations) {#request-flow-nginx-integrations}

1. nginx classifies the client's capabilities (image format support, viewport,
   transfer encoding, Save-Data) into a 32-bit capability mask.
2. The cache is checked for an optimized variant matching that mask.
3. **On a hit**, the variant is served from the memory-mapped cache file with
   `X-PageSpeed: HIT`: no copies, no origin round-trip.
4. **On a miss**, the request is proxied to your origin, the response is
   stored and served with `X-PageSpeed: MISS`, and the worker is notified. It
   optimizes the content and writes the variants back, so later requests for
   the same mask are hits.

## Safe cache mode

By default, mod_pagespeed runs in **safe cache mode**. Optimized resources are
cached for short periods (5 minutes for CSS/JS, 30 minutes for images) with
mandatory revalidation, so misconfigurations self-correct quickly. This is the
recommended mode while you validate your setup. See
[Cache modes](/docs/cache-modes/) for details and options.

## Next steps

- [Is it working?](/docs/is-it-working/): the header table and the first checks
- [Configuration reference](/docs/configuration/): all directives, worker
  flags and tuning options
- [Run in production](/docs/deployment/): permissions, logging and cache sizing
  per deployment shape
- [Monitoring](/docs/monitoring/): health, metrics and what to alert on
- [Troubleshooting](/docs/troubleshooting/): common issues and diagnostics
