// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Microsoft.IO;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// ASP.NET Core middleware that intercepts responses and applies
/// PageSpeed optimizations. Handles HTML (transform + cache), images
/// (cache + worker transcoding), CSS and JS (cache + worker minification).
/// Convention-based (singleton lifetime).
/// </summary>
public sealed class PageSpeedMiddleware
{
    // Characters that could cause header injection in Link headers.
    private static readonly SearchValues<char> UnsafeLinkChars =
        SearchValues.Create("\r\n<>;,\"\0");

    private static readonly RecyclableMemoryStreamManager MsPool = new(
        new RecyclableMemoryStreamManager.Options
        {
            BlockSize = 128 * 1024,
            LargeBufferMultiple = 1024 * 1024,
            MaximumBufferSize = 8 * 1024 * 1024,
        });

    // Default capability mask: Desktop/Identity = 0x08.
    private const uint DefaultMask = 0x08;

    // Transfer encoding bits (6-7) in capability mask.
    private const uint EncodingMask = 0xC0;
    private const uint EncodingIdentity = 0x00;
    private const uint EncodingGzip = 0x40;
    private const uint EncodingBrotli = 0x80;

    // Image format bits (0-1) in capability mask.
    private const uint FormatMask = 0x03;
    private const uint FormatOriginal = 0x00;
    private const uint FormatWebP = 0x01;
    private const uint FormatAvif = 0x02;
    private const uint FormatSvg = 0x03;

    // CSP-safe async-CSS loader served at its reserved same-origin path. The
    // native path is CONTENT-ADDRESSED (a loader-body change yields a new path),
    // so it CANNOT be a managed compile-time literal. We resolve it ONCE from the
    // native single source of truth at type-init (process startup, before any
    // request is served) and cache it in a static readonly field. The per-request
    // path probe then compares against this in-memory string and NEVER calls into
    // native code — a native symbol skew must not be able to fault all GET/HEAD
    // traffic. The native lib is loaded in-process, so this path is identical to
    // the one the transform emits into HTML by construction. If the native getter
    // is unavailable/empty at startup the type initializer throws (fail-fast at
    // boot — TypeInitializationException — not a silent per-request 500). The JS
    // BODY is fetched from the same native source lazily and only after the path
    // already matched, so a body-getter skew breaks only the loader route.
    private static readonly string AsyncCssLoaderPath = ResolveAsyncCssLoaderPath();

    private static string ResolveAsyncCssLoaderPath()
    {
        // ps_async_css_loader_path is already an exported, in-process symbol
        // (the JS-body sibling reads its neighbour), so this adds no new symbol.
        var path = NativePageSpeed.AsyncCssLoaderPath();
        if (string.IsNullOrEmpty(path))
        {
            throw new InvalidOperationException(
                "Native async-CSS loader path is empty; the loaded PageSpeed " +
                "native library is missing ps_async_css_loader_path or is " +
                "ABI-incompatible. Refusing to start with an unknown loader path.");
        }
        return path;
    }

    private static readonly Lazy<byte[]> AsyncCssLoaderJsBytes =
        new(() => Encoding.UTF8.GetBytes(NativePageSpeed.AsyncCssLoaderJs()));

    private readonly RequestDelegate _next;
    private readonly IOptionsMonitor<PageSpeedOptions> _options;
    private readonly IHtmlProcessor _htmlProcessor;
    private readonly IPageSpeedCache _cache;
    private readonly WorkerNotificationService _notifier;
    private readonly ILogger<PageSpeedMiddleware> _logger;

    /// <summary>
    /// Creates a new PageSpeed middleware instance.
    /// </summary>
    public PageSpeedMiddleware(
        RequestDelegate next,
        IOptionsMonitor<PageSpeedOptions> options,
        IHtmlProcessor htmlProcessor,
        IPageSpeedCache cache,
        WorkerNotificationService notifier,
        ILogger<PageSpeedMiddleware> logger)
    {
        _next = next;
        _options = options;
        _htmlProcessor = htmlProcessor;
        _cache = cache;
        _notifier = notifier;
        _logger = logger;

        var opts = options.CurrentValue;
        _logger.LogInformation(
            "PageSpeed cache_mode={Mode} css_max_age={Css}s image_max_age={Image}s",
            opts.CacheMode, opts.CssMaxAgeSeconds, opts.ImageMaxAgeSeconds);
    }

