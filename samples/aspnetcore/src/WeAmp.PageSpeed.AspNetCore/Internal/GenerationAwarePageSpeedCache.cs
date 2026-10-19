// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The cache the middleware uses: a <see cref="IPageSpeedCache"/> that
/// follows the worker's volume resets.
/// </summary>
/// <remarks>
/// <para>A full purge (<c>POST /v1/cache/purge</c> with
/// <c>scope: "all"</c>) makes the worker delete the volume file and create a
/// fresh one, then bump the counter in <c>{VolumePath}.gen</c>
/// (PageSpeedCache::ResetVolume). A process that keeps its open cache keeps
/// mapping the deleted file and serves its pre-purge entries as HITs until it
/// restarts. The nginx module follows the counter
/// (CheckGenerationAndGetCache); this class is the middleware's equivalent.</para>
///
/// <para>At most once per <c>checkInterval</c> (1 s, the nginx module's
/// cadence) one request reads the counter file; every other request pays one
/// clock read and a compare. When the counter moved, a fresh cache is opened
/// and published, and the retired instance is disposed once no call is still
/// running on it. Read results and writers already handed out stay valid
/// after that: each holds its own reference on the native cache (see
/// <see cref="SafeCacheChildHandle"/>), so a response still streaming from
/// the old volume completes.</para>
///
/// <para>If the fresh cache cannot be opened, the current one stays in use
/// and the reopen is retried at the next check.</para>
/// </remarks>
internal sealed class GenerationAwarePageSpeedCache : IPageSpeedCache
{
    internal static readonly TimeSpan DefaultCheckInterval = TimeSpan.FromSeconds(1);

    /// <summary>One opened cache plus the generation it was opened at.</summary>
    private sealed class Slot
    {
        public readonly IPageSpeedCache Cache;
        public readonly ulong Generation;
        // One reference is owned by the decorator while this slot is current;
        // each in-flight call holds one more. Zero = disposed, never revived.
        private int _refs = 1;

        public Slot(IPageSpeedCache cache, ulong generation)
        {
            Cache = cache;
            Generation = generation;
        }

        public bool TryAcquire()
        {
            while (true)
            {
                int refs = Volatile.Read(ref _refs);
                if (refs == 0) return false;
                if (Interlocked.CompareExchange(ref _refs, refs + 1, refs) == refs)
                    return true;
            }
        }

        public void Release()
        {
            if (Interlocked.Decrement(ref _refs) == 0)
                Cache.Dispose();
        }
    }

    private readonly Func<IPageSpeedCache> _factory;
    private readonly string _generationPath;
    private readonly long _intervalMs;
    private readonly Func<long> _clockMs;
    private readonly ILogger _logger;
    private readonly object _swapLock = new();
    private Slot? _current; // null once disposed
    private long _nextCheckMs;

    /// <param name="factory">Opens a cache on the configured volume.</param>
    /// <param name="generationPath">
    /// The worker's generation counter file, <c>{VolumePath}.gen</c>.
    /// </param>
    /// <param name="logger">Optional logger.</param>
    /// <param name="checkInterval">
    /// Minimum time between two reads of the counter file.
    /// </param>
    /// <param name="clockMs">Monotonic milliseconds; tests substitute it.</param>
    public GenerationAwarePageSpeedCache(
        Func<IPageSpeedCache> factory,
        string generationPath,
        ILogger? logger = null,
        TimeSpan? checkInterval = null,
        Func<long>? clockMs = null)
    {
        _factory = factory;
        _generationPath = generationPath;
        _logger = logger ?? NullLogger.Instance;
        _intervalMs = (long)(checkInterval ?? DefaultCheckInterval).TotalMilliseconds;
        _clockMs = clockMs ?? (() => Environment.TickCount64);

        // Read the counter BEFORE opening: a reset that lands between the two
        // then shows up as a changed counter at the next check (one extra
        // reopen at worst) instead of being recorded as already seen.
        ulong generation = ReadGeneration(_generationPath) ?? 0;
        _current = new Slot(_factory(), generation);
        _nextCheckMs = _clockMs() + _intervalMs;
    }

