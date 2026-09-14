// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers.Binary;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Drains worker notifications via a bounded Channel with a persistent
/// connection to the Worker process. Uses Unix domain sockets on Linux/macOS
/// and Named Pipes on Windows. Reconnects automatically on failure.
/// Deduplicates notifications within a cooldown window.
/// </summary>
public sealed class WorkerNotificationService : BackgroundService
{
    private readonly Channel<WorkerNotification> _channel =
        Channel.CreateBounded<WorkerNotification>(
            new BoundedChannelOptions(1024)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
            });
    private static readonly bool IsWindows =
        RuntimeInformation.IsOSPlatform(OSPlatform.Windows);

    private readonly IOptionsMonitor<PageSpeedOptions> _options;
    private readonly InternalWorkerEndpoint _endpoint;
    private readonly ILogger<WorkerNotificationService> _logger;
    private Socket? _socket;           // Unix domain socket (Linux/macOS)
    private NamedPipeClientStream? _pipe; // Named pipe (Windows)
    private string? _connectedPath;

    // Deduplication: skip re-notifying the same URL+hostname+scheme+type within
    // 5 seconds. AgentRequest is part of the key: an agent-markdown
    // notification asks the worker for DIFFERENT work than a normal one for the
    // same URL, so the two must not collapse into a single dedup entry.
    private readonly Dictionary<(string Url, string Hostname, string Scheme, PageSpeedContentType Type, bool AgentRequest), long> _dedup = new();
    private const long DedupCooldownMs = 5000;

    /// <summary>
    /// Creates a new worker notification service.
    /// </summary>
    public WorkerNotificationService(
        IOptionsMonitor<PageSpeedOptions> options,
        InternalWorkerEndpoint endpoint,
        ILogger<WorkerNotificationService> logger)
    {
        _options = options;
        _endpoint = endpoint;
        _logger = logger;
    }

    /// <summary>
    /// Enqueue a notification (non-blocking, drops oldest on overflow).
    /// </summary>
    public bool TryNotify(
        string url, string hostname, string scheme,
        PageSpeedContentType contentType, uint mask, bool agentRequest = false)
    {
        if (scheme != "http" && scheme != "https") return false;

        return _channel.Writer.TryWrite(
            new WorkerNotification(url, hostname, scheme, contentType, mask, agentRequest));
    }

    /// <inheritdoc/>
    protected override async Task ExecuteAsync(
        CancellationToken stoppingToken)
    {
        // Resolve the worker's IPC endpoint once. WorkerProcessHost pins the
        // path it actually launched the worker with — we must NOT re-derive it
        // from our own IOptionsMonitor instance, because IOptions (used by the
        // host) and IOptionsMonitor (used here) hand out distinct WorkerOptions
        // instances, so an independently-resolved auto-path would point at a
        // socket nobody is listening on. A null pinned path means worker
        // coordination is disabled.
        string? socketPath;
        try
        {
            socketPath = await _endpoint.SocketPathReady.WaitAsync(stoppingToken);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (socketPath == null)
        {
            _logger.LogInformation(
                "Worker coordination disabled (no socket path); "
                + "notifications will be drained and discarded.");
        }

        await foreach (var n in _channel.Reader.ReadAllAsync(stoppingToken))
        {
            if (socketPath == null) continue;

            // Deduplication: skip if same URL+hostname+scheme+type was notified recently.
            var now = Environment.TickCount64;
            var key = (n.Url, n.Hostname, n.Scheme, n.ContentType, n.AgentRequest);
            if (_dedup.TryGetValue(key, out var lastTick) &&
                (now - lastTick) < DedupCooldownMs)
            {
                continue;
            }
            _dedup[key] = now;

            // Periodic cleanup of dedup dictionary.
            if (_dedup.Count > 4096)
                PruneDedupEntries(now);

            try
            {
                await SendNotificationAsync(socketPath, n, stoppingToken);
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex,
                    "Worker notification failed for {Url}", n.Url);
                Disconnect();
            }
        }
    }

    // Wire protocol version — MUST match the worker's src/proto/worker_ipc.h
    // kIpcVersion. The reader rejects any other version outright (no
    // cross-version tolerance), so this is bumped in lockstep with the C++ side.
    private const byte IpcVersion = 5;

    /// <summary>
    /// Serializes and sends a notification over the persistent socket.
    /// Wire format (big-endian, v5 — matches worker_ipc.h):
    ///   [4: total_length][1: version=5][4: url_length][url][4: host_length][host][1: ct][4: mask][1: scheme_byte][1: agent_request][4: option_context_length][option_context][1: option_signature_length][option_signature]
    /// total_length covers everything after itself (version + fields).
    /// scheme_byte: 0x01 = http, 0x02 = https (matches C++ IpcScheme).
    /// agent_request: 0/1 (the markdown-render demand bit).
    ///
    /// The v5 option-context fields are emitted EMPTY. This middleware does not
    /// resolve per-request PageSpeed configuration — there is one configuration
    /// for the process — so it has no context to declare, and an empty context
    /// means exactly that. Both length fields are still written: the frame's
    /// shape is fixed by its version, not by which fields happen to carry
    /// anything, and a v5 reader stops at the length its own header declares.
    ///
    /// Supplying a context would mean supplying the payload AND its signature
    /// together; a payload without a signature is refused by the reader,
    /// because the signature can only be checked against the payload it came
    /// with, and that check is the point of carrying it.
    /// </summary>
    private async ValueTask SendNotificationAsync(
        string socketPath, WorkerNotification n,
        CancellationToken ct)
    {
        await EnsureConnectedAsync(socketPath, ct);

        var urlByteCount = Encoding.UTF8.GetByteCount(n.Url);
        var hostByteCount = Encoding.UTF8.GetByteCount(n.Hostname);
        // v5 payload after total_length: version(1) + url_len(4) + url +
        // host_len(4) + host + ct(1) + mask(4) + scheme(1) + agent_request(1)
        // + option_context_len(4) + option_context(0) + option_sig_len(1)
        // + option_signature(0)
        var payloadLen =
            1 + 4 + urlByteCount + 4 + hostByteCount + 1 + 4 + 1 + 1 + 4 + 1;
        // total on wire: total_length(4) + payload
        var totalLen = 4 + payloadLen;
        var buffer = new byte[totalLen];
        var span = buffer.AsSpan();

        // total_length (big-endian, not including this 4-byte field itself).
        BinaryPrimitives.WriteInt32BigEndian(span[0..], payloadLen);
        // version byte.
        span[4] = IpcVersion;
        // url_length + url.
        BinaryPrimitives.WriteInt32BigEndian(span[5..], urlByteCount);
        Encoding.UTF8.GetBytes(n.Url, span[9..]);
        var offset = 9 + urlByteCount;
        // hostname_length + hostname.
        BinaryPrimitives.WriteInt32BigEndian(span[offset..], hostByteCount);
        Encoding.UTF8.GetBytes(n.Hostname, span[(offset + 4)..]);
        offset += 4 + hostByteCount;
        // content_type (1 byte).
        span[offset] = (byte)n.ContentType;
        // capability_mask (big-endian).
        BinaryPrimitives.WriteUInt32BigEndian(span[(offset + 1)..], n.Mask);
        // Scheme byte: 0x01 = http, 0x02 = https (matches C++ IpcScheme).
        span[offset + 5] = n.Scheme == "https" ? (byte)0x02 : (byte)0x01;
        // agent_request byte (v4): the markdown-render demand bit.
        span[offset + 6] = n.AgentRequest ? (byte)1 : (byte)0;
        // option_context_length (v5) = 0, then option_signature_length = 0.
        // Written explicitly rather than left to the zero-initialised buffer,
        // so that the frame layout is visible here and a future non-empty
        // context has an obvious place to go.
        BinaryPrimitives.WriteInt32BigEndian(span[(offset + 7)..], 0);
        span[offset + 11] = 0;

        try
        {
            await SendAllAsync(buffer, ct);
        }
        catch (Exception ex) when (ex is SocketException or IOException)
        {
            // Connection broken — reconnect and retry once.
            Disconnect();
            await EnsureConnectedAsync(socketPath, ct);
            await SendAllAsync(buffer, ct);
        }
    }

    private async ValueTask SendAllAsync(byte[] buffer, CancellationToken ct)
    {
        if (IsWindows)
        {
            await _pipe!.WriteAsync(buffer, ct);
            await _pipe.FlushAsync(ct);
        }
        else
        {
            var remaining = buffer.AsMemory();
            while (remaining.Length > 0)
            {
                var sent = await _socket!.SendAsync(
                    remaining, SocketFlags.None, ct);
                if (sent == 0)
                    throw new SocketException(
                        (int)SocketError.ConnectionReset);
                remaining = remaining[sent..];
            }
        }
    }

    private async ValueTask EnsureConnectedAsync(
        string socketPath, CancellationToken ct)
    {
        if (_connectedPath == socketPath)
        {
            if (IsWindows && _pipe is { IsConnected: true })
                return;
            if (!IsWindows && _socket is { Connected: true })
                return;
        }

        Disconnect();

        if (IsWindows)
        {
            // Worker prepends \\.\pipe\ to socket_path.  The
            // NamedPipeClientStream ctor adds that prefix internally,
            // so we pass the raw socket_path as the pipe name.  If the
            // config already carries the prefix, strip it.
            var pipeName = socketPath;
            const string pipePrefix = @"\\.\pipe\";
            if (pipeName.StartsWith(pipePrefix, StringComparison.Ordinal))
                pipeName = pipeName[pipePrefix.Length..];

            var pipe = new NamedPipeClientStream(
                ".", pipeName, PipeDirection.Out, PipeOptions.Asynchronous);
            try
            {
                await pipe.ConnectAsync(ct);
                _pipe = pipe;
                _connectedPath = socketPath;
            }
            catch
            {
                pipe.Dispose();
                throw;
            }
        }
        else
        {
            var socket = new Socket(
                AddressFamily.Unix, SocketType.Stream,
                ProtocolType.Unspecified);
            try
            {
                await socket.ConnectAsync(
                    new UnixDomainSocketEndPoint(socketPath), ct);
                _socket = socket;
                _connectedPath = socketPath;
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        }
    }

    private void Disconnect()
    {
        if (_pipe != null)
        {
            try { _pipe.Dispose(); }
            catch { /* best-effort cleanup */ }
            _pipe = null;
        }
        if (_socket != null)
        {
            try { _socket.Dispose(); }
            catch { /* best-effort cleanup */ }
            _socket = null;
        }
        _connectedPath = null;
    }

    private void PruneDedupEntries(long now)
    {
        var threshold = DedupCooldownMs;
        var stale = new List<(string, string, string, PageSpeedContentType, bool)>();
        foreach (var (k, v) in _dedup)
        {
            if ((now - v) >= threshold)
                stale.Add(k);
        }
        foreach (var k in stale)
            _dedup.Remove(k);
    }

    /// <inheritdoc/>
    public override void Dispose()
    {
        _channel.Writer.TryComplete();
        Disconnect();
        base.Dispose();
    }

    // Value type: stored inline in Channel segments, avoids heap allocation.
    private readonly record struct WorkerNotification(
        string Url, string Hostname, string Scheme,
        PageSpeedContentType ContentType, uint Mask, bool AgentRequest);
}