    /// <summary>
    /// Invokes the middleware for the given HTTP context.
    /// </summary>
    public async Task InvokeAsync(HttpContext context)
    {
        var opts = _options.CurrentValue;

        // Serve the CSP-safe async-CSS loader JS at its reserved same-origin
        // path (same single source of truth the nginx module serves). Static;
        // only pages whose stylesheets the transform deferred reference it.
        // Checked before the exclude/method gates so the loader is always
        // reachable when the middleware is enabled.
        if (opts.Enabled &&
            (HttpMethods.IsGet(context.Request.Method) ||
             HttpMethods.IsHead(context.Request.Method)) &&
            context.Request.Path.HasValue &&
            string.Equals(context.Request.Path.Value, AsyncCssLoaderPath,
                          StringComparison.Ordinal))
        {
            await ServeAsyncCssLoaderAsync(context);
            return;
        }

        if (!opts.Enabled || IsExcluded(context.Request, opts))
        {
            await _next(context);
            return;
        }

        // Bypass WebSocket upgrades.
        if (context.WebSockets.IsWebSocketRequest)
        {
            await _next(context);
            return;
        }

        // Only cache GET and HEAD requests. POST/PUT/DELETE responses
        // must not poison the cache for subsequent GET requests.
        if (!HttpMethods.IsGet(context.Request.Method) &&
            !HttpMethods.IsHead(context.Request.Method))
        {
            await _next(context);
            return;
        }

        // Build URL and hostname for cache lookups.
        // URL must be path+query only (no scheme, no host) to match the
        // format used by the nginx/Apache modules and expected by the worker.
        var url = string.Concat(
            context.Request.PathBase.Value,
            context.Request.Path.Value,
            context.Request.QueryString.Value);
        var hostname = context.Request.Host.Host;

        // Scheme for cache key partitioning ("http" or "https").
        var scheme = context.Request.Scheme;
        if (scheme != "http" && scheme != "https")
            scheme = "https";

        // Classify request into capability mask.
        uint mask = DefaultMask;
        try { mask = RequestClassifier.Classify(context.Request); }
        catch (Exception ex)
        {
            _logger.LogDebug(ex,
                "Request classification failed, using default mask");
        }

        // ── CACHE HIT fast path ──────────────────────────────────
        // Skip cache on force-refresh (Ctrl+F5: Cache-Control: no-cache
        // or Pragma: no-cache).
        if (!IsForceRefresh(context.Request))
        {
            try
            {
                // ── Agent-markdown negotiation (OFF by default) ──
                // Mirror the nginx serve model: when the operator flag is on and
                // the request explicitly wants markdown,
                // read the worker-written entitlement and route the cache read
                // through the SINGLE audited native gate (ReadBestAgent). With
                // entitled=0 the gate behaves exactly like ReadBest (0x7C is never
                // eligible), so non-markdown / non-entitled requests are
                // unaffected. The native gate also enforces the content-hash
                // binding — a stale/unbound markdown is refused (null → HTML).
                int agentEntitled = 0;
                if (opts.EnableAgentOptimize
                    && RequestClassifier.WantsAgentMarkdown(context.Request))
                {
                    try
                    {
                        agentEntitled =
                            _cache.ReadSharedConfigAgentEntitled() == 1 ? 1 : 0;
                    }
                    catch (Exception ex)
                    {
                        _logger.LogDebug(ex,
                            "Agent entitlement read failed for {Url}; serving HTML",
                            url);
                    }
                }

                using var cached = agentEntitled == 1
                    ? _cache.ReadBestAgent(url, hostname, scheme, mask, agentEntitled)
                    : _cache.ReadBest(url, hostname, scheme, mask);
                if (cached != null)
                {
                    // Did the gate hand us the markdown variant? (Only an entitled
                    // markdown request can reach here, and the binding gate already
                    // ran — so a 0x7C variant is fresh and bound.)
                    bool servedAgentMarkdown =
                        agentEntitled == 1 &&
                        (cached.Mask & 0xFF) == SentinelId.AgentMarkdown;

                    if (ServeCacheHit(context, cached, url, hostname, scheme, mask,
                                      opts, servedAgentMarkdown))
                    {
                        await WriteBorrowedContentAsync(context, cached);
                    }
                    return;
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex,
                    "Cache read failed for {Url}, falling through to origin",
                    url);
            }
        }

        // ── Early Hints for MISS path ──────────────────────────────
        // If cached early hints exist from a prior request, add Link
        // headers now so they ship with the response start. This lets
        // browsers begin fetching critical sub-resources during origin
        // processing.
        AddEarlyHintLinkHeaders(context, url, hostname, scheme);

        // ── CACHE MISS — proxy to origin ─────────────────────────
        var originalBody = context.Response.Body;
        using var buffer = MsPool.GetStream("PageSpeed.Buffer");
        context.Response.Body = buffer;

        try
        {
            await _next(context);
        }
        catch
        {
            context.Response.Body = originalBody;
            throw;
        }

        // Clear Content-Length -- we may change the body size.
        context.Response.ContentLength = null;
        context.Response.Body = originalBody;

        // Skip non-2xx or empty responses.
        if (context.Response.StatusCode < 200 ||
            context.Response.StatusCode >= 300 ||
            buffer.Length == 0)
        {
            buffer.Seek(0, SeekOrigin.Begin);
            await buffer.CopyToAsync(originalBody, context.RequestAborted);
            return;
        }

        // [Critical #1] Skip caching for uncacheable responses:
        // no-store, private, or Set-Cookie.
        var cacheability = ClassifyCacheability(context.Response);
        if (cacheability == ResponseCacheability.Uncacheable)
        {
            buffer.Seek(0, SeekOrigin.Begin);
            await buffer.CopyToAsync(originalBody, context.RequestAborted);
            return;
        }

        // Classify response content type (managed, no P/Invoke needed).
        var responseContentType = context.Response.ContentType;
        var psContentType = ClassifyResponseContentType(responseContentType);
        var cacheControl = context.Response.Headers.CacheControl.ToString();

        switch (psContentType)
        {
            case PageSpeedContentType.Html:
                await HandleHtmlMiss(
                    context, originalBody, buffer, opts,
                    url, hostname, scheme, mask, cacheability,
                    cacheControl);
                break;

            case PageSpeedContentType.Image:
            case PageSpeedContentType.Css:
            case PageSpeedContentType.Js:
                await HandleAssetMiss(
                    context, originalBody, buffer, opts,
                    url, hostname, scheme, mask,
                    psContentType, responseContentType,
                    cacheability, cacheControl);
                break;

            default:
                // Not a cacheable type — pass through.
                buffer.Seek(0, SeekOrigin.Begin);
                await buffer.CopyToAsync(
                    originalBody, context.RequestAborted);
                break;
        }
    }

