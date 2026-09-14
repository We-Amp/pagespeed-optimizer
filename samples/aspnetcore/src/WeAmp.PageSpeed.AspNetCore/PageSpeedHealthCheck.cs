// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Diagnostics.HealthChecks;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Health check that verifies the PageSpeed cache is operational.
/// </summary>
public sealed class PageSpeedHealthCheck : IHealthCheck
{
    private readonly IServiceProvider _sp;

    // Inject IServiceProvider for deferred resolution: if cache creation
    // fails at startup, the health check returns Unhealthy instead of
    // crashing the DI container.

    /// <summary>
    /// Creates a new PageSpeed health check.
    /// </summary>
    public PageSpeedHealthCheck(IServiceProvider sp) => _sp = sp;

    /// <inheritdoc/>
    public Task<HealthCheckResult> CheckHealthAsync(
        HealthCheckContext context,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var cache = _sp.GetRequiredService<IPageSpeedCache>();
            var stats = cache.GetStats();
            // Expose only operational status, not detailed metrics,
            // to avoid information disclosure on unauthenticated endpoints.
            var data = new Dictionary<string, object>
            {
                ["entries"] = stats.CurrentEntries,
            };
            try
            {
                data["version"] = PageSpeedVersion.Current.ToString();
            }
            catch
            {
                data["version"] = "unknown";
            }

            // Surface the worker-written agent_optimize
            // entitlement (read-only; 1 = entitled). Any error => false
            // (fail-safe-closed — never advertise an entitlement we can't read).
            try
            {
                data["agent_optimize_entitled"] =
                    cache.ReadSharedConfigAgentEntitled() == 1;
            }
            catch
            {
                data["agent_optimize_entitled"] = false;
            }

            return Task.FromResult(
                HealthCheckResult.Healthy(
                    "PageSpeed cache is operational", data));
        }
        catch (Exception ex)
        {
            return Task.FromResult(
                HealthCheckResult.Unhealthy("PageSpeed cache error", ex));
        }
    }
}
