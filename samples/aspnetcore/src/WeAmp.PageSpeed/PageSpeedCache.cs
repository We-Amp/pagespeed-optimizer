// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using System.Text;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// Managed wrapper around the native PageSpeed cache.
/// Thread-safe for concurrent reads. Intended as a singleton in DI.
/// </summary>
public sealed class PageSpeedCache : IPageSpeedCache
{
    private readonly SafeCacheHandle _handle;
    private int _disposed;

    // Serve-stats mmap handle. The worker is the sole CREATOR of
    // {parent}/.pagespeed-serve-stats; this process only OPENs it. The file
    // may not exist yet when the cache is constructed (worker still starting),
    // so the open is lazy: IntPtr.Zero until the first successful open, then
    // cached. Guarded by _serveStatsLock so the concurrent-singleton's first
    // HITs don't race the lazy open. The native record_hit is itself lock-free
    // (relaxed atomics), so only the open is serialized.
    private readonly string _volumePath;
    private readonly object _serveStatsLock = new();
    private IntPtr _serveStats; // IntPtr.Zero until opened
    private bool _serveStatsOpened;

    public PageSpeedCache(PageSpeedCacheOptions options)
    {
        if (string.IsNullOrEmpty(options.VolumePath))
            throw new ArgumentException("VolumePath must not be null or empty.", nameof(options));

        _volumePath = options.VolumePath;

        var parentDir = Path.GetDirectoryName(options.VolumePath);
        if (!string.IsNullOrEmpty(parentDir))
        {
            try
            {
                Directory.CreateDirectory(parentDir);
            }
            catch (UnauthorizedAccessException ex)
            {
                // The default volume path (/var/cache/pagespeed/volume.dat) needs
                // elevated permissions, so a local non-root run would otherwise fail
                // here with a bare "Access to the path '/var/cache' is denied".
                throw new InvalidOperationException(
                    $"Cannot create the PageSpeed cache directory '{parentDir}' for volume " +
                    $"'{options.VolumePath}'. The default cache path requires elevated " +
                    "permissions; point the cache at a writable location (set Cache.VolumePath, " +
                    "or PageSpeed:Cache:VolumePath in ASP.NET Core configuration). See " +
                    "https://modpagespeed.com/docs/aspnet-configuration/#cache-volume.",
                    ex);
            }
        }

        var config = new NativeCacheConfig();
        NativePageSpeed.CacheConfigInit(ref config);
        config.VolumeSize = options.VolumeSizeBytes;
        config.EnableChecksum = options.EnableChecksum ? 1 : 0;
        config.RamCacheSize = (nuint)options.RamCacheSizeBytes;
        config.MaxMetadataSize = (nuint)options.MaxMetadataSizeBytes;

        var pathBytes = Encoding.UTF8.GetBytes(options.VolumePath + '\0');
        unsafe
        {
            fixed (byte* pathPtr = pathBytes)
            {
                config.VolumePath = (IntPtr)pathPtr;
                int err = NativePageSpeed.CacheOpen(in config, out var raw);
                NativeCheck.ThrowOnError(err);
                _handle = new SafeCacheHandle(raw);
            }
        }

        // Try to open the serve-stats mmap once now. If the worker hasn't
        // created the file yet (NotFound), leave it closed; RecordServeHit
        // will retry lazily on the first worker-processed HIT.
        TryOpenServeStats();
    }

    /// <summary>
    /// Attempts to open the serve-stats mmap. Returns the (possibly newly
    /// opened) handle, or IntPtr.Zero if the worker hasn't created the file
    /// yet. Idempotent and thread-safe; once opened it is cached, and a
    /// NotFound is retried on subsequent calls (so the first HITs after the
    /// worker starts self-heal). Any other native error closes the door
    /// (marks opened) to avoid thrashing the open on a permanently bad file.
    /// </summary>
    private IntPtr TryOpenServeStats()
    {
        // Fast path: once opened, _serveStats is published (Volatile.Write
        // below) before _serveStatsOpened flips, so a true read here implies a
        // fully-visible handle.
        if (Volatile.Read(ref _serveStatsOpened)) return _serveStats;
        lock (_serveStatsLock)
        {
            if (_serveStatsOpened) return _serveStats;

            int err = NativePageSpeed.ServeStatsOpen(_volumePath, out var raw);
            if (err == (int)PageSpeedError.Ok)
            {
                _serveStats = raw;
                Volatile.Write(ref _serveStatsOpened, true);
                return _serveStats;
            }
            if (err == (int)PageSpeedError.NotFound)
            {
                // Worker not up yet — keep retrying on later HITs.
                return IntPtr.Zero;
            }
            // Any other error: stop trying (treat as permanently unavailable).
            Volatile.Write(ref _serveStatsOpened, true);
            return IntPtr.Zero;
        }
    }

