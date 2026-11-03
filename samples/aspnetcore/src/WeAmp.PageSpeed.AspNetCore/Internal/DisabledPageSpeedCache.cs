// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Cache stand-in used when <see cref="PageSpeedOptions.Enabled"/> is false
/// at startup. It satisfies DI without opening a volume file.
/// </summary>
internal sealed class DisabledPageSpeedCache : IPageSpeedCache
{
    public void Dispose()
    {
    }

    public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
        => null;

    public IReadResult? ReadBestAgent(
        string url, string hostname, string scheme, uint mask, int agentEntitled)
        => null;

    public int ReadSharedConfigAgentEntitled() => 0;

    public IReadResult? ReadAlternate(
        string url, string hostname, string scheme, byte alternateId)
        => null;

    public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
        => null;

    public CacheWriter BeginWrite(
        string url, string hostname, string scheme, CacheWriteParams p)
        => throw Disabled();

    public CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId, ulong contentLength)
        => throw Disabled();

    public CacheStats GetStats() => new(
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    public void RecordServeHit(
        PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask)
    {
    }

    SafeCacheHandle IPageSpeedCache.Handle => throw Disabled();

    private static InvalidOperationException Disabled() => new(
        "PageSpeed is disabled; the cache volume was not created.");
}