    /// <summary>
    /// Reads the counter the way PageSpeedCache::ReadGenerationFile does:
    /// a missing or unparsable file is generation 0. Returns null when the
    /// file exists but could not be read right now (try again later).
    /// </summary>
    internal static ulong? ReadGeneration(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (FileNotFoundException)
        {
            return 0;
        }
        catch (DirectoryNotFoundException)
        {
            return 0;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
        return ulong.TryParse(text.AsSpan().Trim(), out var gen) ? gen : 0;
    }

    /// <summary>The generation of the cache currently in use (for tests).</summary>
    internal ulong CurrentGeneration =>
        (Volatile.Read(ref _current) ?? throw new ObjectDisposedException(GetType().Name))
            .Generation;

    private void MaybeCheckGeneration()
    {
        long now = _clockMs();
        long next = Volatile.Read(ref _nextCheckMs);
        if (now < next) return;
        // One caller per interval wins the slot and does the file read.
        if (Interlocked.CompareExchange(ref _nextCheckMs, now + _intervalMs, next) != next)
            return;
        CheckGenerationNow();
    }

    /// <summary>
    /// Reads the counter file and, when it moved, swaps to a freshly opened
    /// cache. Not throttled.
    /// </summary>
    internal void CheckGenerationNow()
    {
        ulong? read = ReadGeneration(_generationPath);
        if (read is not ulong generation) return;
        var current = Volatile.Read(ref _current);
        if (current == null || current.Generation == generation) return;

        lock (_swapLock)
        {
            current = Volatile.Read(ref _current);
            if (current == null || current.Generation == generation) return;

            IPageSpeedCache fresh;
            try
            {
                fresh = _factory();
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex,
                    "PageSpeed cache generation changed {Old} -> {New} (volume reset), "
                    + "but reopening the cache failed; still using the previous "
                    + "volume, retrying at the next check",
                    current.Generation, generation);
                return;
            }

            // Publish first, then drop the owner reference: a caller that read
            // the old slot and fails to acquire it re-reads and finds this one.
            Volatile.Write(ref _current, new Slot(fresh, generation));
            current.Release();
            _logger.LogInformation(
                "PageSpeed cache generation changed {Old} -> {New} (volume reset), "
                + "reopened the cache",
                current.Generation, generation);
        }
    }

    private Slot Acquire()
    {
        MaybeCheckGeneration();
        while (true)
        {
            var slot = Volatile.Read(ref _current)
                ?? throw new ObjectDisposedException(GetType().Name);
            if (slot.TryAcquire()) return slot;
            // Retired between the read and the acquire; its successor is
            // already published.
        }
    }

    public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadBest(url, hostname, scheme, mask); }
        finally { slot.Release(); }
    }

    public IReadResult? ReadBestAgent(
        string url, string hostname, string scheme, uint mask, int agentEntitled)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadBestAgent(url, hostname, scheme, mask, agentEntitled); }
        finally { slot.Release(); }
    }

    public int ReadSharedConfigAgentEntitled()
    {
        var slot = Acquire();
        try { return slot.Cache.ReadSharedConfigAgentEntitled(); }
        finally { slot.Release(); }
    }

    public IReadResult? ReadAlternate(
        string url, string hostname, string scheme, byte alternateId)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadAlternate(url, hostname, scheme, alternateId); }
        finally { slot.Release(); }
    }

    public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadEarlyHints(url, hostname, scheme); }
        finally { slot.Release(); }
    }

    public CacheWriter BeginWrite(
        string url, string hostname, string scheme, CacheWriteParams p)
    {
        var slot = Acquire();
        try { return slot.Cache.BeginWrite(url, hostname, scheme, p); }
        finally { slot.Release(); }
    }

    public CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId, ulong contentLength)
    {
        var slot = Acquire();
        try
        {
            return slot.Cache.BeginWriteSentinel(
                url, hostname, scheme, sentinelId, contentLength);
        }
        finally { slot.Release(); }
    }

    public CacheStats GetStats()
    {
        var slot = Acquire();
        try { return slot.Cache.GetStats(); }
        finally { slot.Release(); }
    }

    public void RecordServeHit(
        PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask)
    {
        var slot = Acquire();
        try { slot.Cache.RecordServeHit(type, originalBytes, optimizedBytes, mask); }
        finally { slot.Release(); }
    }

    /// <summary>
    /// The current instance's handle, for a native call made by the caller.
    /// A swap can close it between this getter and that call; the call then
    /// fails with <see cref="ObjectDisposedException"/> and the caller reads
    /// this property again (see HtmlProcessor).
    /// </summary>
    SafeCacheHandle IPageSpeedCache.Handle
    {
        get
        {
            var slot = Volatile.Read(ref _current)
                ?? throw new ObjectDisposedException(GetType().Name);
            return slot.Cache.Handle;
        }
    }

    public void Dispose()
    {
        lock (_swapLock)
        {
            Interlocked.Exchange(ref _current, null)?.Release();
        }
    }
}
