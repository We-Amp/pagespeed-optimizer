// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/browser/agent_fetcher.h"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "gtest/gtest.h"
#include "lib/net/fetch_policy.h"
#include "lib/net/upstream_pin.h"

// Agent fetcher battery. The dangerous direction throughout is an
// EGRESS that the policy should have refused — to a lateral/private host, an
// un-allowlisted third party, a rebinding host, or a redirect target that re-points
// somewhere private. Every "no fetch / deny" case below is an SSRF or exfil pivot
// the render must not make. All effects (spawn, DNS) are mocked, so this runs
// offline and hermetically on every platform.

namespace pagespeed {
namespace {

// ---- mock effects ---------------------------------------------------------

CurlSpawnOutcome Ok(std::string raw) {
  CurlSpawnOutcome o;
  o.spawned = true;
  o.exit_code = 0;
  o.output = std::move(raw);
  return o;
}

CurlSpawnOutcome CurlFailed(int code) {
  CurlSpawnOutcome o;
  o.spawned = true;
  o.exit_code = code;
  return o;
}

struct FakeSpawn {
  std::vector<std::vector<std::string>> calls;
  std::function<CurlSpawnOutcome(const std::vector<std::string>&, int)> handler;
  CurlSpawnOutcome operator()(const std::vector<std::string>& argv,
                              std::size_t) {
    int idx = static_cast<int>(calls.size());
    calls.push_back(argv);
    return handler(argv, idx);
  }
};

AgentFetcherDeps MakeDeps(FakeSpawn* spawn,
                          std::map<std::string, std::vector<std::string>> dns) {
  AgentFetcherDeps d;
  d.spawn = [spawn](const std::vector<std::string>& a, std::size_t m) {
    return (*spawn)(a, m);
  };
  d.resolve = [dns](std::string_view host) -> std::vector<std::string> {
    auto it = dns.find(std::string(host));
    return it == dns.end() ? std::vector<std::string>{} : it->second;
  };
  d.max_redirect_hops = 5;
  return d;
}

FetchPolicy MakePolicy() {
  FetchPolicy p;
  auto up =
      ParseUpstream("http://127.0.0.1:3000");  // the SPA's own loopback API
  if (up.has_value()) p.pinned_upstreams.push_back(*up);
  p.allow_hosts = {"cdn.example.com"};  // one opt-in third party
  return p;
}

std::string UrlOf(const std::vector<std::string>& argv) {
  for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--url") return argv[i + 1];
  }
  return "";
}
bool HasResolve(const std::vector<std::string>& argv,
                const std::string& triple) {
  for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--resolve" && argv[i + 1] == triple) return true;
  }
  return false;
}
bool HasAnyResolve(const std::vector<std::string>& argv) {
  for (const std::string& a : argv) {
    if (a == "--resolve") return true;
  }
  return false;
}

// ---- ParseCurlResponse ----------------------------------------------------

TEST(ParseCurlResponse, BasicCrlf) {
  auto r = ParseCurlResponse(
      "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "
      "5\r\n\r\nhello");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->body, "hello");
  EXPECT_TRUE(r->location.empty());
}

TEST(ParseCurlResponse, LfOnlyEmptyBody) {
  auto r = ParseCurlResponse("HTTP/2 204\nX-Foo: bar\n\n");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->status, 204);
  EXPECT_EQ(r->body, "");
}

TEST(ParseCurlResponse, LocationCaseInsensitive) {
  auto r =
      ParseCurlResponse("HTTP/1.1 302 Found\r\nLOCATION: https://x/y\r\n\r\n");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->status, 302);
  EXPECT_EQ(r->location, "https://x/y");
}

TEST(ParseCurlResponse, SkipsInterim1xx) {
  // 103 Early Hints (increasingly common) precedes the real 200 in curl -i output.
  auto r = ParseCurlResponse(
      "HTTP/1.1 103 Early Hints\r\nLink: </s.css>\r\n\r\n"
      "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\n\r\nbody-bytes");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->body, "body-bytes");
}

TEST(ParseCurlResponse, MalformedRejected) {
  EXPECT_FALSE(ParseCurlResponse("not http\r\n\r\n").has_value());
  EXPECT_FALSE(
      ParseCurlResponse("HTTP/1.1 200 OK").has_value());  // no terminator
  EXPECT_FALSE(ParseCurlResponse("").has_value());
  EXPECT_FALSE(ParseCurlResponse("HTTP/1.1 2x0 OK\r\n\r\n").has_value());
  EXPECT_FALSE(
      ParseCurlResponse("HTTP/1.1 2000 OK\r\n\r\n").has_value());  // 4 digits
}

