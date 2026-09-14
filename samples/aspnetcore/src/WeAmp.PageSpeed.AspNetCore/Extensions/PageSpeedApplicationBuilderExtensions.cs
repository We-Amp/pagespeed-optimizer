// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Builder;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Extension methods for adding the PageSpeed middleware to the pipeline.
/// </summary>
public static class PageSpeedApplicationBuilderExtensions
{
    /// <summary>
    /// Adds PageSpeed HTML optimization middleware.
    /// Recommended ordering:
    ///   app.UseExceptionHandler();
    ///   app.UseHttpsRedirection();
    ///   app.UsePageSpeed();           // here
    ///   app.UseResponseCompression(); // after PageSpeed
    ///   app.UseStaticFiles();
    ///   app.UseRouting();
    /// </summary>
    public static IApplicationBuilder UsePageSpeed(
        this IApplicationBuilder app)
    {
        // ConsoleMiddleware reverse-proxies /v1/ws/* WebSocket upgrades
        // from the SPA to the loopback worker, which requires the
        // WebSocketMiddleware to be present in the pipeline first.
        // UseWebSockets is documented as idempotent; calling it here
        // means customer apps don't have to.
        app.UseWebSockets();
        // ConsoleMiddleware runs first so it can short-circuit /console/*
        // and the proxied /v1/* worker-API paths (health, ws, ...) before
        // any HTML optimization touches them.
        app.UseMiddleware<ConsoleMiddleware>();
        return app.UseMiddleware<PageSpeedMiddleware>();
    }
}
