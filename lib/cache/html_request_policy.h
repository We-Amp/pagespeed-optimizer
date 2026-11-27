// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_HTML_REQUEST_POLICY_H_
#define PAGESPEED_LIB_CACHE_HTML_REQUEST_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "lib/cache/headers_sidecar.h"

namespace pagespeed {

// The request side of the HTML cache policy: the checks a front end runs on
// the REQUEST before it reads or records anything, written down once so every
// front end gets the same answer.  Pure functions of their arguments, like the
// rest of the policy (html_cache_policy.h): no I/O, no globals, no clock.
//
//   HtmlRequestGuard          unkeyed request headers and cache-deception
//                             paths: one call, before any read, for the
//                             store and the serve side alike;
//   ClassifyHtmlCookies       the Cookie lines sorted into none,
//                             ignorable-only or blocking;
//   HtmlIgnoreCookieGuard     the config-load check on an operator's
//                             addition to the ignore list;
//   HtmlPolicyStamp           the stamp of the credentialed policy in force;
//   HtmlUnsafeMethodInvalidates
//                             whether a response to an unsafe method
//                             invalidates the stored page.
//
// The error direction of every choice is toward the origin: a refusal costs
// one cache entry or one origin fetch, whereas a wrong admission replays one
// client's page to another.

// ---- the request guard -----------------------------------------------------

enum class HtmlGuardReason : uint8_t {
  kNone = 0,
  kRequestHeader = 1,  // an unkeyed header that can change the response
  kPath = 2,           // a path two spellings could reach, or a static name
};

struct HtmlGuardVerdict {
  HtmlGuardReason reason = HtmlGuardReason::kNone;
  // The index of the refused header line in the caller's array; -1 for a
  // path refusal or none.
  int64_t header_index = -1;
  // The refusal is an X-Forwarded-Proto that does not name the keyed scheme
  // (the one case an operator fixes with a setting, so a caller can say so).
  bool xfp = false;
};

// UNKEYED REQUEST HEADERS.  The cache key is the scheme, host and path.  A
// request header that an origin may use to build the page but that is not in
// the key would let one request poison the page served to every other, so a
// request carrying one is neither stored nor answered:
//   X-Forwarded-Host, X-Host, X-Original-Host, X-Original-URL, X-Rewrite-URL,
//   X-Forwarded-Port, X-Forwarded-Scheme, X-Forwarded-Prefix,
//   X-HTTP-Method-Override, X-Method-Override, Forwarded, X-Forwarded-Ssl,
//   Front-End-Https, X-HTTP-Method, X-Forwarded-Protocol, X-Url-Scheme,
//   X-Forwarded-Server, X-Original-URI, X-Forwarded-Uri
// (any value, an empty one included).  Every name here and below is compared
// case-insensitively with `_` read as `-`, because CGI-style origins map both
// spellings to one variable.  X-Forwarded-Proto is refused only when a
// line's value, trimmed and compared case-insensitively, is not exactly the
// keyed scheme (a list, an empty value, a second line that disagrees, or a
// keyed scheme other than http or https is refused).  X-Forwarded-For and
// X-Real-IP are allowed.
//
// AJAX MARKERS.  A request a front end treats as AJAX may be answered with a
// fragment, which must never be stored under the page's key nor answered with
// the page: X-Requested-With whose value contains XMLHttpRequest (any case,
// as frameworks test it), and
// X-MicrosoftAjax or X-Prototype-Version with a non-empty value.
//
// CACHE-DECEPTION PATHS.  `raw_path` is the request target as received; a
// query (from the first `?`) is not examined except as said below.  Refused:
// a `;` in the path; `%2F` or `%5C` in any case, or a raw `\`; a dot segment
// (`.` or `..`, also with either dot percent-encoded as `%2e`); and a last
// path segment whose extension (after its last `.`) is a static-asset one:
// css js png jpg jpeg gif svg webp ico woff woff2 json txt xml (any case),
// read after percent-decoding the segment and dropping trailing dots and
// spaces (which some servers strip).  A path ending in `/` has an empty last
// segment.
//
// The first refusal in this order wins: the path, then each header line in
// the caller's order.
HtmlGuardVerdict HtmlRequestGuard(std::string_view raw_path,
                                  std::span<const HeaderField> headers,
                                  std::string_view keyed_scheme);

// ---- cookies ---------------------------------------------------------------

// The built-in ignore list's version.  It is part of the policy stamp, so a
// change to the list re-records every page once.
inline constexpr uint32_t kHtmlCookieListVersion = 1;

enum class HtmlCookieClass : uint8_t {
  kNone = 0,           // no Cookie line
  kIgnorableOnly = 1,  // every cookie is on the ignore list in force
  kBlocking = 2,       // anything else
};

// A byte range into the caller's cookie lines.
struct HtmlCookieSpan {
  size_t line = 0;
  size_t offset = 0;
  size_t length = 0;
};

struct HtmlCookieVerdict {
  HtmlCookieClass cookie_class = HtmlCookieClass::kNone;
  // A pair did not parse (no `=`, an empty or non-token name, a value
  // outside the RFC 6265 cookie-octets once its surrounding quotes are
  // removed: a space, comma, backslash, quote, control byte or byte above
  // 0x7E) or a line held no pair at all.
  bool malformed = false;
  // The first blocking cookie's name; length 0 when there is none to name
  // (a malformed pair, an empty line).
  HtmlCookieSpan first_blocking;
  // The listed cookies' values, in order, without surrounding quotes, for a
  // caller's check that none is echoed into the stored page.
  std::vector<HtmlCookieSpan> ignorable_values;
};

// Sort the request's Cookie lines.  A cookie is ignorable when its name is on
// the built-in list (with `ignore_builtin`) or matches an operator addition,
// and is not a session cookie.  The built-in list, case-sensitive, with `*`
// a trailing wildcard:
//   _ga _ga_* _gid _gat _gat_* _gcl_* __utma __utmb __utmc __utmt __utmt_*
//   __utmv __utmz _fbp _fbc _hjid _hjSession_* _hjSessionUser_* _hjFirstSeen
//   _hjAbsoluteSessionInProgress _hjIncludedInPageviewSample
//   _hjIncludedInSessionSample _hjTLDTest _hjLocalStorageTest
//   _hjSessionStorageTest _hjCookieTest _hjViewportId _hjRecordingEnabled
//   _hjRecordingLastActivity _hjUserAttributesHash _hjClosedSurveyInvites
//   _hjDonePolls _clck _clsk _pk_id.* _pk_ses.* _pk_ref.* _uetsid _uetvid
//   _uetmsclkid
// (exact names, and the prefixes the vendors document).
// SESSION PATTERNS are tested first and are never ignorable, whatever the
// additions (compared case-insensitively):
//   PHPSESSID JSESSIONID ASP.NET_SessionId .ASPXAUTH .AspNetCore.*
//   laravel_session connect.sid sessionid csrftoken XSRF-TOKEN SESS* SSESS*
//   wordpress_* wp-postpass_* comment_author_* woocommerce_*
//   wp_woocommerce_session_* *_session
// An addition that HtmlIgnoreCookieGuard refuses is ignored.  Pairs are split
// on `;`, with optional whitespace around names and values (a value is then
// RFC 6265 cookie-octets, optionally quoted as a whole); empty members are
// skipped, but a line with no pair at all is blocking.  Scanning stops at the
// first blocking cookie.
HtmlCookieVerdict ClassifyHtmlCookies(
    std::span<const std::string_view> cookie_lines, bool ignore_builtin,
    std::span<const std::string_view> additions);

struct HtmlIgnoreCookieGuardVerdict {
  bool accepted = false;
  // An accepted prefix: it may still match a session-shaped name, which
  // classification then blocks.
  bool warning = false;
};

// The config-load check on one operator addition: a cookie name, or a prefix
// with one trailing `*`.  Refused: empty; `*` alone; a `*` other than one
// trailing one; fewer than 3 characters before the `*`; a character outside
// an RFC 9110 token; a name that matches a session pattern (any case); a
// prefix that would cover a session pattern's fixed leading part, or lies
// inside it (any case).
HtmlIgnoreCookieGuardVerdict HtmlIgnoreCookieGuard(std::string_view addition);

// ---- the policy stamp ------------------------------------------------------

enum class HtmlCredentialedPolicy : uint8_t {
  kAnonymousOnly = 0,
  kExplicitPublic = 1,
};

// A non-zero 64-bit stamp of the credentialed policy: the policy value, the
// built-in list switch and version, and the additions as a set (order and
// repeats do not matter).  Equal inputs give equal stamps on every build.
uint64_t HtmlPolicyStamp(HtmlCredentialedPolicy credentialed,
                         bool ignore_builtin, uint32_t list_version,
                         std::span<const std::string_view> additions);

// ---- unsafe methods --------------------------------------------------------

// RFC 9111 section 4.4: a response with a non-error status (200 to 399) to a
// request whose method is not known to be safe invalidates the stored page.
// The safe methods are GET, HEAD, OPTIONS and TRACE, compared
// case-sensitively (RFC 9110 section 9.1); an empty method is not one.
bool HtmlUnsafeMethodInvalidates(std::string_view method, int status);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_HTML_REQUEST_POLICY_H_