    /// <summary>
    /// Serves a cache HIT for any content type. Sets appropriate headers
    /// including Content-Type (with image format negotiation), encoding,
    /// Vary, Cache-Control, ETag, Last-Modified, and X-PageSpeed.
    /// Returns true if the body should be written (200), false for 304.
    /// </summary>
    /// <summary>
    /// Should a worker notification for this request carry the
    /// agent_request bit (asking the worker to render markdown)? The same gate
    /// as the serve negotiation — OFF by default, the request explicitly wants
    /// markdown, AND the worker-written entitlement is set.
    /// Short-circuits before the native call for non-agent traffic; fail-closed.
    /// </summary>
    private bool ShouldRequestAgentRender(HttpContext context, PageSpeedOptions opts)
    {
        if (!opts.EnableAgentOptimize) return false;
        try
        {
            return RequestClassifier.WantsAgentMarkdown(context.Request)
                && _cache.ReadSharedConfigAgentEntitled() == 1;
        }
        catch
        {
            return false;
        }
    }

    // Lease renewal for the zero-copy HIT write.
    // cached.ContentMemory borrows Cyclone mmap bytes; the async transfer to
    // a slow client can hold that borrow far past the read lease's 3.75s
    // guaranteed floor (3/4 of the default 5s lease). Chunking the write
    // gives us a renewal point whenever transport backpressure returns
    // control between chunks: we re-stamp the lease at a <= 3s cadence.
    // Residuals (documented, accepted): a client that stalls the transport
    // for minutes *within* a single chunk await gives no renewal point, and
    // renewal itself stops protecting past Cyclone's 60s anti-starvation
    // wrap ceiling — either way a multi-minute transfer is unprotected
    // again past 60s (pre-lease exposure, now bounded and observable via
    // the cache's wraps_forced_past_lease counter).
    private const int LeaseWriteChunkBytes = 128 * 1024;
    private static readonly TimeSpan LeaseRenewInterval =
        TimeSpan.FromSeconds(3);

    private static async Task WriteBorrowedContentAsync(
        HttpContext context, IReadResult cached)
    {
        var content = cached.ContentMemory;
        if (content.Length <= LeaseWriteChunkBytes)
        {
            // Typical sizes: single write, completes well inside the
            // initial lease stamped by the read itself.
            await context.Response.Body.WriteAsync(
                content, context.RequestAborted);
            return;
        }

        var sinceRenew = System.Diagnostics.Stopwatch.StartNew();
        for (int offset = 0; offset < content.Length;
             offset += LeaseWriteChunkBytes)
        {
            if (sinceRenew.Elapsed >= LeaseRenewInterval)
            {
                cached.RenewLease();
                sinceRenew.Restart();
            }
            int len = Math.Min(LeaseWriteChunkBytes, content.Length - offset);
            await context.Response.Body.WriteAsync(
                content.Slice(offset, len), context.RequestAborted);
        }
    }

