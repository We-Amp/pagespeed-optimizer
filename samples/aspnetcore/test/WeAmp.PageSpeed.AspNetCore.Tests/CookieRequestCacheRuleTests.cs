// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Http;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The rule for what a request carrying a cookie may be served from, or
/// record into, the shared cache. The same answers as the native HTML cache
/// policy's HtmlCookieRequestMayShare / HtmlExplicitlySharedFromFlags.
/// </summary>
public class CookieRequestCacheRuleTests
{
    [Theory]
    [InlineData(null, null, false)]
    [InlineData("Cookie", "session=abc", true)]
    [InlineData("cookie", "session=abc", true)]
    [InlineData("Cookie", "", true)]
    [InlineData("Cookie2", "$Version=1", true)]
    [InlineData("X-Cookie", "session=abc", false)]
    public void RequestHasCookie_CountsTheHeaderWhateverItsValue(
        string? name, string? value, bool expected)
    {
        var ctx = new DefaultHttpContext();
        if (name != null) ctx.Request.Headers[name] = value;
        Assert.Equal(expected, CookieRequestCacheRule.RequestHasCookie(ctx.Request));
    }

    [Theory]
    [InlineData(false, false, true)]
    [InlineData(false, true, true)]
    [InlineData(true, false, false)]
    [InlineData(true, true, true)]
    public void MayShare_NeedsTheMarkOnlyWithACookie(
        bool hasCookie, bool explicitShared, bool expected) =>
        Assert.Equal(expected, CookieRequestCacheRule.MayShare(hasCookie, explicitShared));

    [Theory]
    [InlineData(null, false)]
    [InlineData("", false)]
    [InlineData("max-age=60", false)]
    [InlineData("must-revalidate", false)]
    [InlineData("max-age=60, must-revalidate", false)]
    [InlineData("no-cache", false)]
    [InlineData("public", true)]
    [InlineData("PUBLIC, max-age=60", true)]
    [InlineData("max-age=60 , Public", true)]
    [InlineData("s-maxage=60", true)]
    [InlineData("S-MaxAge = 60", true)]
    [InlineData("s-maxage=0", false)]
    [InlineData("s-maxage=abc", false)]
    [InlineData("s-maxage=-5", false)]
    [InlineData("s-maxage=\"60\"", false)]
    [InlineData("s-maxage=", false)]
    [InlineData("s-maxage", false)]
    [InlineData("s-maxage=60, s-maxage=120", false)]
    [InlineData("s-maxage=0, public", true)]
    public void ExplicitlySharedFromCacheControl_NeedsPublicOrAPositiveSMaxage(
        string? cacheControl, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.ExplicitlySharedFromCacheControl(cacheControl));

    [Theory]
    [InlineData(0, false)]
    [InlineData(CacheControlCcFlags.MustRevalidate, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.NoCache, false)]
    [InlineData(CacheControlCcFlags.Public, true)]
    [InlineData(CacheControlCcFlags.SMaxagePresent, true)]
    public void ExplicitlySharedFromFlags_ReadsPublicAndSMaxage(int flags, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.ExplicitlySharedFromFlags((ushort)flags));

    [Theory]
    [InlineData(PageSpeedContentType.Html, true)]
    [InlineData(PageSpeedContentType.Other, true)]
    [InlineData(PageSpeedContentType.Css, false)]
    [InlineData(PageSpeedContentType.Js, false)]
    [InlineData(PageSpeedContentType.Image, false)]
    public void AppliesTo_EverythingButStylesheetsScriptsAndImages(
        PageSpeedContentType type, bool expected) =>
        Assert.Equal(expected, CookieRequestCacheRule.AppliesTo(type));

    private static readonly DateTimeOffset Now =
        new(2026, 11, 14, 12, 0, 0, TimeSpan.Zero);

    [Theory]
    [InlineData(null, null, null, false)]
    [InlineData("public", null, null, false)]
    [InlineData("max-age=3600", null, null, true)]
    [InlineData("max-age=0", null, null, false)]
    [InlineData("max-age=abc", null, null, false)]
    [InlineData("max-age=60, max-age=120", null, null, false)]
    [InlineData("s-maxage=60", null, null, true)]
    [InlineData("s-maxage=0, max-age=3600", null, null, false)]
    [InlineData("s-maxage=abc, max-age=3600", null, null, false)]
    [InlineData("must-revalidate", null, null, false)]
    [InlineData(null, "Sat, 14 Nov 2026 13:00:00 GMT", null, true)]
    [InlineData(null, "Sat, 14 Nov 2026 11:00:00 GMT", null, false)]
    [InlineData(null, "Sat, 14 Nov 2026 13:00:00 GMT", "Sat, 14 Nov 2026 14:00:00 GMT", false)]
    [InlineData(null, "0", null, false)]
    [InlineData("max-age=0", "Sat, 14 Nov 2026 13:00:00 GMT", null, false)]
    public void HasExplicitLifetime_NeedsTheOriginsOwnPositiveLifetime(
        string? cacheControl, string? expires, string? date, bool expected) =>
        Assert.Equal(expected,
            CookieRequestCacheRule.HasExplicitLifetime(cacheControl, expires, date, Now));
}
