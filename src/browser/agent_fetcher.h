// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef SRC_BROWSER_AGENT_FETCHER_H_
#define SRC_BROWSER_AGENT_FETCHER_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/net/curl_fetcher.h"
#include "lib/net/fetch_policy.h"

// The out-of-Chrome, IP-pinned subresource fetcher for the agent_optimize
// render. This is the SYSTEMS layer wrapped around the pure lib/net
// kernel (G1 upstream_pin + G2 ssrf_guard + fetch_policy + the pinned-curl argv
// builder): it resolves DNS, runs the SSRF gate on the resolved addresses, spawns
// the hardened pinned curl, parses the `-i` HTTP response, and follows redirects
// MANUALLY — re-adjudicating G1/G2 on every hop before any egress.
//
// It performs NO CDP I/O. The CDP Fetch bridge (page_analysis.cc, P1.4d) runs
// FetchSubresource() on the libuv work pool and, back on the loop thread, turns the
// returned AgentFetchOutcome into a Fetch.fulfillRequest (on a fulfilled fetch) or
// a Fetch.failRequest (on any deny/failure). Chrome itself stays air-gapped
// (Network offline:true) — ALL egress is via this fetcher, IP-pinned and policed.
//
// Everything except the two injected blocking effects (spawn, resolve) is pure and
// cross-platform, so the redirect engine + response parser + header hygiene + env
// scrub are unit-tested hermetically with a mock spawn/resolver. The real spawn and
// resolver are POSIX implementations; on Windows they are fail-closed stubs (the P1
// agent_optimize worker ships behind nginx on Linux).
//
// Fail-closed everywhere: any resolve/SSRF/parse/spawn failure, a malformed URL or
// port, an un-resolvable redirect, or exceeding the redirect bound => a DENY outcome
// (never a partial or unvalidated fulfill).