// ---- SanitizeResponseHeaders ----------------------------------------------

TEST(SanitizeResponseHeaders, StripsHopByHopAndStateful) {
  std::vector<HttpHeader> in = {
      {"Content-Type", "text/html"},    {"Set-Cookie", "a=b"},
      {"Transfer-Encoding", "chunked"}, {"Content-Length", "10"},
      {"Connection", "keep-alive"},     {"Cache-Control", "no-cache"},
  };
  auto out = SanitizeResponseHeaders(in);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].name, "Content-Type");
  EXPECT_EQ(out[1].name, "Cache-Control");
}

// ---- ResolveRedirectTarget ------------------------------------------------

TEST(ResolveRedirectTarget, Absolute) {
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p", "https://b.com/x"),
            "https://b.com/x");
}
TEST(ResolveRedirectTarget, SchemeRelative) {
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p", "//b.com/x"),
            "https://b.com/x");
}
TEST(ResolveRedirectTarget, RootRelative) {
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p/q", "/r"),
            "https://a.com/r");
}
TEST(ResolveRedirectTarget, PathRelative) {
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p/q", "r"),
            "https://a.com/p/r");
}
TEST(ResolveRedirectTarget, QueryOnly) {
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p?x=1", "?y=2"),
            "https://a.com/p?y=2");
}
TEST(ResolveRedirectTarget, RejectsUnusable) {
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p", "#frag"), "");
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p", ""), "");
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p", "   "), "");
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p", "http://"),
            "");  // no host
}
TEST(ResolveRedirectTarget, NonHttpSchemeCannotChangeHost) {
  // A non-http(s) Location is treated as a same-host relative path; it can never
  // re-point the host (and is re-adjudicated regardless).
  EXPECT_EQ(ResolveRedirectTarget("https://a.com/p", "javascript:x"),
            "https://a.com/javascript:x");
}

// ---- BuildScrubbedCurlEnv -------------------------------------------------

TEST(BuildScrubbedCurlEnv, KeepsOnlyAllowlist) {
  std::vector<std::string> src = {
      "PATH=/usr/bin:/bin",
      "HOME=/root",
      "http_proxy=http://evil:8080",
      "https_proxy=http://evil:8080",
      "all_proxy=socks5://evil",
      "CURL_HOME=/tmp/evil",
      "NETRC=/tmp/netrc",
      "SSL_CERT_FILE=/etc/ssl/cert.pem",
      "CURLOPT_PROXY=x",
      "LD_PRELOAD=/tmp/x.so",
  };
  auto out = BuildScrubbedCurlEnv(src);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], "PATH=/usr/bin:/bin");
  EXPECT_EQ(out[1], "SSL_CERT_FILE=/etc/ssl/cert.pem");
  for (const std::string& e : out) {
    EXPECT_EQ(e.find("proxy"), std::string::npos);
    EXPECT_EQ(e.find("HOME"), std::string::npos);
  }
}
TEST(BuildScrubbedCurlEnv, SkipsMalformedEntries) {
  auto out = BuildScrubbedCurlEnv({"NOEQUALS", "PATH=/bin"});
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], "PATH=/bin");
}

// ---- FetchSubresource (integration with mock spawn/resolve) ---------------

TEST(FetchSubresource, SameOriginUpstreamProxied) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    return Ok(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"x\":1}");
  };
  auto deps = MakeDeps(&spawn, {});  // literal upstream, no DNS needed
  auto out = FetchSubresource("http://127.0.0.1:3000/api", ResourceClass::kXhr,
                              MakePolicy(), deps);
  EXPECT_TRUE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kProxyUpstream);
  EXPECT_EQ(out.status, 200);
  EXPECT_EQ(out.body, "{\"x\":1}");
  ASSERT_EQ(spawn.calls.size(), 1u);
  EXPECT_FALSE(HasAnyResolve(spawn.calls[0]));  // literal host == pinned ip
  EXPECT_EQ(UrlOf(spawn.calls[0]), "http://127.0.0.1:3000/api");
}

