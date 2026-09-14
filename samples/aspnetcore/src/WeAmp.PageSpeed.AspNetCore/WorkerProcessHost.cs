// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// BackgroundService that auto-starts and manages the factory_worker
/// process as a child process. Resolves the worker binary from the
/// NuGet native assets layout (runtimes/{rid}/native/).
/// </summary>
public sealed class WorkerProcessHost : BackgroundService
{
    private readonly IOptions<PageSpeedOptions> _options;
    private readonly InternalWorkerEndpoint _endpoint;
    private readonly ILogger<WorkerProcessHost> _logger;
    private Process? _process;

    /// <summary>
    /// Creates a new worker process host.
    /// </summary>
    public WorkerProcessHost(
        IOptions<PageSpeedOptions> options,
        InternalWorkerEndpoint endpoint,
        ILogger<WorkerProcessHost> logger)
    {
        _options = options;
        _endpoint = endpoint;
        _logger = logger;
    }

    /// <inheritdoc/>
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var workerOpts = _options.Value.Worker;
        if (!workerOpts.AutoStart)
        {
            _logger.LogInformation("Worker auto-start is disabled");
            // No worker means no IPC endpoint. Publish null so the
            // notification service stops waiting and disables itself instead
            // of blocking forever on SocketPathReady.
            _endpoint.SetSocketPath(null);
            return;
        }

        // Resolve the socket path once, up front, and reuse the captured value
        // for the rest of this method. We then publish it through the shared
        // InternalWorkerEndpoint so the concurrently-starting
        // WorkerNotificationService connects to the SAME endpoint we launch the
        // worker with (it can't re-derive it: IOptions vs IOptionsMonitor hand
        // out distinct WorkerOptions instances, so an independently-resolved
        // auto-path would differ).
        var socketPath = workerOpts.SocketPath;
        if (socketPath == null)
        {
            // Explicit "SocketPath": null (or = null in code) → coordination
            // is disabled by the operator. The worker would launch with no IPC
            // endpoint and the middleware would drop every notification, so
            // make the intent loud rather than silently shipping a worker that
            // can never report stats.
            _logger.LogWarning(
                "Worker.SocketPath is explicitly null — worker coordination is "
                + "disabled. notifications.received and variants.written will "
                + "stay 0. Leave SocketPath unset (the default) to auto-resolve "
                + "an endpoint, or set AutoStart=false if you don't want a worker.");
        }
        else
        {
            _logger.LogDebug("Worker IPC endpoint: {SocketPath}", socketPath);
        }
        // Publish the pinned path so WorkerNotificationService connects to the
        // exact endpoint we launch the worker with, rather than re-deriving an
        // auto-path off its own (distinct) WorkerOptions instance.
        _endpoint.SetSocketPath(socketPath);

        var workerPath = ResolveWorkerPath();
        if (workerPath == null)
        {
            _logger.LogWarning(
                "factory_worker binary not found; worker process will not start");
            return;
        }

        _logger.LogInformation("Starting factory_worker: {Path}", workerPath);

