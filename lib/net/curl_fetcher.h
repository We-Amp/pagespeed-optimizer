// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef LIB_NET_CURL_FETCHER_H_
#define LIB_NET_CURL_FETCHER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Builds the argv for ONE IP-pinned, credential-stripped, GET-only `curl`
// invocation for the agent_optimize fetcher (spawn-curl model — the
// same posix_spawn-the-binary pattern the license client already uses). This is a
// PURE function (no spawn) so the security-critical command shape is unit-testable;
// the actual posix_spawn on the libuv work pool + the CDP Fetch.fulfillRequest
// bridge land in P1.4 (they need the worker event loop).
//
// Security properties encoded here (asserted by tests):
//   * `--resolve host:port:IP` pins the connection to the caller's already-resolved
//     and SSRF-validated IP; TLS SNI + the Host header stay the original hostname,
//     closing DNS rebinding on this path.
//   * `--proto =http,https` + `--proto-redir =http,https` — scheme allowlist.
//   * `--max-redirs 0` and NO `-L` — curl never auto-follows; the fetcher follows
//     manually so each redirect hop is re-adjudicated (G1/G2) before egress.
//   * GET only; NO cookies / Authorization / request body / forwarded page headers
//     (anonymous view, §3.4) — only a fixed neutral UA + Accept are sent.
//   * `--url <url>` passes the URL as an explicit option value so a `-`-leading URL
//     can never be interpreted as a curl flag (argv-injection guard).
//   * `--max-time` + `--max-filesize` bound time and response size.

namespace pagespeed {

struct CurlFetchOptions {
  int max_time_sec = 5;                       // hard request timeout
  std::size_t max_response_bytes = 5u << 20;  // 5 MiB response cap
  std::string user_agent =                    // fixed, anonymous (no page UA)
      "Mozilla/5.0 (compatible; mod_pagespeed agent_optimize)";
  std::string accept = "text/html,application/xhtml+xml,*/*";
};

// Build the argv (argv[0] == "curl") to fetch `url`, pinning `pinned_ip` for the
// connection. `host`/`port` are the URL's authority (host without brackets for an
// IPv6 literal). The caller MUST have already parsed the URL, run the G1/G2 policy,
// resolved the host, and SSRF-validated `pinned_ip`. Returns an empty vector if any
// argument is empty/invalid (fail-closed — never emit an un-pinned command).
std::vector<std::string> BuildPinnedGetArgv(std::string_view url,
                                            std::string_view host, int port,
                                            std::string_view pinned_ip,
                                            const CurlFetchOptions& opts);

}  // namespace pagespeed

#endif  // LIB_NET_CURL_FETCHER_H_