TEST(FetchSubresource, AllowlistedCdnFetchedAndPinned) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    return Ok(
        "HTTP/1.1 200 OK\r\nContent-Type: "
        "text/javascript\r\n\r\nconsole.log(1)");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_TRUE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kFetchAllowlisted);
  EXPECT_EQ(out.body, "console.log(1)");
  ASSERT_EQ(spawn.calls.size(), 1u);
  EXPECT_TRUE(HasResolve(spawn.calls[0], "cdn.example.com:443:93.184.216.34"));
}

// THE keystone SSRF test: an allowlisted host that DNS-rebinds to a private IP must
// be blocked BEFORE egress (no spawn, no fulfill).
TEST(FetchSubresource, AllowlistedCdnRebindToPrivateBlocked) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    ADD_FAILURE() << "spawn must NOT run for a private-rebind target";
    return Ok("HTTP/1.1 200 OK\r\n\r\n");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"127.0.0.1"}}});  // rebind
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 0u);
}

// EVERY resolved address must be public — one private among several => block.
TEST(FetchSubresource, AllowlistedCdnMixedAddressesBlocked) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    ADD_FAILURE() << "must not egress when any address is private";
    return Ok("HTTP/1.1 200 OK\r\n\r\nx");
  };
  auto deps =
      MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34", "10.0.0.5"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(spawn.calls.size(), 0u);
}

// G1 carve-out: a configured upstream that resolves to a PRIVATE IP IS allowed
// (byte-equals config) — proving G2's public-only rule does not apply to G1.
TEST(FetchSubresource, UpstreamHostnameResolvingPrivateAllowed) {
  FetchPolicy p;
  auto up = ParseUpstream("http://backend.internal:8080");
  ASSERT_TRUE(up.has_value());
  p.pinned_upstreams.push_back(*up);
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    return Ok("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\nOK");
  };
  auto deps = MakeDeps(&spawn, {{"backend.internal", {"10.0.0.5"}}});
  auto out = FetchSubresource("http://backend.internal:8080/api",
                              ResourceClass::kXhr, p, deps);
  EXPECT_TRUE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kProxyUpstream);
  ASSERT_EQ(spawn.calls.size(), 1u);
  EXPECT_TRUE(HasResolve(spawn.calls[0], "backend.internal:8080:10.0.0.5"));
}

TEST(FetchSubresource, ImageKeepsElementNoFetch) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    ADD_FAILURE() << "images are never byte-fetched";
    return Ok("");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/a.png",
                              ResourceClass::kImage, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyKeepElement);
  EXPECT_EQ(spawn.calls.size(), 0u);
}

TEST(FetchSubresource, WebSocketAndUnlistedThirdPartyDenied) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    ADD_FAILURE() << "must not egress";
    return Ok("");
  };
  auto deps = MakeDeps(&spawn, {{"evil.example", {"93.184.216.34"}}});
  auto p = MakePolicy();
  auto ws = FetchSubresource("http://127.0.0.1:3000/ws",
                             ResourceClass::kWebSocket, p, deps);
  EXPECT_EQ(ws.action, FetchAction::kDenyBlockedByClient);
  auto third = FetchSubresource("https://evil.example/x.js",
                                ResourceClass::kScript, p, deps);
  EXPECT_EQ(third.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 0u);
}

TEST(FetchSubresource, RedirectWithinAllowlistFollowed) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&,
                     int idx) -> CurlSpawnOutcome {
    if (idx == 0) {
      return Ok(
          "HTTP/1.1 302 Found\r\nLocation: "
          "https://cdn.example.com/v2/lib.js\r\n\r\n");
    }
    return Ok("HTTP/1.1 200 OK\r\nContent-Type: text/javascript\r\n\r\nFINAL");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_TRUE(out.fulfilled);
  EXPECT_EQ(out.body, "FINAL");
  EXPECT_EQ(out.final_url, "https://cdn.example.com/v2/lib.js");
  EXPECT_EQ(spawn.calls.size(), 2u);
}

// A redirect to a loopback non-upstream is re-adjudicated and denied mid-chain.
TEST(FetchSubresource, RedirectToLoopbackDenied) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&,
                     int idx) -> CurlSpawnOutcome {
    if (idx == 0) {
      return Ok(
          "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:6379/\r\n\r\n");
    }
    ADD_FAILURE() << "must not fetch the loopback redirect target";
    return Ok("HTTP/1.1 200 OK\r\n\r\n");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 1u);  // only the first hop ran
}

