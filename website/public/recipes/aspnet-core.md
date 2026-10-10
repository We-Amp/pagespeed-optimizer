# ASP.NET Core: install and verify the mod_pagespeed 2.1 middleware

Source: https://modpagespeed.com/docs/aspnet-getting-started/ and
https://modpagespeed.com/docs/aspnet-configuration/. Header: `X-PageSpeed`.
Scope: the in-process `WeAmp.PageSpeed.AspNetCore` NuGet package. The
Linux-only `WeAmp.PageSpeed.Sidecar` package is a different integration (header
`X-Page-Speed`), described in the source document.

## 1. Prerequisites

.NET 8 or .NET 10 SDK; linux-x64, linux-arm64, osx-arm64 or win-x64; an
ASP.NET Core application that serves HTML.

```bash
dotnet --list-sdks              # 8.x or 10.x
dotnet --info | grep -i 'RID'   # linux-x64, linux-arm64, osx-arm64 or win-x64
```

## 2. Install

In the project directory:

```bash
dotnet add package WeAmp.PageSpeed.AspNetCore
```

## 3. Minimal configuration

`Program.cs`, with `UsePageSpeed()` before any middleware that writes the
response body:

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddPageSpeed();   // binds the "PageSpeed" configuration section

var app = builder.Build();
app.UsePageSpeed();
// the rest of the pipeline
app.Run();
```

`appsettings.json`. The default cache path, `/var/cache/pagespeed/volume.dat`,
needs elevated permissions; for a local run point it at a writable path (a
relative path is created under the app's working directory):

```json
{
  "PageSpeed": {
    "Enabled": true,
    "Cache": { "VolumePath": "pagespeed-cache/volume.dat" }
  }
}
```

The worker process starts automatically; no `Worker` section is needed.
Production cache paths and every option:
https://modpagespeed.com/docs/aspnet-configuration/

## 4. Verify

```bash
dotnet run   # note the listening URL, e.g. http://localhost:5123
curl -s -o /dev/null -D - 'http://localhost:<port>/?mps-verify=agent' | grep -i '^x-pagespeed:'
```

Pass: `X-PageSpeed: MISS` on the first request, `HIT` once the worker has
written the variant. Use a content route such as `/`, not `/console/*`, which
carries no header. No header: https://modpagespeed.com/docs/troubleshooting/

## 5. Rollback

```bash
dotnet remove package WeAmp.PageSpeed.AspNetCore
```

Remove `AddPageSpeed()`, `UsePageSpeed()` and the `using` from `Program.cs`,
the `PageSpeed` section from `appsettings.json`, and the cache volume file.
Pinning a previous version: https://modpagespeed.com/docs/uninstall/#aspnet-core