    private bool ServeCacheHit(
        HttpContext context, IReadResult cached,
        string url, string hostname, string scheme, uint mask,
        PageSpeedOptions opts, bool servedAgentMarkdown = false)
    {
        var cachedMask = cached.Mask;
        var cachedType = cached.ContentType;
        var content = cached.ContentMemory;

        // Determine Content-Type. The agent markdown variant is served
        // as text/markdown at the same URL. Otherwise, for images the worker may
        // have transcoded to a different format (WebP, AVIF, SVG).
        string? ct;
        if (servedAgentMarkdown)
        {
            ct = "text/markdown; charset=utf-8";
        }
        else
        {
            ct = cachedType == PageSpeedContentType.Image
                ? GetImageContentType(cachedMask, cached.OriginContentType)
                : cached.OriginContentType;
            ct ??= cachedType switch
            {
                PageSpeedContentType.Html => "text/html; charset=utf-8",
                PageSpeedContentType.Css => "text/css; charset=utf-8",
                PageSpeedContentType.Js => "application/javascript; charset=utf-8",
                _ => "application/octet-stream",
            };
        }

        context.Response.StatusCode = 200;
        context.Response.ContentType = ct;
        context.Response.ContentLength = content.Length;
        context.Response.Headers["X-PageSpeed"] = "HIT";

        // Serve-time bandwidth savings: mirror the nginx front-end,
        // which records right after it adds the HIT header. Gate is
        // byte-for-byte the nginx gate — worker-processed AND a non-zero origin
        // size (front-end-written originals carry origin_content_length == 0).
        // Recorded here (not on the 304 paths below) because a subsequent 304
        // still represents a validated cache hit, exactly as nginx counts when
        // it adds the HIT header.
        // Markdown HITs are excluded from bandwidth-saved accounting —
        // that story is about optimized assets, not the rendered-DOM variant.
        if (!servedAgentMarkdown &&
            cached.IsWorkerProcessed && cached.OriginContentLength > 0)
            // Pass the served variant's mask so SVG image variants (format bits
            // == SVG) also tick svg.served, mirroring nginx.
            _cache.RecordServeHit(cachedType, cached.OriginContentLength, (ulong)content.Length, cachedMask);

        // Cache-Control on HIT responses.
        // Note: origin private/no-store responses never reach here —
        // they are filtered by ClassifyCacheability() upstream.
        if (servedAgentMarkdown)
        {
            // The markdown variant is anonymous-view-only and must
            // never be stored by a shared cache.
            context.Response.Headers.CacheControl = "private, no-cache";
        }
        else if (cachedType == PageSpeedContentType.Html)
        {
            // HTML always gets no-cache in both modes.
            context.Response.Headers.CacheControl = "no-cache";
        }
        else
        {
            var maxAge = GetMaxAgeForType(cachedType, opts);
            context.Response.Headers.CacheControl =
                BuildAssetCacheControl(
                    opts.CacheMode, maxAge,
                    cached.OriginCcFlags);
        }

        // Age and Last-Modified headers from cache insertion time.
        var insertedAt = cached.CacheInsertedAt;
        if (insertedAt > 0)
        {
            var age = DateTimeOffset.UtcNow.ToUnixTimeSeconds() - insertedAt;
            if (age > 0)
                context.Response.Headers["Age"] = age.ToString();

            var lastModified = DateTimeOffset.FromUnixTimeSeconds(insertedAt);
            context.Response.Headers["Last-Modified"] =
                lastModified.ToString("R");
        }

        // Content-Encoding for compressed variants.
        var encoding = cachedMask & EncodingMask;
        if (encoding == EncodingGzip)
            context.Response.Headers.ContentEncoding = "gzip";
        else if (encoding == EncodingBrotli)
            context.Response.Headers.ContentEncoding = "br";

        // Vary headers per content type.
        SetVaryHeaders(context, cachedType);

        if (servedAgentMarkdown)
        {
            // Accept MUST participate in Vary so an intermediary can
            // never serve the markdown body to a non-markdown request, and the
            // rendered-DOM markdown must stay out of search indexes. Set
            // deterministically (the 0x7C variant is typed kOther, so
            // SetVaryHeaders leaves Vary unset for it).
            context.Response.Headers.Vary = "Accept, Accept-Encoding";
            context.Response.Headers["X-Robots-Tag"] = "noindex";
        }

        // Link preload/preconnect headers (HTML only; markdown has no browser
        // sub-resources).
        if (cachedType == PageSpeedContentType.Html && !servedAgentMarkdown)
            AddLinkHeadersFromCache(context, url, hostname, scheme);

        // Re-notify worker if cached entry needs revalidation.
        if (cached.NeedsRevalidation)
            if (!_notifier.TryNotify(url, hostname, scheme, cachedType, mask,
                                     ShouldRequestAgentRender(context, opts)))
                _logger.LogDebug("TryNotify failed for revalidation: {Url}", url);

        // ETag based on cache metadata.
        var contentLength = content.Length;
        var etag = $"W/\"{cached.CacheInsertedAt:x}-{contentLength:x}\"";
        context.Response.Headers.ETag = etag;

        // ── Conditional request: If-None-Match ──────────────────
        var ifNoneMatch = context.Request.Headers.IfNoneMatch.ToString();
        if (!string.IsNullOrEmpty(ifNoneMatch))
        {
            if (MatchesETag(ifNoneMatch, etag))
            {
                context.Response.StatusCode = StatusCodes.Status304NotModified;
                context.Response.ContentLength = null;
                return false;
            }
        }

        // ── Conditional request: If-Modified-Since ──────────────
        var ifModifiedSince = context.Request.Headers.IfModifiedSince.ToString();
        if (!string.IsNullOrEmpty(ifModifiedSince) &&
            cached.CacheInsertedAt > 0)
        {
            if (DateTimeOffset.TryParse(ifModifiedSince, out var since))
            {
                var insertedAtDto = DateTimeOffset.FromUnixTimeSeconds(
                    cached.CacheInsertedAt);
                if (insertedAtDto <= since)
                {
                    context.Response.StatusCode = StatusCodes.Status304NotModified;
                    context.Response.ContentLength = null;
                    return false;
                }
            }
        }

        return true;
    }

    /// <summary>
    /// Returns the max-age value (seconds) for the given content type
    /// based on per-type options. CSS and JS share the same setting.
    /// </summary>
    private static int GetMaxAgeForType(
        PageSpeedContentType type, PageSpeedOptions opts)
        => type switch
        {
            PageSpeedContentType.Css or PageSpeedContentType.Js => opts.CssMaxAgeSeconds,
            PageSpeedContentType.Image => opts.ImageMaxAgeSeconds,
            _ => opts.CssMaxAgeSeconds,
        };