namespace pagespeed {

struct HttpHeader {
  std::string name;
  std::string value;
};

// The result of spawning ONE curl. `spawned`==false => the process never started.
// `exit_code` unset => it did not complete (spawn failure / oversize kill). `output`
// is curl's stdout (the `-i` raw HTTP response), truncated to the byte cap.
struct CurlSpawnOutcome {
  bool spawned = false;
  std::optional<int> exit_code;
  std::string output;
  std::string error;  // diagnostic only
};

// Injected blocking effects (so FetchSubresource() stays pure + hermetically
// testable):
//  * SpawnCurlFn — run `argv` (argv[0]=="curl") to completion with a SCRUBBED
//    environment, capturing up to `max_bytes` of stdout. The real impl posix_spawns
//    with a PATH/CA-only envp (no HOME/CURL_HOME/NETRC/*_proxy/CURLOPT_* — H2 +
//    proxy-pin defense in depth).
//  * ResolveHostFn — resolve `host` to its A/AAAA presentation-form addresses
//    (empty on failure). The real impl is getaddrinfo. Feeds the SSRF guard and
//    supplies the IP to pin.
using SpawnCurlFn = std::function<CurlSpawnOutcome(
    const std::vector<std::string>& argv, std::size_t max_bytes)>;
using ResolveHostFn =
    std::function<std::vector<std::string>(std::string_view host)>;

struct AgentFetcherDeps {
  SpawnCurlFn spawn;
  ResolveHostFn resolve;
  CurlFetchOptions curl_opts;
  int max_redirect_hops = 5;
  // Require the https scheme on EVERY hop (initial URL and each redirect
  // target), not just whatever the scheme allowlist permits.  Set by callers
  // fetching trust-anchor material (Web Bot Auth key directories):
  // a single http hop would hand the response body to an on-path attacker.
  bool require_https = false;
};

// The per-render agent_optimize configuration handed to PageAnalyzer::Analyze.
// `policy` == nullptr (the default) preserves the legacy offline/cache-only render
// (no egress). When `policy` is set, the CDP Fetch bridge runs FetchSubresource()
// (using `deps`) on the libuv work pool for each paused request and turns the result
// into Fetch.fulfillRequest / Fetch.failRequest. Held by shared_ptr so an in-flight
// async fetch keeps the policy + deps alive even if the render session ends.
struct AgentRenderOptions {
  std::shared_ptr<const FetchPolicy> policy;
  AgentFetcherDeps deps;
  // Settle delay (ms) between networkIdle and reading the rendered DOM, to let
  // hydration flush remaining microtasks/paints (content fidelity = the moat).
  // 0 reads immediately (tests).
  int settle_ms = 1500;
};

// What the CDP bridge should do with a paused request after adjudication+fetch.
struct AgentFetchOutcome {
  // The terminal policy action.
  //   * fulfilled==true (only with kProxyUpstream/kFetchAllowlisted)
  //                                    => Fetch.fulfillRequest(status, headers, body)
  //   * action==kDenyKeepElement       => Fetch.failRequest (the <img>/<video>
  //                                       element + its src/alt stay in the DOM)
  //   * any other (deny/fetch failure) => Fetch.failRequest BlockedByClient
  FetchAction action = FetchAction::kDenyBlockedByClient;
  bool fulfilled = false;  // true => status/headers/body are valid
  int status = 0;
  std::vector<HttpHeader>
      headers;             // hygiene'd (hop-by-hop + Set-Cookie stripped)
  std::string body;        // raw bytes (the bridge base64-encodes)
  std::string final_url;   // URL after redirects (diagnostics)
  std::string diagnostic;  // human-readable deny/fail reason (logging)
};

// Adjudicate + (if allowed) fetch one subresource. BLOCKING — runs on the libuv
// work pool, never the loop thread.
AgentFetchOutcome FetchSubresource(std::string_view url, ResourceClass rc,
                                   const FetchPolicy& policy,
                                   const AgentFetcherDeps& deps);

// ---- Exposed for unit testing (pure helpers) ------------------------------

// A parsed HTTP response from curl `-i` stdout (leading 1xx interim blocks are
// skipped). nullopt on a malformed status line / missing header terminator.
struct ParsedHttpResponse {
  int status = 0;
  std::vector<HttpHeader> headers;  // as received (name case preserved)
  std::string body;
  std::string
      location;  // value of the (case-insensitive) Location header, or ""
};
std::optional<ParsedHttpResponse> ParseCurlResponse(std::string_view raw);

// Resolve a (possibly relative) redirect target against the current absolute URL.
// Supports absolute, scheme-relative (//host/p), root-relative (/p), and simple
// path/query-relative targets. Returns "" for anything it cannot resolve to an
// http(s) absolute URL (=> the caller fails closed). The result is ALWAYS re-run
// through the full G1/G2 gate, so this need only be reasonable, not perfect — it
// can never widen the policy because the joined URL's host is re-parsed + re-checked.
std::string ResolveRedirectTarget(std::string_view base_url,
                                  std::string_view location);

// Drop hop-by-hop + framing + stateful headers (Set-Cookie, Connection,
// Transfer-Encoding, Content-Length, ...) that must not be forwarded verbatim into
// Fetch.fulfillRequest (Chrome re-derives framing from the body we hand it).
std::vector<HttpHeader> SanitizeResponseHeaders(
    const std::vector<HttpHeader>& headers);

// Build the scrubbed curl environment (a list of "KEY=VALUE") from `source_env`
// (entries "KEY=VALUE"), keeping ONLY a safe allowlist (PATH + the TLS CA vars) so
// nothing the host environment carries (HOME/CURL_HOME/NETRC/*_proxy/CURLOPT_*) can
// change curl's behavior or defeat the IP pin. Pure (source injected) for testing.
std::vector<std::string> BuildScrubbedCurlEnv(
    const std::vector<std::string>& source_env);

// Production effects (Windows: fail-closed stubs). Factories so the bridge gets the
// real syscalls while tests inject mocks.
SpawnCurlFn RealCurlSpawn();
ResolveHostFn RealHostResolver();

}  // namespace pagespeed

#endif  // SRC_BROWSER_AGENT_FETCHER_H_
