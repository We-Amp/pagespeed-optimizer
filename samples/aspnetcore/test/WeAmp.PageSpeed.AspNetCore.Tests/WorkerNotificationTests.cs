// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers.Binary;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NSubstitute;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

public class WorkerNotificationTests
{
    [Fact]
    public void TryNotify_ReturnsTrue_WhenChannelHasCapacity()
    {
        // Arrange
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var logger = Substitute.For<ILogger<WorkerNotificationService>>();
        var service = new WorkerNotificationService(optionsMonitor, new InternalWorkerEndpoint(), logger);

        // Act
        bool result = service.TryNotify(
            "http://example.com/page.html",
            "example.com",
            "https",
            PageSpeedContentType.Html,
            0);

        // Assert
        Assert.True(result);
    }

    [Fact]
    public void TryNotify_ReturnsTrue_ForMultipleNotifications()
    {
        // Arrange
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var logger = Substitute.For<ILogger<WorkerNotificationService>>();
        var service = new WorkerNotificationService(optionsMonitor, new InternalWorkerEndpoint(), logger);

        // Act: enqueue several notifications.
        bool result1 = service.TryNotify(
            "http://example.com/page1.html", "example.com", "http",
            PageSpeedContentType.Html, 0);
        bool result2 = service.TryNotify(
            "http://example.com/page2.html", "example.com", "http",
            PageSpeedContentType.Css, 0x08);
        bool result3 = service.TryNotify(
            "http://example.com/image.jpg", "example.com", "http",
            PageSpeedContentType.Image, 0x01);

        // Assert: all should succeed since channel capacity is 1024.
        Assert.True(result1);
        Assert.True(result2);
        Assert.True(result3);
    }

    [Fact]
    public void TryNotify_AcceptsAllContentTypes()
    {
        // Arrange
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var logger = Substitute.For<ILogger<WorkerNotificationService>>();
        var service = new WorkerNotificationService(optionsMonitor, new InternalWorkerEndpoint(), logger);

        // Act & Assert: all content types should be accepted.
        Assert.True(service.TryNotify(
            "http://example.com/a", "example.com", "http",
            PageSpeedContentType.Html, 0));
        Assert.True(service.TryNotify(
            "http://example.com/b", "example.com", "http",
            PageSpeedContentType.Css, 0));
        Assert.True(service.TryNotify(
            "http://example.com/c", "example.com", "http",
            PageSpeedContentType.Js, 0));
        Assert.True(service.TryNotify(
            "http://example.com/d", "example.com", "http",
            PageSpeedContentType.Image, 0));
        Assert.True(service.TryNotify(
            "http://example.com/e", "example.com", "http",
            PageSpeedContentType.Other, 0));
    }

    // ── agent_request + the v3→v4 regression fix ─────────────────────

    [Fact]
    public void TryNotify_WithAgentRequest_ReturnsTrue()
    {
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var service = new WorkerNotificationService(
            optionsMonitor, new InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());

        Assert.True(service.TryNotify(
            "http://example.com/p", "example.com", "https",
            PageSpeedContentType.Html, 0, agentRequest: true));
        // Back-compat: the 5-arg form (agentRequest defaults false) still binds.
        Assert.True(service.TryNotify(
            "http://example.com/q", "example.com", "https",
            PageSpeedContentType.Html, 0));
    }

    /// <summary>
    /// The load-bearing regression proof: the .NET writer must emit a v5 frame
    /// (worker_ipc.h kIpcVersion=5) carrying the agent_request byte and the
    /// empty option-context tail — the worker rejects any other version
    /// outright, so a frame one version behind is dropped and cache-fill
    /// silently stops. Drives the real socket writer against a loopback
    /// Unix-domain listener and inspects the bytes. Also proves the dedup key
    /// separates agent vs non-agent notifications for the same URL.
    /// Unix-socket only (Windows uses named pipes).
    /// </summary>
    [Fact]
    public async Task SendsV5Frame_WithAgentRequestByte_AndDedupSeparatesAgent()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return; // named pipes

        var sockPath = $"/tmp/psn-{Guid.NewGuid():N}".Substring(0, 24) + ".sock";
        using var cts = new System.Threading.CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var listener = new Socket(
            AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        try
        {
            listener.Bind(new UnixDomainSocketEndPoint(sockPath));
            listener.Listen(1);

            var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
            optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
            var endpoint = new InternalWorkerEndpoint();
            var service = new WorkerNotificationService(
                optionsMonitor, endpoint,
                Substitute.For<ILogger<WorkerNotificationService>>());

            await service.StartAsync(cts.Token);
            endpoint.SetSocketPath(sockPath);

            var acceptTask = listener.AcceptAsync(cts.Token).AsTask();

            // Same URL+type, differing only in agentRequest → distinct dedup
            // keys → BOTH delivered (not collapsed).
            Assert.True(service.TryNotify(
                "http://example.com/page", "example.com", "https",
                PageSpeedContentType.Html, 0x08, agentRequest: true));
            Assert.True(service.TryNotify(
                "http://example.com/page", "example.com", "https",
                PageSpeedContentType.Html, 0x08, agentRequest: false));

            using var conn = await acceptTask;
            var frame1 = await ReadFrameAsync(conn, cts.Token);
            var frame2 = await ReadFrameAsync(conn, cts.Token);

            // [4 total_len][1 version]...[1 scheme][1 agent_request]
            //   [4 option_context_len][1 option_signature_len]
            Assert.Equal(5, frame1[4]);          // version == kIpcVersion (v5)
            Assert.Equal(5, frame2[4]);

            // The option context is empty, so the frame ends in four zero
            // length bytes and one zero length byte. Asserted explicitly:
            // a writer that emitted a payload without its signature, or a
            // signature without its payload, would be refused by the reader,
            // and the shape is the only thing standing between here and that.
            Assert.Equal(0, frame1[^5]);         // option_context_length b0
            Assert.Equal(0, frame1[^4]);
            Assert.Equal(0, frame1[^3]);
            Assert.Equal(0, frame1[^2]);         // option_context_length b3
            Assert.Equal(0, frame1[^1]);         // option_signature_length

            // agent_request sits immediately before that five-byte tail.
            Assert.Equal(1, frame1[^6]);         // agent_request == 1
            Assert.Equal(0, frame2[^6]);         // agent_request == 0

            await service.StopAsync(cts.Token);
        }
        finally
        {
            try { System.IO.File.Delete(sockPath); } catch { /* best effort */ }
        }
    }

    private static async Task<byte[]> ReadFrameAsync(
        Socket s, System.Threading.CancellationToken ct)
    {
        var header = await ReadExactlyAsync(s, 4, ct);
        int payloadLen = BinaryPrimitives.ReadInt32BigEndian(header);
        var payload = await ReadExactlyAsync(s, payloadLen, ct);
        var frame = new byte[4 + payloadLen];
        header.CopyTo(frame, 0);
        payload.CopyTo(frame, 4);
        return frame;
    }

    private static async Task<byte[]> ReadExactlyAsync(
        Socket s, int n, System.Threading.CancellationToken ct)
    {
        var buf = new byte[n];
        int got = 0;
        while (got < n)
        {
            int r = await s.ReceiveAsync(buf.AsMemory(got), SocketFlags.None, ct);
            if (r == 0) throw new System.IO.IOException("socket closed early");
            got += r;
        }
        return buf;
    }
}