    /// <summary>
    /// Builds the Cache-Control header for a non-HTML cache HIT response.
    /// Matches the nginx-side BuildCacheControlHeader contract (lib/cache/
    /// cache_control_header.cc, post-PR #252): in aggressive mode,
    /// stale-if-error is only emitted when the origin permits stale
    /// serving. When the origin sent must-revalidate / no-cache /
    /// proxy-revalidate / s-maxage, RFC 9111 §4.2.4 forbids stale
    /// serving and §5.2.2.11 makes stale-if-error meaningless — so we
    /// suppress it and propagate must-revalidate instead.
    /// </summary>
    internal static string BuildAssetCacheControl(
        CacheMode mode, int maxAge, ushort originCcFlags)
    {
        if (mode == CacheMode.Safe)
            return $"max-age={maxAge}, must-revalidate";

        // Aggressive mode. For a private cache (ASP.NET in-process),
        // proxy-revalidate and s-maxage are ignored per RFC 9111 §5.2.2.9-10.
        bool revalidationRequired =
            (originCcFlags & (CacheControlCcFlags.NoCache | CacheControlCcFlags.MustRevalidate)) != 0;

        if (revalidationRequired)
            return $"public, max-age={maxAge}, must-revalidate";

        return $"public, max-age={maxAge}, stale-if-error=86400";
    }

    /// <summary>
    /// Returns true when the origin Cache-Control header contains a
    /// directive that forbids serving stale responses per RFC 9111
    /// §4.2.4: must-revalidate, no-cache, proxy-revalidate, or
    /// s-maxage. Mirrors the nginx-side `revalidation_required` flag
    /// <summary>
    /// For images, derives Content-Type from format bits in the
    /// capability mask. The worker writes transcoded variants with
    /// format bits set to WebP (01), AVIF (10), or SVG (11).
    /// </summary>
    private static string GetImageContentType(
        uint mask, string? originContentType)
    {
        return (mask & FormatMask) switch
        {
            FormatWebP => "image/webp",
            FormatAvif => "image/avif",
            FormatSvg => "image/svg+xml",
            _ => originContentType ?? "image/jpeg",
        };
    }

    /// <summary>
    /// Sets Vary headers appropriate for the content type.
    /// </summary>
    private static void SetVaryHeaders(
        HttpContext context, PageSpeedContentType type)
    {
        switch (type)
        {
            case PageSpeedContentType.Image:
                context.Response.Headers.Vary = "Accept, Save-Data, User-Agent";
                break;
            case PageSpeedContentType.Html:
                context.Response.Headers.Vary = "Accept-Encoding, User-Agent";
                break;
            case PageSpeedContentType.Css:
            case PageSpeedContentType.Js:
                context.Response.Headers.Vary = "Accept-Encoding";
                break;
        }
    }

