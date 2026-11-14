// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_COOKIE_CACHE_GATE_H_
#define PAGESPEED_SRC_NGINX_COOKIE_CACHE_GATE_H_

// Shared-cache gate for HTML requests that carry a Cookie header.  Pure C++
// (no nginx headers) so the decision is unit-testable without an nginx build
// (same pattern as authz_cache_gate).
//
// The rule is the HTML cache policy's (lib/cache/html_cache_policy.h, store
// rule 8 and serve rule 4), called through HtmlCookieRequestMayShare so the
// port and the policy cannot drift: a page fetched for a request that carried
// a cookie is recorded only when the origin marked it explicitly shareable
// (`public`, or `s-maxage`), and a stored page is served to a cookie-bearing
// request only when the stored entry carries the same mark.  A page that
// varies on the cookie it was fetched with would otherwise be replayed to
// visitors holding a different cookie, or none.
//
// Scope: HTML, and the Markdown copy rendered from it (the caller passes the
// HTML parent's stored flags for that copy).  Stylesheets, scripts and images
// are outside the policy and pass unchanged.
//
// Both call sites speak the AlternateMetadata origin_cc_flags vocabulary: the
// record path passes the just-parsed response flags, the HIT path the STORED
// entry's flags.  The s-maxage mark keys on kCCOriginSMaxagePresent, as the
// §3.5 gate does.  Flags 0 (no Cache-Control at store time, or no stored
// parent) carry no mark and the gate fails closed for a cookie-bearing
// request.
//
// Independent of the Authorization gate (authz_cache_gate.h): a request
// carrying both must pass both.

#include <cstdint>

namespace pagespeed {

// True when the shared cache may record (record path) or serve (HIT path) an
// entry for this request as far as the Cookie rule is concerned.  Always true
// when the request carries no Cookie header or the entry is not HTML.
bool CookieCacheGateAllows(bool request_has_cookie, bool html_class,
                           uint16_t origin_cc_flags);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_COOKIE_CACHE_GATE_H_
