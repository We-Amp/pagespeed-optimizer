// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// PageSpeed:Enabled false must not launch factory_worker. AutoStart false
/// still must not launch a child when Enabled stays true, so an external
/// worker (and the tests that opt out of a subprocess) keep working.
/// </summary>
public class WorkerDisabledTests
{
    [Fact]
    public async Task EnabledFalse_DoesNotLaunchWorker_EvenWhenAutoStartIsTrue()
    {
        var sentinel = Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.txt");
        var script = WriteLauncher(sentinel);
        var log = new ListLogger();
        var endpoint = new InternalWorkerEndpoint();
        var options = new PageSpeedOptions { Enabled = false };
        var host = new WorkerProcessHost(
            Options.Create(options), endpoint, log)
        {
            WorkerPathOverride = script,
        };

        try
        {
            await host.StartAsync(CancellationToken.None);

            var socketPath = await endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Null(socketPath);
            Assert.Contains(log.Entries, e =>
                e.Level == LogLevel.Information
                && e.Message.Contains("PageSpeed is disabled"));
            Assert.DoesNotContain(log.Entries, e => e.Message.Contains("Starting factory_worker"));
            await Task.Delay(300);
            Assert.False(File.Exists(sentinel));
        }
        finally
        {
            host.Dispose();
            File.Delete(script);
            if (File.Exists(sentinel))
                File.Delete(sentinel);
        }
    }

    [Fact]
    public async Task AutoStartFalse_DoesNotLaunchWorker_WhenEnabled()
    {
        var sentinel = Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.txt");
        var script = WriteLauncher(sentinel);
        var log = new ListLogger();
        var endpoint = new InternalWorkerEndpoint();
        var options = new PageSpeedOptions { Enabled = true };
        options.Worker.AutoStart = false;
        var host = new WorkerProcessHost(
            Options.Create(options), endpoint, log)
        {
            WorkerPathOverride = script,
        };

        try
        {
            await host.StartAsync(CancellationToken.None);

            var socketPath = await endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Null(socketPath);
            Assert.Contains(log.Entries, e =>
                e.Level == LogLevel.Information
                && e.Message.Contains("Worker auto-start is disabled"));
            Assert.DoesNotContain(log.Entries, e => e.Message.Contains("Starting factory_worker"));
            await Task.Delay(300);
            Assert.False(File.Exists(sentinel));
        }
        finally
        {
            host.Dispose();
            File.Delete(script);
            if (File.Exists(sentinel))
                File.Delete(sentinel);
        }
    }

    private static string WriteLauncher(string sentinel)
    {
        var path = Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.cmd");
        File.WriteAllText(path, $"echo launched> \"{sentinel}\"\r\n");
        return path;
    }

    private sealed class ListLogger : ILogger<WorkerProcessHost>
    {
        private readonly List<(LogLevel Level, string Message)> _entries = new();

        public IReadOnlyList<(LogLevel Level, string Message)> Entries
        {
            get { lock (_entries) return _entries.ToArray(); }
        }

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            lock (_entries) _entries.Add((logLevel, formatter(state, exception)));
        }
    }
}