// A redirect to a DIFFERENT allowlisted host that rebinds private => SSRF block on
// the hop (the per-hop resolve+IsPublicUrl catches it).
TEST(FetchSubresource, RedirectToRebindingAllowlistHostDenied) {
  FetchPolicy p;
  p.allow_hosts = {"cdn.example.com", "cdn2.example.com"};
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&,
                     int idx) -> CurlSpawnOutcome {
    if (idx == 0) {
      return Ok(
          "HTTP/1.1 301 Moved\r\nLocation: "
          "https://cdn2.example.com/x.js\r\n\r\n");
    }
    ADD_FAILURE() << "must not fetch the rebinding host";
    return Ok("HTTP/1.1 200 OK\r\n\r\n");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}},
                                {"cdn2.example.com", {"169.254.169.254"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, p, deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(spawn.calls.size(), 1u);
}

// require_https (trust-anchor fetches): the scheme floor applies to
// EVERY hop — an http redirect target is denied before any egress, and an
// http INITIAL url never spawns at all.
TEST(FetchSubresource, RequireHttpsDeniesHttpRedirectHop) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&,
                     int idx) -> CurlSpawnOutcome {
    if (idx == 0) {
      return Ok(
          "HTTP/1.1 301 Moved\r\nLocation: "
          "http://cdn.example.com/downgraded.js\r\n\r\n");
    }
    ADD_FAILURE() << "must not fetch the http downgrade target";
    return Ok("HTTP/1.1 200 OK\r\n\r\n");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  deps.require_https = true;
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kFetch, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 1u);  // only the https hop ran
}

TEST(FetchSubresource, RequireHttpsDeniesHttpInitialUrl) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) -> CurlSpawnOutcome {
    ADD_FAILURE() << "must not spawn for an http url under require_https";
    return Ok("HTTP/1.1 200 OK\r\n\r\n");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  deps.require_https = true;
  auto out = FetchSubresource("http://cdn.example.com/lib.js",
                              ResourceClass::kFetch, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_TRUE(spawn.calls.empty());
}

TEST(FetchSubresource, RedirectBoundEnforced) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&,
                     int idx) -> CurlSpawnOutcome {
    return Ok("HTTP/1.1 302 Found\r\nLocation: https://cdn.example.com/p" +
              std::to_string(idx) + "\r\n\r\n");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  deps.max_redirect_hops = 3;
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 4u);  // hops 0..3 fetch; hop 4 (>3) denies
}

TEST(FetchSubresource, CurlTransportFailureDenied) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    return CurlFailed(7);  // e.g. connection refused
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
}

TEST(FetchSubresource, UnparseableResponseDenied) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    return Ok("garbage with no headers");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
}

TEST(FetchSubresource, ResolveFailureDenied) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    ADD_FAILURE() << "must not egress when DNS fails";
    return Ok("");
  };
  auto deps = MakeDeps(&spawn, {});  // cdn.example.com NOT resolvable
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 0u);
}

TEST(FetchSubresource, FulfilledHeadersAreSanitized) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    return Ok(
        "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\nSet-Cookie: s=1\r\n"
        "Transfer-Encoding: chunked\r\n\r\nbody");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/a.css",
                              ResourceClass::kStylesheet, MakePolicy(), deps);
  ASSERT_TRUE(out.fulfilled);
  for (const HttpHeader& h : out.headers) {
    std::string lname = absl::AsciiStrToLower(h.name);
    EXPECT_NE(lname, "set-cookie");
    EXPECT_NE(lname, "transfer-encoding");
  }
}

TEST(FetchSubresource, MisconfiguredDepsDeny) {
  AgentFetcherDeps deps;  // no spawn/resolve injected
  auto out = FetchSubresource("https://cdn.example.com/x.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
}

// MF-2: the G1 own-origin carve-out allows private backends but must REFUSE a
// configured origin that resolves to the cloud-metadata endpoint — no spawn.
TEST(FetchSubresource, G1UpstreamResolvingLinkLocalBlocked) {
  FetchPolicy p;
  auto up = ParseUpstream("http://app.customer.com:80");
  ASSERT_TRUE(up.has_value());
  p.pinned_upstreams.push_back(*up);
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    ADD_FAILURE() << "must not fetch a link-local/metadata pin";
    return Ok("HTTP/1.1 200 OK\r\n\r\nCREDS");
  };
  auto deps = MakeDeps(&spawn, {{"app.customer.com", {"169.254.169.254"}}});
  auto out = FetchSubresource("http://app.customer.com/api",
                              ResourceClass::kXhr, p, deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 0u);
}