    SafeCacheHandle IPageSpeedCache.Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            return _handle;
        }
    }

    /// <summary>
    /// Read the best-matching alternate for the given URL and capability mask.
    /// Returns null on cache miss.
    /// </summary>
    public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        int err = NativePageSpeed.CacheReadBest(
            _handle, url, hostname, scheme, mask, out var raw);
        if (err == (int)PageSpeedError.NotFound) return null;
        NativeCheck.ThrowOnError(err);
        return new ReadResult(new SafeReadResultHandle(raw));
    }

    /// <summary>
    /// Read the best agent-markdown variant via the SINGLE audited
    /// native gate (entitlement + content-hash binding). Returns null on miss OR
    /// when the gate refuses a stale/unbound variant (PS_ERR_NOT_FOUND), so the
    /// caller falls through to HTML. <paramref name="agentEntitled"/> MUST be
    /// derived from <see cref="ReadSharedConfigAgentEntitled"/> AND the request's
    /// markdown intent — never hard-coded.
    /// </summary>
    public IReadResult? ReadBestAgent(
        string url, string hostname, string scheme, uint mask, int agentEntitled)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        int err = NativePageSpeed.CacheReadBestAgent(
            _handle, url, hostname, scheme, mask, agentEntitled, out var raw);
        if (err == (int)PageSpeedError.NotFound) return null;
        NativeCheck.ThrowOnError(err);
        return new ReadResult(new SafeReadResultHandle(raw));
    }

    /// <summary>
    /// The worker-written agent_optimize entitlement for this
    /// cache's volume. 1 = entitled; 0 = not entitled (incl. a missing config —
    /// the safe default); -1 = invalid argument.
    /// </summary>
    public int ReadSharedConfigAgentEntitled()
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        return NativePageSpeed.ReadSharedConfigAgentEntitled(_volumePath);
    }

    /// <summary>Read a specific alternate by ID. Returns null on miss.</summary>
    public IReadResult? ReadAlternate(
        string url, string hostname, string scheme, byte alternateId)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        int err = NativePageSpeed.CacheReadAlternate(
            _handle, url, hostname, scheme, alternateId, out var raw);
        if (err == (int)PageSpeedError.NotFound) return null;
        NativeCheck.ThrowOnError(err);
        return new ReadResult(new SafeReadResultHandle(raw));
    }

    /// <summary>Read early hints for a URL. Returns null on miss.</summary>
    public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        int err = NativePageSpeed.CacheReadEarlyHints(
            _handle, url, hostname, scheme, out var raw);
        if (err == (int)PageSpeedError.NotFound) return null;
        NativeCheck.ThrowOnError(err);
        return new ReadResult(new SafeReadResultHandle(raw));
    }

    /// <summary>Begin a cache write. Dispose the writer to abort.</summary>
    public CacheWriter BeginWrite(
        string url, string hostname, string scheme, CacheWriteParams writeParams)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        var native = writeParams.ToNative();

        // Pin OriginContentType as UTF-8 for the duration of ps_cache_write_begin.
        if (writeParams.OriginContentType != null)
        {
            var ctBytes = System.Text.Encoding.UTF8.GetBytes(
                writeParams.OriginContentType + '\0');
            unsafe
            {
                fixed (byte* ctPtr = ctBytes)
                {
                    native.OriginCt = (IntPtr)ctPtr;
                    int err = NativePageSpeed.CacheWriteBegin(
                        _handle, url, hostname, scheme, in native, out var raw);
                    NativeCheck.ThrowOnError(err);
                    return new CacheWriter(new SafeWriteHandle(raw));
                }
            }
        }
        else
        {
            int err = NativePageSpeed.CacheWriteBegin(
                _handle, url, hostname, scheme, in native, out var raw);
            NativeCheck.ThrowOnError(err);
            return new CacheWriter(new SafeWriteHandle(raw));
        }
    }

    /// <summary>
    /// Begin writing a sentinel entry (e.g., early hints).
    /// Dispose the writer to abort.
    /// </summary>
    public CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId,
        ulong contentLength)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        int err = NativePageSpeed.CacheWriteSentinel(
            _handle, url, hostname, scheme, sentinelId, contentLength,
            out var raw);
        NativeCheck.ThrowOnError(err);
        return new CacheWriter(new SafeWriteHandle(raw));
    }

    /// <summary>Get aggregate cache statistics.</summary>
    public CacheStats GetStats()
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        var native = new NativeCacheStats
        {
            StructSize = (nuint)Marshal.SizeOf<NativeCacheStats>()
        };
        NativeCheck.ThrowOnError(
            NativePageSpeed.CacheStats(_handle, ref native));
        return CacheStats.FromNative(native);
    }

    /// <summary>
    /// Record one worker-processed serve HIT into the shared serve-stats mmap.
    /// The caller gates on IsWorkerProcessed AND
    /// OriginContentLength &gt; 0 (the exact nginx gate). Lazily opens the mmap
    /// if the worker hadn't created it when this cache was constructed. No-op
    /// if the file still isn't available. The native record_hit is itself
    /// lock-free (relaxed atomics); the brief lock here only serializes against
    /// Dispose() unmapping the handle, not against other recorders.
    /// </summary>
    public void RecordServeHit(
        PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask)
    {
        if (_disposed != 0) return;
        var handle = TryOpenServeStats();
        if (handle == IntPtr.Zero) return;
        // Hold _serveStatsLock across the native call so Dispose() (which closes
        // the handle under the same lock, after setting _disposed) cannot unmap
        // it mid-write. The re-check of _disposed inside the lock closes the
        // last window: if Dispose already ran, we skip the (now-closed) handle.
        // Uncontended in steady state — the critical section is a single relaxed
        // atomic add; graceful shutdown drains requests before disposal anyway.
        lock (_serveStatsLock)
        {
            if (_disposed != 0) return;
            NativePageSpeed.ServeStatsRecordHit(
                handle, (int)type, originalBytes, optimizedBytes, mask);
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
        {
            _handle.Dispose();
            // Close the serve-stats mmap if we ever opened it. Take the lock
            // so we don't race a lazy open in flight on another thread.
            lock (_serveStatsLock)
            {
                if (_serveStats != IntPtr.Zero)
                {
                    NativePageSpeed.ServeStatsClose(_serveStats);
                    _serveStats = IntPtr.Zero;
                }
                Volatile.Write(ref _serveStatsOpened, true); // block further lazy opens
            }
        }
    }
}