        var startInfo = new ProcessStartInfo
        {
            FileName = workerPath,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        var cacheOpts = _options.Value.Cache;
        if (!string.IsNullOrEmpty(cacheOpts.VolumePath))
        {
            // Provision the directory the worker's cache lives in BEFORE
            // spawning it. The daemon validates this directory at startup and
            // refuses to start when it is absent -- it never creates it, never
            // chowns, never falls back to another location.
            // That contract is written for the PACKAGED daemon, whose
            // directory is created by tmpfiles.d ahead of the service. This
            // host is the other case: an embedded worker spawned by the
            // application itself, under the application's own uid, at a path
            // the application chose. Nothing in a NuGet install creates it, so
            // the host that picks the path is the component that must make it.
            //
            // It used to work by accident: PageSpeedCache's constructor
            // creates the same parent directory, and whichever of the two ran
            // first decided whether the worker came up. That race is why the
            // worker started on one run and exited at startup on the next.
            EnsureCacheDirectory(cacheOpts.VolumePath, _logger);

            startInfo.ArgumentList.Add("--cache-path");
            startInfo.ArgumentList.Add(cacheOpts.VolumePath);
        }
        if (cacheOpts.VolumeSizeBytes > 0)
        {
            startInfo.ArgumentList.Add("--cache-size");
            startInfo.ArgumentList.Add(cacheOpts.VolumeSizeBytes.ToString());
        }
        if (socketPath != null)
        {
            startInfo.ArgumentList.Add("--socket");
            startInfo.ArgumentList.Add(socketPath);
        }
        // Resolve the worker's management-API port.
        //
        //   ApiPort > 0  → customer-set; honor exactly.
        //   ApiPort == 0 → auto-allocate an ephemeral loopback port so the
        //                  middleware-served /console/ SPA and its worker
        //                  proxy always have a worker to talk to. Loopback only;
        //                  never bound on 0.0.0.0.
        int apiPort = workerOpts.ApiPort;
        if (apiPort <= 0)
        {
            apiPort = EphemeralLoopbackPort.Allocate();
            _logger.LogDebug(
                "Worker.ApiPort=0 → auto-allocated ephemeral loopback port {Port}",
                apiPort);
        }
        _endpoint.SetPort(apiPort);
        startInfo.ArgumentList.Add("--api-port");
        startInfo.ArgumentList.Add(apiPort.ToString());
        // The worker refuses to start with an enabled but
        // tokenless management API unless the caller says so deliberately.
        // This one is deliberate and always has been: an ephemeral LOOPBACK
        // port, spawned by and for this process, proxied only through the
        // middleware's own /console/ route. The transport is the boundary.
        startInfo.ArgumentList.Add("--api-no-auth");
        if (workerOpts.LogLevel != null)
        {
            startInfo.ArgumentList.Add("--log-level");
            startInfo.ArgumentList.Add(workerOpts.LogLevel);
        }
        var consoleDir = ResolveConsoleDir(workerOpts, workerPath);
        if (consoleDir != null)
        {
            startInfo.ArgumentList.Add("--console-dir");
            startInfo.ArgumentList.Add(consoleDir);
        }

        _logger.LogDebug("Worker args: {Args}", string.Join(" ", startInfo.ArgumentList));

        try
        {
            _process = Process.Start(startInfo);
            if (_process == null)
            {
                _logger.LogError("Failed to start factory_worker process");
                return;
            }

            // Drain stdout/stderr to prevent OS pipe buffer deadlock.
            _ = Task.Run(async () =>
            {
                try
                {
                    while (await _process.StandardOutput.ReadLineAsync() is { } line)
                        _logger.LogDebug("worker stdout: {Line}", line);
                }
                catch { /* process exited */ }
            }, stoppingToken);
            _ = Task.Run(async () =>
            {
                try
                {
                    while (await _process.StandardError.ReadLineAsync() is { } line)
                        _logger.LogDebug("worker stderr: {Line}", line);
                }
                catch { /* process exited */ }
            }, stoppingToken);

            _logger.LogInformation(
                "factory_worker started (PID {Pid})", _process.Id);

            try
            {
                await _process.WaitForExitAsync(stoppingToken);
            }
            catch (OperationCanceledException)
            {
                // Shutdown requested.
            }
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "factory_worker process failed");
        }
    }

    /// <inheritdoc/>
    public override void Dispose()
    {
        try
        {
            if (_process is { HasExited: false })
            {
                _process.Kill(entireProcessTree: true);
                if (!_process.WaitForExit(TimeSpan.FromSeconds(5)))
                    _logger.LogWarning(
                        "factory_worker (PID {Pid}) did not exit within 5s after Kill",
                        _process.Id);
            }
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Error stopping factory_worker");
        }
        _process?.Dispose();
        base.Dispose();
    }

    private static string? ResolveConsoleDir(WorkerOptions opts, string workerPath)
    {
        // Explicitly set (empty string = disabled)
        if (opts.ConsoleDir != null)
            return opts.ConsoleDir.Length > 0 ? opts.ConsoleDir : null;

        // Auto-discover: look for console/ next to factory_worker
        var dir = Path.GetDirectoryName(workerPath);
        if (dir == null) return null;
        var consolePath = Path.Combine(dir, "console");
        return Directory.Exists(consolePath) ? consolePath : null;
    }

    private static string? ResolveWorkerPath()
    {
        var rid = RuntimeInformation.RuntimeIdentifier;
        // AppContext.BaseDirectory resolves to the app's output directory
        // under both classic and single-file/AOT layouts, whereas
        // Assembly.Location returns an empty string when the assembly is
        // embedded in a single-file bundle (IL3000). MSBuild's NuGet
        // runtime resolution copies factory_worker[.exe] to
        //   <appdir>/runtimes/<rid>/native/
        // so this path resolves identically across layouts.
        var appDir = AppContext.BaseDirectory;
        var binaryName = RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
            ? "factory_worker.exe"
            : "factory_worker";

        // 1. Try runtimes/{rid}/native/ (NuGet package layout)
        var nugetPath = Path.Combine(appDir, "runtimes", rid, "native", binaryName);
        if (File.Exists(nugetPath))
            return nugetPath;

        // 2. Try alongside the app binary (local dev / direct copy)
        var localPath = Path.Combine(appDir, binaryName);
        if (File.Exists(localPath))
            return localPath;

        return null;
    }

    /// <summary>
    /// Creates the directory that holds the worker's cache volume, so the
    /// worker can start against it.
    /// </summary>
    /// <remarks>
    /// Created owner-only (0700) on Unix: the volume, the shared config and
    /// the serve-stats mmap all live in here, and for an embedded worker the
    /// only peer that ever needs them is this application, running as the same
    /// user. An existing directory is left exactly as it is -- if an operator
    /// pointed the cache at a directory they had already set up, its
    /// permissions are theirs to choose.
    ///
    /// Failure is logged, not thrown: the worker is a best-effort background
    /// service, and the daemon's own startup message names the cause better
    /// than a guess made here would. PageSpeedCache reports the
    /// permission-denied case with remediation when the same path is opened on
    /// the request side.
    /// </remarks>
    internal static void EnsureCacheDirectory(string volumePath, ILogger logger)
    {
        string? dir;
        try
        {
            dir = Path.GetDirectoryName(volumePath);
        }
        catch (ArgumentException)
        {
            // A malformed path is the options validator's problem, not ours.
            return;
        }

        if (string.IsNullOrEmpty(dir) || Directory.Exists(dir))
            return;

        try
        {
            Directory.CreateDirectory(dir);
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                File.SetUnixFileMode(
                    dir,
                    UnixFileMode.UserRead | UnixFileMode.UserWrite
                        | UnixFileMode.UserExecute);
            }
            logger.LogDebug("Created worker cache directory {Directory}", dir);
        }
        catch (Exception ex) when (ex is IOException
                                       or UnauthorizedAccessException
                                       or NotSupportedException)
        {
            logger.LogWarning(
                ex,
                "Could not create the worker cache directory {Directory}. The "
                + "worker refuses to start without it; set Cache.VolumePath "
                + "(PageSpeed:Cache:VolumePath) to a location this application "
                + "can write.",
                dir);
        }
    }
}