    /// <summary>
    /// Handles an HTML cache MISS: transforms the HTML, caches it,
    /// writes early hints, and notifies the worker.
    /// </summary>
    private async Task HandleHtmlMiss(
        HttpContext context, Stream originalBody,
        RecyclableMemoryStream buffer, PageSpeedOptions opts,
        string url, string hostname, string scheme, uint mask,
        ResponseCacheability cacheability,
        string? cacheControl)
    {
        // Size gate for HTML processing.
        if (buffer.Length > opts.MaxResponseBufferBytes ||
            !RequestClassifier.IsHtmlResponse(context.Response))
        {
            buffer.Seek(0, SeekOrigin.Begin);
            await buffer.CopyToAsync(originalBody, context.RequestAborted);
            return;
        }

        try
        {
            var underlying = buffer.GetBuffer();
            var htmlSpan = new ReadOnlySpan<byte>(
                underlying, 0, (int)buffer.Length);

            context.RequestAborted.ThrowIfCancellationRequested();
            using var result = _htmlProcessor.Process(
                htmlSpan, url, hostname);
            context.RequestAborted.ThrowIfCancellationRequested();

            if (result.Modified)
            {
                AddPreloadHeaders(context, result);
                context.Response.Headers["X-PageSpeed"] = "MISS";
                SetVaryHeaders(context, PageSpeedContentType.Html);

                var output = result.OutputMemory;
                context.Response.ContentLength = output.Length;
                await originalBody.WriteAsync(
                    output, context.RequestAborted);

                TryWriteToCache(
                    url, hostname, scheme, output,
                    PageSpeedContentType.Html,
                    context.Response.ContentType,
                    result.NeedsRevalidation,
                    cacheControl);

                TryWriteEarlyHints(
                    url, hostname, scheme, result.EarlyHintsMemory);

                // [Critical #2] Skip worker notification if no-transform.
                if (cacheability != ResponseCacheability.NoTransform)
                {
                    if (!_notifier.TryNotify(
                        url, hostname, scheme, PageSpeedContentType.Html, mask,
                        ShouldRequestAgentRender(context, opts)))
                        _logger.LogDebug("TryNotify failed for HTML: {Url}", url);
                }
            }
            else
            {
                buffer.Seek(0, SeekOrigin.Begin);
                context.Response.ContentLength = buffer.Length;
                await buffer.CopyToAsync(
                    originalBody, context.RequestAborted);
            }
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex,
                "PageSpeed HTML processing failed, passing through original");

            if (!context.Response.HasStarted)
            {
                buffer.Seek(0, SeekOrigin.Begin);
                context.Response.ContentLength = buffer.Length;
                await buffer.CopyToAsync(
                    originalBody, context.RequestAborted);
            }
            else
            {
                _logger.LogError(ex,
                    "Response already started, cannot fall back");
            }
        }
    }

    /// <summary>
    /// Handles a cache MISS for images, CSS, and JS: caches the
    /// original response and notifies the worker for optimization
    /// (image transcoding, CSS/JS minification, compression).
    /// </summary>
    private async Task HandleAssetMiss(
        HttpContext context, Stream originalBody,
        RecyclableMemoryStream buffer, PageSpeedOptions opts,
        string url, string hostname, string scheme, uint mask,
        PageSpeedContentType psContentType,
        string? responseContentType,
        ResponseCacheability cacheability,
        string? cacheControl)
    {
        // Pass through the original response to the client.
        buffer.Seek(0, SeekOrigin.Begin);
        context.Response.ContentLength = buffer.Length;
        context.Response.Headers["X-PageSpeed"] = "MISS";
        SetVaryHeaders(context, psContentType);
        await buffer.CopyToAsync(originalBody, context.RequestAborted);

        // [High #4] Size gate: skip caching for oversized assets.
        if (buffer.Length > opts.MaxAssetCacheBytes)
        {
            _logger.LogDebug(
                "Asset {Url} exceeds MaxAssetCacheBytes ({Size} > {Max}), skipping cache",
                url, buffer.Length, opts.MaxAssetCacheBytes);
            return;
        }

        // Cache the original (identity) content.
        var underlying = buffer.GetBuffer();
        var data = new ReadOnlyMemory<byte>(
            underlying, 0, (int)buffer.Length);

        TryWriteToCache(
            url, hostname, scheme, data,
            psContentType, responseContentType,
            needsRevalidation: false,
            cacheControl: context.Response.Headers.CacheControl.ToString());

        // [Critical #2] Skip worker notification if no-transform.
        if (cacheability != ResponseCacheability.NoTransform)
        {
            if (!_notifier.TryNotify(url, hostname, scheme, psContentType, mask))
                _logger.LogDebug("TryNotify failed for {Type}: {Url}", psContentType, url);
        }
    }

    private void TryWriteToCache(
        string url, string hostname, string scheme,
        ReadOnlyMemory<byte> output,
        PageSpeedContentType contentType,
        string? responseContentType,
        bool needsRevalidation,
        string? cacheControl = null)
    {
        try
        {
            byte flags = 0;
            if (needsRevalidation) flags |= CacheFlags.NeedsRevalidation;

            // Parse origin Cache-Control to derive origin_cc_flags.
            ushort originCcFlags = 0;
            if (!string.IsNullOrEmpty(cacheControl))
            {
                var cc = new Native.NativeCacheControl
                {
                    StructSize = (nuint)Marshal.SizeOf<Native.NativeCacheControl>()
                };

                // cacheControl is the response's joined Cache-Control value
                // (StringValues.ToString() -- multiple header lines join with
                // ", ", which is exactly the RFC 9111 single-list form the
                // parser walks, quoting included), so ONE call parses the list.
                int err = Native.NativePageSpeed.ParseCacheControl(cacheControl, ref cc);
                if (err != 0) // PS_OK
                {
                    _logger.LogDebug(
                        "ParseCacheControl failed for '{Value}': {Err}",
                        cacheControl, err);
                }
                originCcFlags = cc.CcFlags;
            }

            var writeParams = new CacheWriteParams
            {
                AlternateId = (byte)DefaultMask,
                ContentLength = (ulong)output.Length,
                FullMask = DefaultMask,
                ContentType = contentType,
                Flags = flags,
                OriginContentType = responseContentType,
                OriginCcFlags = originCcFlags,
            };
            using var writer = _cache.BeginWrite(
                url, hostname, scheme, writeParams);
            writer.Write(output.Span);
            writer.Commit();
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Cache write failed for {Url}", url);
        }
    }

    private void TryWriteEarlyHints(
        string url, string hostname, string scheme,
        ReadOnlyMemory<byte> earlyHints)
    {
        // An empty hint list must still overwrite the sentinel:
        // reprocessing can legitimately drop every hint (e.g. async-CSS
        // newly defers the only stylesheet once it caches), and a
        // surviving sentinel from an earlier pass would keep re-promoting
        // the demoted download forever. Readers treat empty content as
        // "no hints"; the managed cache API has no per-alternate remove.
        try
        {
            using var writer = _cache.BeginWriteSentinel(
                url, hostname, scheme, SentinelId.EarlyHints,
                (ulong)earlyHints.Length);
            writer.Write(earlyHints.Span);
            writer.Commit();
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Early hints cache write failed for {Url}", url);
        }
    }

    private void AddLinkHeadersFromCache(
        HttpContext context, string url, string hostname, string scheme)
    {
        try
        {
            using var hints = _cache.ReadEarlyHints(url, hostname, scheme);
            if (hints == null) return;
            AddLinkHeadersFromHints(context, hints.ContentMemory);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Early hints read failed for {Url}", url);
        }
    }

    /// <summary>
    /// Adds Link headers from cached early hints on the MISS path.
    /// These hints were generated during a prior HTML processing and
    /// provide preload/preconnect signals to the browser before the
    /// current response is fully generated.
    /// </summary>
    private void AddEarlyHintLinkHeaders(
        HttpContext context, string url, string hostname, string scheme)
    {
        try
        {
            using var hints = _cache.ReadEarlyHints(url, hostname, scheme);
            if (hints == null) return;
            AddLinkHeadersFromHints(context, hints.ContentMemory);
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex,
                "Early hints pre-send failed for {Url}", url);
        }
    }

    /// <summary>
    /// Classifies a response Content-Type header into a PageSpeedContentType.
    /// Uses managed string matching (no P/Invoke) so tests work without
    /// the native library.
    /// </summary>
    internal static PageSpeedContentType ClassifyResponseContentType(
        string? contentType)
    {
        if (contentType == null) return PageSpeedContentType.Other;
        if (contentType.StartsWith("text/html", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Html;
        if (contentType.StartsWith("text/css", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Css;
        if (contentType.StartsWith("text/javascript", StringComparison.OrdinalIgnoreCase) ||
            contentType.StartsWith("application/javascript", StringComparison.OrdinalIgnoreCase) ||
            contentType.StartsWith("application/x-javascript", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Js;
        if (contentType.StartsWith("image/", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Image;
        return PageSpeedContentType.Other;
    }

    /// <summary>
    /// Classifies the response cacheability based on Cache-Control directives
    /// and Set-Cookie presence. Returns Uncacheable for no-store/private/Set-Cookie,
    /// NoTransform if the origin forbids transformation, Cacheable otherwise.
    /// </summary>
    internal static ResponseCacheability ClassifyCacheability(
        HttpResponse response)
    {
        // Set-Cookie means the response is personalized — never cache.
        if (response.Headers.SetCookie.Count > 0)
            return ResponseCacheability.Uncacheable;

        var cc = response.Headers.CacheControl.ToString();
        if (string.IsNullOrEmpty(cc))
            return ResponseCacheability.Cacheable;

        // Parse Cache-Control directives (case-insensitive).
        if (ContainsDirective(cc, "no-store") ||
            ContainsDirective(cc, "private"))
            return ResponseCacheability.Uncacheable;

        if (ContainsDirective(cc, "no-transform"))
            return ResponseCacheability.NoTransform;

        return ResponseCacheability.Cacheable;
    }

    /// <summary>
    /// Checks if a Cache-Control header value contains the given directive.
    /// Handles comma-separated directives with optional whitespace.
    /// </summary>
    internal static bool ContainsDirective(string cacheControl, string directive)
    {
        var span = cacheControl.AsSpan();
        while (span.Length > 0)
        {
            int comma = span.IndexOf(',');
            var token = comma >= 0 ? span[..comma] : span;
            token = token.Trim();

            // Directive may have a value (e.g. "max-age=3600").
            int eq = token.IndexOf('=');
            var name = eq >= 0 ? token[..eq].Trim() : token;

            if (name.Equals(directive, StringComparison.OrdinalIgnoreCase))
                return true;

            span = comma >= 0 ? span[(comma + 1)..] : [];
        }
        return false;
    }

    /// <summary>
    /// Checks if an If-None-Match header value matches the given ETag.
    /// Handles comma-separated ETag lists per RFC 7232 §3.2.
    /// Supports wildcard "*" matching.
    /// </summary>
    internal static bool MatchesETag(string ifNoneMatch, string etag)
    {
        var span = ifNoneMatch.AsSpan().Trim();
        if (span.SequenceEqual("*"))
            return true;

        while (span.Length > 0)
        {
            int comma = span.IndexOf(',');
            var token = comma >= 0 ? span[..comma] : span;
            token = token.Trim();

            // Weak comparison (RFC 7232 §2.3.2): both W/ and strong
            // ETags match for conditional GET. Compare the full token.
            if (token.Equals(etag, StringComparison.Ordinal))
                return true;

            span = comma >= 0 ? span[(comma + 1)..] : [];
        }
        return false;
    }

    /// <summary>
    /// Detects browser force-refresh (Ctrl+F5). Browsers send
    /// Cache-Control: no-cache and/or Pragma: no-cache to indicate
    /// the user wants a fresh response from origin.
    /// </summary>
    internal static bool IsForceRefresh(HttpRequest request)
    {
        var cc = request.Headers.CacheControl.ToString();
        if (!string.IsNullOrEmpty(cc) &&
            ContainsDirective(cc, "no-cache"))
            return true;

        var pragma = request.Headers.Pragma.ToString();
        return !string.IsNullOrEmpty(pragma) &&
               ContainsDirective(pragma, "no-cache");
    }

    /// <summary>
    /// Checks if the request path matches any exclusion pattern.
    /// Patterns containing * or ? are matched as globs; others as prefixes
    /// for backward compatibility.
    /// </summary>
    // Serve the CSP-safe async-CSS loader JS (immutable, long-cache). GET ships
    // the body; HEAD ships headers only.
    private static async Task ServeAsyncCssLoaderAsync(HttpContext context)
    {
        var bytes = AsyncCssLoaderJsBytes.Value;
        var response = context.Response;
        response.StatusCode = StatusCodes.Status200OK;
        response.ContentType = "text/javascript; charset=utf-8";
        // The path is CONTENT-ADDRESSED (AsyncCssLoaderPath embeds a hash of the
        // loader body), so a future loader change yields a NEW path and can never
        // be masked by a stale cache — immutable+1yr is correct here, in lockstep
        // with the nginx module's Cache-Control for the same path.
        response.Headers.CacheControl = "public, max-age=31536000, immutable";
        response.Headers["X-Content-Type-Options"] = "nosniff";
        // Provenance marker so a deploy smoke can assert the loader was served
        // by the middleware (not a 200 from some other handler), matching nginx.
        response.Headers["X-PageSpeed"] = "async-css-loader";
        response.ContentLength = bytes.Length;
        if (!HttpMethods.IsHead(context.Request.Method))
        {
            await response.Body.WriteAsync(bytes);
        }
    }

    private static bool IsExcluded(
        HttpRequest request, PageSpeedOptions opts)
    {
        var path = request.Path.Value;
        if (path == null) return false;

        foreach (var pattern in opts.ExcludePaths)
        {
            if (pattern.AsSpan().IndexOfAny('*', '?') >= 0)
            {
                if (GlobMatch(path, pattern))
                    return true;
            }
            else
            {
                if (path.StartsWith(pattern,
                    StringComparison.OrdinalIgnoreCase))
                    return true;
            }
        }
        return false;
    }

    /// <summary>
    /// Simple glob matcher supporting * (any sequence) and ? (single char).
    /// Case-insensitive. Does not support character classes or braces.
    /// </summary>
    internal static bool GlobMatch(
        ReadOnlySpan<char> input, ReadOnlySpan<char> pattern)
    {
        int ip = 0, pp = 0;
        int starIp = -1, starPp = -1;

        while (ip < input.Length)
        {
            if (pp < pattern.Length &&
                (char.ToLowerInvariant(pattern[pp]) ==
                     char.ToLowerInvariant(input[ip]) ||
                 pattern[pp] == '?'))
            {
                ip++;
                pp++;
            }
            else if (pp < pattern.Length && pattern[pp] == '*')
            {
                starPp = pp++;
                starIp = ip;
            }
            else if (starPp >= 0)
            {
                pp = starPp + 1;
                ip = ++starIp;
            }
            else
            {
                return false;
            }
        }

        while (pp < pattern.Length && pattern[pp] == '*')
            pp++;

        return pp == pattern.Length;
    }

    /// <summary>
    /// Adds Link preload/preconnect headers from an HtmlResult's early hints.
    /// </summary>
    private static void AddPreloadHeaders(
        HttpContext context, HtmlResult result)
    {
        var hints = result.EarlyHintsMemory;
        if (hints.IsEmpty) return;
        AddLinkHeadersFromHints(context, hints);
    }

    /// <summary>
    /// Shared hint-line parser: adds Link headers from newline-separated
    /// early hints data (used by both HIT and MISS paths).
    /// </summary>
    internal static void AddLinkHeadersFromHints(
        HttpContext context, ReadOnlyMemory<byte> hints)
    {
        var hintsSpan = hints.Span;
        int start = 0;
        for (int i = 0; i <= hintsSpan.Length; i++)
        {
            if (i == hintsSpan.Length || hintsSpan[i] == (byte)'\n')
            {
                if (i > start)
                {
                    var line = hintsSpan[start..i];
                    var value = BuildLinkHeader(line);
                    if (value != null)
                        context.Response.Headers.Append("Link", value);
                }
                start = i + 1;
            }
        }
    }

    private static string? BuildLinkHeader(ReadOnlySpan<byte> line)
    {
        ReadOnlySpan<byte> preconnectCors = "preconnect-cors:"u8;
        ReadOnlySpan<byte> preconnect = "preconnect:"u8;
        ReadOnlySpan<byte> image = "image:"u8;

        string url;
        string rel;

        // CORS-mode origin: crossorigin must ride along so the warmed
        // connection pool matches the request mode of the motivating
        // resource (fonts, crossorigin scripts, ES modules).
        if (line.StartsWith(preconnectCors))
        {
            url = Encoding.UTF8.GetString(line[preconnectCors.Length..]);
            rel = "preconnect; crossorigin";
        }
        else if (line.StartsWith(preconnect))
        {
            url = Encoding.UTF8.GetString(line[preconnect.Length..]);
            rel = "preconnect";
        }
        else if (line.StartsWith(image))
        {
            url = Encoding.UTF8.GetString(line[image.Length..]);
            rel = "preload; as=image";
        }
        else
        {
            url = Encoding.UTF8.GetString(line);
            rel = "preload; as=style";
        }

        if (url.AsSpan().IndexOfAny(UnsafeLinkChars) >= 0)
            return null;

        return $"<{url}>; rel={rel}";
    }
}

/// <summary>
/// Response cacheability classification based on Cache-Control directives.
/// </summary>
public enum ResponseCacheability
{
    /// <summary>Response can be cached and transformed.</summary>
    Cacheable,

    /// <summary>Response must not be cached (no-store, private, Set-Cookie).</summary>
    Uncacheable,

    /// <summary>Response can be cached but must not be transformed (no-transform).</summary>
    NoTransform,
}