// MF-2 hardening (consolidated review): the G1 link-local guard must reject if
// ANY resolved address is link-local, not just the pinned front(). A resolver
// returning [private, metadata] would otherwise pin the private front() and
// slip the metadata IP past the guard — and a future fallback could reach it.
TEST(FetchSubresource, G1UpstreamLinkLocalNotFirstAddressBlocked) {
  FetchPolicy p;
  auto up = ParseUpstream("http://app.customer.com:80");
  ASSERT_TRUE(up.has_value());
  p.pinned_upstreams.push_back(*up);
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&, int) {
    ADD_FAILURE() << "must not fetch when any resolved address is link-local";
    return Ok("HTTP/1.1 200 OK\r\n\r\nCREDS");
  };
  // The link-local address is SECOND; the pre-fix guard only checked front().
  auto deps =
      MakeDeps(&spawn, {{"app.customer.com", {"10.0.0.5", "169.254.169.254"}}});
  auto out = FetchSubresource("http://app.customer.com/api",
                              ResourceClass::kXhr, p, deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(), 0u);
}

// MF-1: a redirect Location whose authority uses a backslash-before-@ would make
// curl connect to a different host than the policy validated; the argv builder
// rejects the parser-divergent byte, so the hop fails closed (no second fetch).
TEST(FetchSubresource, RedirectWithBackslashAuthorityDenied) {
  FakeSpawn spawn;
  spawn.handler = [](const std::vector<std::string>&,
                     int idx) -> CurlSpawnOutcome {
    if (idx == 0) {
      return Ok(
          "HTTP/1.1 302 Found\r\nLocation: https://cdn.example.com\\@127.0.0.1/"
          "\r\n\r\n");
    }
    ADD_FAILURE() << "must not fetch the parser-divergent redirect target";
    return Ok("HTTP/1.1 200 OK\r\n\r\nX");
  };
  auto deps = MakeDeps(&spawn, {{"cdn.example.com", {"93.184.216.34"}}});
  auto out = FetchSubresource("https://cdn.example.com/lib.js",
                              ResourceClass::kScript, MakePolicy(), deps);
  EXPECT_FALSE(out.fulfilled);
  EXPECT_EQ(out.action, FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(spawn.calls.size(),
            1u);  // only hop 0 ran; hop 1 (backslash) denied
}

// SF-1: encoding/integrity/framing headers describe a transform the fetcher never
// applied (it serves curl's literal bytes) and MUST be stripped before fulfill.
TEST(SanitizeResponseHeaders, StripsEncodingIntegrityFraming) {
  std::vector<HttpHeader> in = {
      {"Content-Type", "text/html"}, {"Content-Encoding", "gzip"},
      {"Content-MD5", "abc"},        {"Content-Range", "bytes 0-1/2"},
      {"Cache-Control", "no-cache"},
  };
  auto out = SanitizeResponseHeaders(in);
  for (const HttpHeader& h : out) {
    std::string l = absl::AsciiStrToLower(h.name);
    EXPECT_NE(l, "content-encoding");
    EXPECT_NE(l, "content-md5");
    EXPECT_NE(l, "content-range");
  }
  ASSERT_EQ(out.size(), 2u);  // Content-Type + Cache-Control survive
}

// SF-3: header names that aren't tokens and values carrying control bytes are
// dropped — they could split/corrupt the response Chrome synthesizes from fulfill.
TEST(SanitizeResponseHeaders, DropsControlCharAndBadNameHeaders) {
  std::vector<HttpHeader> in = {
      {"Content-Type", "text/html"},
      {"X-Evil", std::string("a\rb")},    // interior CR
      {"X-Nul", std::string("a\0b", 3)},  // NUL in value
      {"Bad Name", "v"},                  // space => not a token
      {"", "v"},                          // empty name
      {"X-Ok", "fine"},
  };
  auto out = SanitizeResponseHeaders(in);
  for (const HttpHeader& h : out) {
    EXPECT_NE(h.name, "X-Evil");
    EXPECT_NE(h.name, "X-Nul");
    EXPECT_NE(h.name, "Bad Name");
    EXPECT_FALSE(h.name.empty());
  }
  ASSERT_EQ(out.size(), 2u);  // Content-Type + X-Ok survive
}

}  // namespace
}  // namespace pagespeed
