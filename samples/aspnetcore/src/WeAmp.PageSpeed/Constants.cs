// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

namespace WeAmp.PageSpeed;

/// <summary>
/// Sentinel alternate IDs for special cache entries.
/// Pass to ps_cache_write_sentinel / ps_cache_read_alternate.
/// </summary>
public static class SentinelId
{
    public const byte EarlyHints = 0x1C;
    public const byte Warmup = 0x2C;
    public const byte ContentHash = 0x3C;
    public const byte Subresource = 0x4C;
    public const byte BrowserProfile = 0x5C;

    /// <summary>
    /// The agent_optimize rendered-DOM markdown variant
    /// (lib/classify/alternate_id.h kAgentMarkdown). A served variant whose
    /// <c>(Mask &amp; 0xFF)</c> equals this is the markdown body — never written
    /// from managed code; the worker is the sole writer.
    /// </summary>
    public const byte AgentMarkdown = 0x7C;
}

/// <summary>Cache entry flags.</summary>
public static class CacheFlags
{
    public const byte NeedsRevalidation = 0x01;

    // NOTE (#454): bit 0x02 is C++ kFlagWorkerProcessed (worker dedup) — NOT an
    // origin-revalidation signal. The real signal is now exposed via OriginCcFlags
    // (ReadResult.OriginCcFlags) and is plumbed through ps_parse_cache_control.
}

/// <summary>Origin Cache-Control directive bits (PS_CC_ORIGIN_*).</summary>
public static class CacheControlCcFlags
{
    public const ushort NoCache = 0x0001;
    public const ushort MustRevalidate = 0x0002;
    public const ushort NoStore = 0x0004;
    public const ushort Private = 0x0008;
    public const ushort Public = 0x0010;
    public const ushort Immutable = 0x0020;
    public const ushort SMaxagePresent = 0x0040;
    public const ushort ProxyRevalidate = 0x0080;
    public const ushort NoTransform = 0x0100;
    public const ushort HeaderPresent = 0x0200;
    public const ushort PrivateQualified = 0x0400;
    public const ushort PrivateBare = 0x0800;
    public const ushort NoCacheQualified = 0x1000;
    public const ushort NoCacheBare = 0x2000;
}
