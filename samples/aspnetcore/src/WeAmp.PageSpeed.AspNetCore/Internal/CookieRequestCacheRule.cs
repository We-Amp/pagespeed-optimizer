// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Globalization;
using Microsoft.AspNetCore.Http;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// When a request that carries a cookie may be answered from, or recorded
/// into, the shared cache.
/// </summary>
/// <remarks>
/// <para>The rule is the native HTML cache policy's cookie rule
/// (lib/cache/html_cache_policy.h, store rule 8 and serve rule 4), exported
/// there as <c>HtmlCookieRequestMayShare</c> and
/// <c>HtmlExplicitlySharedFromFlags</c> and applied by the nginx port through
/// src/nginx/cookie_cache_gate.h. The C API does not expose it, so it is
/// restated here; <see cref="MayShare"/> and
/// <see cref="ExplicitlySharedFromFlags"/> mirror those two functions and
/// must give the same answers.</para>
///
/// <para>A page fetched for a request that carried a <c>Cookie</c> (or
/// <c>Cookie2</c>) header is recorded only when the origin marked it
/// explicitly shareable: <c>public</c>, or one <c>s-maxage</c> whose value
/// is delta-seconds greater than zero. <c>must-revalidate</c> is not such a
/// mark. A stored page is served to a cookie-bearing request only when the
/// stored entry carries the mark. Presence of the header is what counts,
/// whatever its value: an empty <c>Cookie</c> header is a cookie.</para>
///
/// <para>Stylesheets, scripts and images are outside that rule. A
/// stylesheet, script or image fetched for a cookie-bearing request is
/// recorded only when the origin gave it an explicit freshness lifetime
/// (<see cref="HasExplicitLifetime"/>); the middleware's own default
/// lifetime does not stand in for one.</para>
///
/// <para>Independent of the anonymous-request rule
/// (<see cref="PageSpeedMiddleware.IsAnonymousRequest"/>): a request must
/// pass both to be recorded.</para>
/// </remarks>
internal static class CookieRequestCacheRule
{
    /// <summary>Does the request carry a <c>Cookie</c> or <c>Cookie2</c> header, whatever its value?</summary>
    public static bool RequestHasCookie(HttpRequest request) =>
        request.Headers.ContainsKey("Cookie") ||
        request.Headers.ContainsKey("Cookie2");

    /// <summary>
    /// Is the stored or fetched content of this type judged by the
    /// explicit-sharing mark? Everything except stylesheets, scripts and
    /// images (which <see cref="HasExplicitLifetime"/> covers on the record
    /// side).
    /// </summary>
    public static bool AppliesTo(PageSpeedContentType type) =>
        type is not (PageSpeedContentType.Css or PageSpeedContentType.Js
                     or PageSpeedContentType.Image);

    /// <summary>Mirrors <c>HtmlCookieRequestMayShare</c>.</summary>
    public static bool MayShare(bool requestHasCookie, bool explicitShared) =>
        !requestHasCookie || explicitShared;

    /// <summary>
    /// Mirrors <c>HtmlExplicitlySharedFromFlags</c>: the mark as the stored
    /// origin Cache-Control flags record it (<c>public</c>, or an
    /// <c>s-maxage</c> directive). The flags record that s-maxage was
    /// present, not its value, so this is one step wider than
    /// <see cref="ExplicitlySharedFromCacheControl"/>; an entry recorded for
    /// a cookie-bearing request passed that stricter check first.
    /// </summary>
    public static bool ExplicitlySharedFromFlags(ushort originCcFlags) =>
        (originCcFlags &
         (CacheControlCcFlags.Public | CacheControlCcFlags.SMaxagePresent)) != 0;

    /// <summary>
    /// The mark as the origin's Cache-Control value states it: a
    /// <c>public</c> directive, or exactly one <c>s-maxage</c> whose value is
    /// unquoted delta-seconds greater than zero.
    /// </summary>
    public static bool ExplicitlySharedFromCacheControl(string? cacheControl)
    {
        if (string.IsNullOrEmpty(cacheControl)) return false;
        var d = Scan(cacheControl);
        if (d.Public) return true;
        return d.SMaxageCount == 1 && d.SMaxage > 0;
    }

    /// <summary>
    /// Does the origin response state its own freshness lifetime for a
    /// shared cache, greater than zero? <c>s-maxage</c> decides when present,
    /// otherwise <c>max-age</c>, otherwise <c>Expires</c> against
    /// <c>Date</c> (or <paramref name="now"/> without one). A directive
    /// given more than once, or with a value that is not delta-seconds, or
    /// an <c>Expires</c> that is not an HTTP-date, states nothing.
    /// </summary>
    public static bool HasExplicitLifetime(
        string? cacheControl, string? expires, string? date, DateTimeOffset now)
    {
        var d = Scan(cacheControl ?? "");
        if (d.SMaxageCount > 1 || d.MaxAgeCount > 1) return false;
        if (d.SMaxageCount == 1) return d.SMaxage > 0;
        if (d.MaxAgeCount == 1) return d.MaxAge > 0;
        if (!TryParseHttpDate(expires, out var expiresAt)) return false;
        var origin = TryParseHttpDate(date, out var dateAt) ? dateAt : now;
        return expiresAt > origin;
    }

    private struct Directives
    {
        public bool Public;
        public int SMaxageCount;
        public long SMaxage;  // -1 when the value is not delta-seconds
        public int MaxAgeCount;
        public long MaxAge;   // -1 when the value is not delta-seconds
    }

    // Comma-split, as the native policy's HasValidSMaxage / CountDirective
    // walk a Cache-Control line.
    private static Directives Scan(string cacheControl)
    {
        var d = new Directives { SMaxage = -1, MaxAge = -1 };
        foreach (var raw in cacheControl.Split(','))
        {
            var token = raw.AsSpan();
            int eq = token.IndexOf('=');
            var name = (eq >= 0 ? token[..eq] : token).Trim(" \t");
            var value = eq >= 0 ? token[(eq + 1)..].Trim(" \t") : ReadOnlySpan<char>.Empty;
            if (name.Equals("public", StringComparison.OrdinalIgnoreCase))
            {
                d.Public = true;
            }
            else if (name.Equals("s-maxage", StringComparison.OrdinalIgnoreCase))
            {
                d.SMaxageCount++;
                d.SMaxage = eq >= 0 ? DeltaSeconds(value) : -1;
            }
            else if (name.Equals("max-age", StringComparison.OrdinalIgnoreCase))
            {
                d.MaxAgeCount++;
                d.MaxAge = eq >= 0 ? DeltaSeconds(value) : -1;
            }
        }
        return d;
    }

    // RFC 9111 §1.2.2 delta-seconds: one or more digits, no quotes, no sign.
    // Large values saturate. -1 for anything else.
    private static long DeltaSeconds(ReadOnlySpan<char> value)
    {
        if (value.IsEmpty) return -1;
        long v = 0;
        foreach (var c in value)
        {
            if (c < '0' || c > '9') return -1;
            v = v >= int.MaxValue ? int.MaxValue : v * 10 + (c - '0');
        }
        return v;
    }

    private static bool TryParseHttpDate(string? value, out DateTimeOffset at) =>
        DateTimeOffset.TryParseExact(
            value, "r", CultureInfo.InvariantCulture,
            DateTimeStyles.AssumeUniversal, out at);
}
