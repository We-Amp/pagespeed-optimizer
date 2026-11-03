// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Collections.Concurrent;
using System.Net;
using System.Net.Http;
using System.Security.Claims;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The middleware stores a response only when the request was anonymous as
/// far as the application can tell: no <c>Authorization</c> or
/// <c>Proxy-Authorization</c> header, no authenticated user, and no
/// authorization requirement on the endpoint (unless it allows anonymous
/// access). A response to any other request is served to that client only,
/// and an entry not recorded under this rule is never served.
/// Runs the standalone middleware (no worker) with the real native cache, in
/// the documented order: <c>UsePageSpeed()</c> first, then routing and
/// authentication. Requires the native library; skips when it is absent.
/// </summary>
public class AnonymousRecordTests : IDisposable
{
    private const string UserHeader = "X-Test-User";
    private const string PassHeader = "X-Test-Pass";
    private const string SchemeName = "Test";

    private const string OpenCss = "header{background:#0a0a0a}";
    private const string HeldCss = "header{background:#123456}";
    private const string HeldMarker = "123456";

    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), $"ps_anon_{Guid.NewGuid():N}");

    private readonly ConcurrentDictionary<string, int> _served = new();

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { }
    }

    /// <summary>Signs in the user named by the <c>X-Test-User</c> header.</summary>
    private sealed class HeaderAuthHandler : AuthenticationHandler<AuthenticationSchemeOptions>
    {
        public HeaderAuthHandler(
            IOptionsMonitor<AuthenticationSchemeOptions> options,
            ILoggerFactory logger, UrlEncoder encoder)
            : base(options, logger, encoder) { }

        protected override Task<AuthenticateResult> HandleAuthenticateAsync()
        {
            var name = Request.Headers[UserHeader].ToString();
            if (string.IsNullOrEmpty(name))
                return Task.FromResult(AuthenticateResult.NoResult());
            var identity = new ClaimsIdentity(
                new[] { new Claim(ClaimTypes.Name, name) }, SchemeName);
            return Task.FromResult(AuthenticateResult.Success(
                new AuthenticationTicket(new ClaimsPrincipal(identity), SchemeName)));
        }
    }

    // A policy that admits a request by a fact other than a signed-in user.
    private static void PassPolicy(AuthorizationPolicyBuilder p) =>
        p.RequireAssertion(c =>
            c.Resource is HttpContext h && h.Request.Headers.ContainsKey(PassHeader));

    private async Task<WebApplication?> StartAsync(
        Action<WebApplication> map, bool asyncCss = false)
    {
        WebApplication app;
        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = Path.Combine(_dir, "volume.dat");
                opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
                opts.Worker.AutoStart = false;  // standalone: no worker
                opts.Html.EnableAsyncCss = asyncCss;
            });
            builder.Services.AddAuthentication(SchemeName)
                .AddScheme<AuthenticationSchemeOptions, HeaderAuthHandler>(SchemeName, null);
            builder.Services.AddAuthorization();
            app = builder.Build();
        }
        catch (DllNotFoundException)
        {
            return null;  // native library not available
        }

        app.UsePageSpeed();
        app.UseRouting();
        app.UseAuthentication();
        app.UseAuthorization();
        map(app);

        try
        {
            await app.StartAsync();
        }
        catch (DllNotFoundException)
        {
            await app.DisposeAsync();
            return null;  // native library not available
        }
        return app;
    }

    private RouteHandlerBuilder Css(WebApplication app, string path, string body) =>
        Css(app, path, _ => body);

    private RouteHandlerBuilder Css(
        WebApplication app, string path, Func<HttpContext, string> body) =>
        app.MapGet(path, (HttpContext ctx) =>
        {
            _served.AddOrUpdate(path, 1, (_, n) => n + 1);
            ctx.Response.Headers.CacheControl = "public, max-age=3600";
            return Results.Text(body(ctx), "text/css");
        });

    private RouteHandlerBuilder Page(WebApplication app, string path, string html) =>
        app.MapGet(path, (HttpContext ctx) =>
        {
            _served.AddOrUpdate(path, 1, (_, n) => n + 1);
            ctx.Response.Headers.CacheControl = "public, max-age=60";
            return Results.Text(html, "text/html");
        });

    private int Served(string path) => _served.TryGetValue(path, out var n) ? n : 0;

    private static HttpClient Client(WebApplication app)
    {
        var client = app.GetTestClient();
        client.BaseAddress = new Uri("https://localhost/");
        return client;
    }

    private static HttpRequestMessage Get(string path, bool html,
        params (string Name, string Value)[] headers)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, path);
        if (html) request.Headers.Accept.ParseAdd("text/html");
        foreach (var (name, value) in headers)
            request.Headers.TryAddWithoutValidation(name, value);
        return request;
    }

    private static string XPageSpeed(HttpResponseMessage response) =>
        response.Headers.TryGetValues("X-PageSpeed", out var v)
            ? string.Join(",", v) : "";

    private static string CriticalBlock(string page)
    {
        var block = Regex.Match(page,
            "<style[^>]*data-pagespeed-critical[^>]*>(.*?)</style>",
            RegexOptions.Singleline);
        return block.Success ? block.Groups[1].Value : "";
    }

    private const string PageHtml =
        "<!doctype html><html><head>" +
        "<link rel=\"stylesheet\" href=\"/open.css\">" +
        "</head><body><header>Top</header><main><p>Hello</p></main>" +
        "</body></html>";

    // ── The request kinds whose responses are not stored ─────────────

    public static IEnumerable<object[]> Admitted()
    {
        foreach (var html in new[] { false, true })
        {
            yield return new object[] { "authorization-header", html };
            yield return new object[] { "signed-in-user", html };
            yield return new object[] { "authorize-endpoint", html };
        }
    }

    private static (string, string)[] AdmittedHeaders(string kind) => kind switch
    {
        // No handler reads Basic: the user stays anonymous, only the header
        // tells.
        "authorization-header" => new[] { ("Authorization", "Basic dXNlcjpwYXNz") },
        "signed-in-user" => new[] { (UserHeader, "alice") },
        // Admitted by the endpoint's policy without a signed-in user.
        "authorize-endpoint" => new[] { (PassHeader, "1") },
        _ => throw new ArgumentException(kind),
    };

    [Theory]
    [MemberData(nameof(Admitted))]
    public async Task ResponseToNonAnonymousRequest_IsNotCached(string kind, bool html)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            var target = html
                ? Page(a, path, PageHtml)
                : Css(a, path, HeldCss);
            if (kind == "authorize-endpoint") target.RequireAuthorization(PassPolicy);
        });
        if (app == null) return;
        try
        {
            var client = Client(app);
            // An anonymous visit records the page's stylesheet.
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();

            var first = await client.SendAsync(Get(path, html, AdmittedHeaders(kind)));
            Assert.Equal(HttpStatusCode.OK, first.StatusCode);
            var firstBody = await first.Content.ReadAsStringAsync();
            if (html)
            {
                // Still optimized for this client.
                Assert.Contains("0a0a0a", CriticalBlock(firstBody));
            }
            else
            {
                Assert.Contains(HeldMarker, firstBody);
            }
            Assert.Equal(1, Served(path));

            // A following anonymous request is a miss: the application answers.
            var second = await client.SendAsync(Get(path, html));
            var secondBody = await second.Content.ReadAsStringAsync();
            Assert.NotEqual("HIT", XPageSpeed(second));
            if (kind == "authorize-endpoint")
            {
                Assert.Equal(HttpStatusCode.Unauthorized, second.StatusCode);
                Assert.DoesNotContain(HeldMarker, secondBody);
                Assert.DoesNotContain("<header>", secondBody);
            }
            else
            {
                Assert.Equal(HttpStatusCode.OK, second.StatusCode);
                Assert.Equal(2, Served(path));
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // Positive control for the theory above: the same responses to anonymous
    // requests are stored and served from the cache, also on an endpoint
    // with an authorization requirement that allows anonymous access.
    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task ResponseToAnonymousRequest_IsCached(bool html, bool allowAnonymous)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            var target = html ? Page(a, path, PageHtml) : Css(a, path, HeldCss);
            if (allowAnonymous) target.RequireAuthorization(PassPolicy).AllowAnonymous();
        });
        if (app == null) return;
        try
        {
            var client = Client(app);
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();
            (await client.SendAsync(Get(path, html))).EnsureSuccessStatusCode();
            var second = await client.SendAsync(Get(path, html));
            second.EnsureSuccessStatusCode();
            Assert.Equal("HIT", XPageSpeed(second));
            Assert.Equal(1, Served(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── A stylesheet held back from anonymous clients ───────────────

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CriticalCss_NeverFromAStylesheetFirstFetchedBySignedInClient(bool asyncCss)
    {
        const string page =
            "<!doctype html><html><head>" +
            "<link rel=\"stylesheet\" href=\"/open.css\">" +
            "<link rel=\"stylesheet\" href=\"/held.css\">" +
            "</head><body><header>Top</header><main><p>Hello</p></main>" +
            "</body></html>";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            Css(a, "/held.css", HeldCss).RequireAuthorization();
            Page(a, "/", page);
        }, asyncCss);
        if (app == null) return;
        try
        {
            var client = Client(app);
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();
            for (var i = 0; i < 3; i++)
            {
                var primed = await client.SendAsync(
                    Get("/held.css", false, (UserHeader, "alice")));
                Assert.Equal(HttpStatusCode.OK, primed.StatusCode);
                Assert.Contains(HeldMarker, await primed.Content.ReadAsStringAsync());
            }

            // The page for an anonymous client, built and then from the cache.
            for (var i = 0; i < 2; i++)
            {
                var response = await client.SendAsync(Get("/", true));
                response.EnsureSuccessStatusCode();
                var body = await response.Content.ReadAsStringAsync();
                Assert.Contains("0a0a0a", CriticalBlock(body));
                Assert.DoesNotContain(HeldMarker, body);
            }

            // The signed-in client still gets the stylesheet from the application.
            var own = await client.SendAsync(Get("/held.css", false, (UserHeader, "alice")));
            Assert.Equal(HttpStatusCode.OK, own.StatusCode);
            Assert.Contains(HeldMarker, await own.Content.ReadAsStringAsync());
            Assert.Equal(4, Served("/held.css"));

            // An anonymous client is refused by the application, never served
            // the stylesheet from the cache.
            var anonymous = await client.GetAsync("/held.css");
            Assert.Equal(HttpStatusCode.Unauthorized, anonymous.StatusCode);
            Assert.NotEqual("HIT", XPageSpeed(anonymous));
            Assert.DoesNotContain(HeldMarker, await anonymous.Content.ReadAsStringAsync());
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [Fact]
    public async Task CriticalCss_NeverCarriesAnotherUsersStylesheet()
    {
        // One URL, a body per signed-in user.
        const string page =
            "<!doctype html><html><head>" +
            "<link rel=\"stylesheet\" href=\"/theme.css\">" +
            "</head><body><header>Top</header><main><p>Hello</p></main>" +
            "</body></html>";
        var app = await StartAsync(a =>
        {
            Css(a, "/theme.css", ctx =>
                ctx.Request.Headers[UserHeader].ToString() == "alice"
                    ? "header{background:#aa0001}"
                    : "header{background:#bb0002}");
            Page(a, "/", page);
        });
        if (app == null) return;
        try
        {
            var client = Client(app);
            for (var i = 0; i < 3; i++)
            {
                (await client.SendAsync(Get("/theme.css", false, (UserHeader, "alice"))))
                    .EnsureSuccessStatusCode();
            }
            foreach (var user in new[] { "bob", "" })
            {
                var request = user.Length > 0
                    ? Get("/", true, (UserHeader, user))
                    : Get("/", true);
                var response = await client.SendAsync(request);
                response.EnsureSuccessStatusCode();
                Assert.DoesNotContain("aa0001", await response.Content.ReadAsStringAsync());
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── Entries recorded without the stamp ──────────────────────────

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CacheEntryWithoutAnonymousRecordStamp_IsAMiss(bool stamped)
    {
        const string path = "/planted.css";
        var app = await StartAsync(a => Css(a, path, OpenCss));
        if (app == null) return;
        try
        {
            byte flags = CacheFlags.HeadersClassified;
            if (stamped) flags |= CacheFlags.AnonymousRecorded;
            var planted = Encoding.UTF8.GetBytes("header{background:#feed00}");
            var cache = app.Services.GetRequiredService<IPageSpeedCache>();
            using (var writer = cache.BeginWrite(path, "localhost", "https",
                new CacheWriteParams
                {
                    AlternateId = 0x08,
                    ContentLength = (ulong)planted.Length,
                    FullMask = 0x08,
                    ContentType = PageSpeedContentType.Css,
                    Flags = flags,
                    OriginContentType = "text/css",
                }))
            {
                writer.Write(planted);
                writer.Commit();
            }

            var response = await Client(app).GetAsync(path);
            response.EnsureSuccessStatusCode();
            var body = await response.Content.ReadAsStringAsync();
            if (stamped)
            {
                // Control: the planted entry is readable and served.
                Assert.Equal("HIT", XPageSpeed(response));
                Assert.Contains("feed00", body);
                Assert.Equal(0, Served(path));
            }
            else
            {
                Assert.NotEqual("HIT", XPageSpeed(response));
                Assert.DoesNotContain("feed00", body);
                Assert.Equal(1, Served(path));
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }
}
