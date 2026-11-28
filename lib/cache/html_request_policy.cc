// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/html_request_policy.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sha256.hpp"

namespace pagespeed {
namespace {

char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
  }
  return true;
}

bool StartsWith(std::string_view s, std::string_view prefix, bool fold) {
  if (s.size() < prefix.size()) return false;
  const std::string_view head = s.substr(0, prefix.size());
  return fold ? EqualsIgnoreCase(head, prefix) : head == prefix;
}

bool EndsWith(std::string_view s, std::string_view suffix, bool fold) {
  if (s.size() < suffix.size()) return false;
  const std::string_view tail = s.substr(s.size() - suffix.size());
  return fold ? EqualsIgnoreCase(tail, suffix) : tail == suffix;
}

// RFC 9110 section 5.6.3 optional whitespace.
std::string_view TrimOws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

// RFC 9110 section 5.6.2 tchar.
bool IsTokenChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
      (u >= 'A' && u <= 'Z')) {
    return true;
  }
  constexpr std::string_view kSpecials = "!#$%&'*+-.^_`|~";
  return kSpecials.find(c) != std::string_view::npos;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = LowerAscii(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Every well-formed %XX decoded; anything else kept as is.
std::string PercentDecode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = HexValue(s[i + 1]);
      const int lo = HexValue(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

// ---- the request guard -----------------------------------------------------

// Lowercase, with `-`; a request's name is compared with `_` read as `-`.
constexpr std::array<std::string_view, 19> kUnkeyedHeaders = {
    "x-forwarded-host",
    "x-host",
    "x-original-host",
    "x-original-url",
    "x-rewrite-url",
    "x-forwarded-port",
    "x-forwarded-scheme",
    "x-forwarded-prefix",
    "x-http-method-override",
    "x-method-override",
    "forwarded",
    "x-forwarded-ssl",
    "front-end-https",
    "x-http-method",
    "x-forwarded-protocol",
    "x-url-scheme",
    "x-forwarded-server",
    "x-original-uri",
    "x-forwarded-uri"};

// A request header name against a lowercase, `-`-spelled one: ASCII case is
// folded and `_` reads as `-`, because CGI-style origins map both spellings
// to the same variable.
bool HeaderNameIs(std::string_view name, std::string_view lower) {
  if (name.size() != lower.size()) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i] == '_' ? '-' : LowerAscii(name[i]);
    if (c != lower[i]) return false;
  }
  return true;
}

// Whether `value` contains `needle` (lowercase), ASCII case folded: the
// test frameworks apply to an AJAX marker.
bool ContainsIgnoreCase(std::string_view value, std::string_view needle) {
  if (needle.size() > value.size()) return false;
  for (size_t i = 0; i + needle.size() <= value.size(); ++i) {
    if (EqualsIgnoreCase(value.substr(i, needle.size()), needle)) return true;
  }
  return false;
}

constexpr std::array<std::string_view, 14> kStaticExtensions = {
    "css",  "js",  "png",  "jpg",   "jpeg", "gif", "svg",
    "webp", "ico", "woff", "woff2", "json", "txt", "xml"};

bool PathRefused(std::string_view raw_path) {
  const std::string_view path = raw_path.substr(0, raw_path.find('?'));
  if (path.find_first_of(";\\") != std::string_view::npos) return true;
  // An encoded separator: the server may decode it into a second path.
  for (size_t i = 0; i + 2 < path.size(); ++i) {
    if (path[i] != '%') continue;
    const char a = path[i + 1];
    const char b = LowerAscii(path[i + 2]);
    if ((a == '2' && b == 'f') || (a == '5' && b == 'c')) return true;
  }
  // Dot segments, with either dot spelled as %2e.
  size_t start = 0;
  std::string_view last;
  while (true) {
    const size_t slash = path.find('/', start);
    const std::string_view segment = path.substr(
        start, slash == std::string_view::npos ? std::string_view::npos
                                               : slash - start);
    const std::string decoded = PercentDecode(segment);
    if (decoded == "." || decoded == "..") return true;
    if (slash == std::string_view::npos) {
      last = segment;
      break;
    }
    start = slash + 1;
  }
  // A static-asset name that answered text/html is the deception shape.
  // Trailing dots and spaces are dropped first: some servers strip them, so
  // `x.css.` and `x.css%20` name `x.css` there.
  std::string decoded_last = PercentDecode(last);
  while (!decoded_last.empty() &&
         (decoded_last.back() == '.' || decoded_last.back() == ' ')) {
    decoded_last.pop_back();
  }
  const size_t dot = decoded_last.rfind('.');
  if (dot == std::string::npos) return false;
  const std::string_view ext = std::string_view(decoded_last).substr(dot + 1);
  for (std::string_view known : kStaticExtensions) {
    if (EqualsIgnoreCase(ext, known)) return true;
  }
  return false;
}

// ---- cookies ---------------------------------------------------------------

// `*` is a wildcard only at one end.
bool MatchesPattern(std::string_view name, std::string_view pattern,
                    bool fold) {
  if (!pattern.empty() && pattern.back() == '*') {
    return StartsWith(name, pattern.substr(0, pattern.size() - 1), fold);
  }
  if (!pattern.empty() && pattern.front() == '*') {
    return EndsWith(name, pattern.substr(1), fold);
  }
  return fold ? EqualsIgnoreCase(name, pattern) : name == pattern;
}

// Exact names, or the prefixes the vendors document, so no unrelated cookie
// that merely shares a few leading characters is ignored.
constexpr std::array<std::string_view, 40> kBuiltinIgnorable = {
    // Google Analytics and Ads.
    "_ga", "_ga_*", "_gid", "_gat", "_gat_*", "_gcl_*", "__utma", "__utmb",
    "__utmc", "__utmt", "__utmt_*", "__utmv", "__utmz",
    // Meta.
    "_fbp", "_fbc",
    // Hotjar.
    "_hjid", "_hjSession_*", "_hjSessionUser_*", "_hjFirstSeen",
    "_hjAbsoluteSessionInProgress", "_hjIncludedInPageviewSample",
    "_hjIncludedInSessionSample", "_hjTLDTest", "_hjLocalStorageTest",
    "_hjSessionStorageTest", "_hjCookieTest", "_hjViewportId",
    "_hjRecordingEnabled", "_hjRecordingLastActivity", "_hjUserAttributesHash",
    "_hjClosedSurveyInvites", "_hjDonePolls",
    // Microsoft Clarity.
    "_clck", "_clsk",
    // Matomo.
    "_pk_id.*", "_pk_ses.*", "_pk_ref.*",
    // Microsoft Advertising.
    "_uetsid", "_uetvid", "_uetmsclkid"};

constexpr std::array<std::string_view, 18> kSessionPatterns = {
    "PHPSESSID",
    "JSESSIONID",
    "ASP.NET_SessionId",
    ".ASPXAUTH",
    ".AspNetCore.*",
    "laravel_session",
    "connect.sid",
    "sessionid",
    "csrftoken",
    "XSRF-TOKEN",
    "SESS*",
    "SSESS*",
    "wordpress_*",
    "wp-postpass_*",
    "comment_author_*",
    "woocommerce_*",
    "wp_woocommerce_session_*",
    "*_session"};

bool IsSessionName(std::string_view name) {
  for (std::string_view pattern : kSessionPatterns) {
    if (MatchesPattern(name, pattern, /*fold=*/true)) return true;
  }
  return false;
}

// RFC 6265 section 4.1.1 cookie-octet.
bool IsCookieOctet(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u == 0x21 || (u >= 0x23 && u <= 0x2B) || (u >= 0x2D && u <= 0x3A) ||
         (u >= 0x3C && u <= 0x5B) || (u >= 0x5D && u <= 0x7E);
}

}  // namespace

HtmlGuardVerdict HtmlRequestGuard(std::string_view raw_path,
                                  std::span<const HeaderField> headers,
                                  std::string_view keyed_scheme) {
  HtmlGuardVerdict v;
  if (PathRefused(raw_path)) {
    v.reason = HtmlGuardReason::kPath;
    return v;
  }
  for (size_t i = 0; i < headers.size(); ++i) {
    const std::string_view name = headers[i].name;
    const std::string_view value = TrimOws(headers[i].value);
    bool refused = false;
    for (std::string_view unkeyed : kUnkeyedHeaders) {
      if (HeaderNameIs(name, unkeyed)) refused = true;
    }
    // X-Original-URL that repeats the keyed target byte for byte names the
    // URL the page is stored under, so an origin that builds the page from
    // it builds that URL's page.  Only the `-` spelling, and never a path
    // with an empty segment, which origins read in more than one way.
    if (refused && HeaderNameIs(name, "x-original-url") &&
        name.find('_') == std::string_view::npos && !value.empty() &&
        value == raw_path &&
        value.substr(0, value.find('?')).find("//") == std::string_view::npos) {
      refused = false;
    }
    // A keyed scheme other than http or https matches no value.
    const bool known_scheme = EqualsIgnoreCase(keyed_scheme, "http") ||
                              EqualsIgnoreCase(keyed_scheme, "https");
    if (HeaderNameIs(name, "x-forwarded-proto") &&
        (!known_scheme || !EqualsIgnoreCase(value, keyed_scheme))) {
      refused = true;
      v.xfp = true;
    }
    if (HeaderNameIs(name, "x-requested-with") &&
        ContainsIgnoreCase(value, "xmlhttprequest")) {
      refused = true;
    }
    if ((HeaderNameIs(name, "x-microsoftajax") ||
         HeaderNameIs(name, "x-prototype-version")) &&
        !value.empty()) {
      refused = true;
    }
    if (refused) {
      v.reason = HtmlGuardReason::kRequestHeader;
      v.header_index = static_cast<int64_t>(i);
      return v;
    }
  }
  return v;
}

HtmlIgnoreCookieGuardVerdict HtmlIgnoreCookieGuard(std::string_view addition) {
  HtmlIgnoreCookieGuardVerdict v;
  if (addition.empty()) return v;
  const size_t stars =
      static_cast<size_t>(std::count(addition.begin(), addition.end(), '*'));
  const bool prefix = stars == 1 && addition.back() == '*';
  if (stars > 1 || (stars == 1 && !prefix)) return v;
  const std::string_view base =
      prefix ? addition.substr(0, addition.size() - 1) : addition;
  if (base.empty() || (prefix && base.size() < 3)) return v;
  for (char c : base) {
    if (!IsTokenChar(c) || c == '*') return v;
  }
  for (std::string_view pattern : kSessionPatterns) {
    if (!prefix) {
      if (MatchesPattern(base, pattern, /*fold=*/true)) return v;
      continue;
    }
    // A prefix overlaps a pattern when either fixed leading part extends the
    // other; a pattern with no fixed leading part (`*_session`) is left to
    // classification, which tests it first.
    std::string_view lead = pattern;
    if (!lead.empty() && lead.front() == '*') continue;
    if (!lead.empty() && lead.back() == '*') lead.remove_suffix(1);
    if (StartsWith(lead, base, /*fold=*/true) ||
        StartsWith(base, lead, /*fold=*/true)) {
      return v;
    }
  }
  v.accepted = true;
  v.warning = prefix;
  return v;
}

HtmlCookieVerdict ClassifyHtmlCookies(
    std::span<const std::string_view> cookie_lines, bool ignore_builtin,
    std::span<const std::string_view> additions) {
  HtmlCookieVerdict v;
  if (cookie_lines.empty()) return v;
  v.cookie_class = HtmlCookieClass::kIgnorableOnly;
  std::vector<std::string_view> accepted;
  for (std::string_view a : additions) {
    if (HtmlIgnoreCookieGuard(a).accepted) accepted.push_back(a);
  }
  const auto block = [&v](size_t line, std::string_view whole,
                          std::string_view name, bool malformed) {
    v.cookie_class = HtmlCookieClass::kBlocking;
    v.malformed = malformed;
    v.ignorable_values.clear();
    v.first_blocking.line = line;
    v.first_blocking.offset =
        name.empty() ? 0 : static_cast<size_t>(name.data() - whole.data());
    v.first_blocking.length = name.size();
    return v;
  };
  for (size_t li = 0; li < cookie_lines.size(); ++li) {
    const std::string_view line = cookie_lines[li];
    size_t pairs = 0;
    std::string_view rest = line;
    while (true) {
      const size_t semi = rest.find(';');
      const std::string_view member = TrimOws(rest.substr(0, semi));
      if (!member.empty()) {
        ++pairs;
        const size_t eq = member.find('=');
        if (eq == std::string_view::npos) return block(li, line, {}, true);
        const std::string_view name = TrimOws(member.substr(0, eq));
        std::string_view value = TrimOws(member.substr(eq + 1));
        if (name.empty()) return block(li, line, {}, true);
        for (char c : name) {
          if (!IsTokenChar(c)) return block(li, line, {}, true);
        }
        // RFC 6265 section 4.1.1: a value is quoted as a whole or not at
        // all, and holds cookie-octets only (no space, comma, backslash,
        // quote, control byte or byte above 0x7E). Origin parsers split a
        // value outside that grammar differently, some on a comma or a
        // space, so it can hide a second cookie.
        if (!value.empty() && value.front() == '"') {
          if (value.size() < 2 || value.back() != '"') {
            return block(li, line, {}, true);
          }
          value = value.substr(1, value.size() - 2);
        }
        for (char c : value) {
          if (!IsCookieOctet(c)) return block(li, line, {}, true);
        }
        bool ignorable = false;
        if (!IsSessionName(name)) {
          if (ignore_builtin) {
            for (std::string_view p : kBuiltinIgnorable) {
              if (MatchesPattern(name, p, /*fold=*/false)) ignorable = true;
            }
          }
          for (std::string_view p : accepted) {
            if (MatchesPattern(name, p, /*fold=*/false)) ignorable = true;
          }
        }
        if (!ignorable) return block(li, line, name, false);
        v.ignorable_values.push_back(
            {li, static_cast<size_t>(value.data() - line.data()),
             value.size()});
      }
      if (semi == std::string_view::npos) break;
      rest = rest.substr(semi + 1);
    }
    // An empty or separator-only line names nothing a client could mean.
    if (pairs == 0) return block(li, line, {}, true);
  }
  return v;
}

uint64_t HtmlPolicyStamp(HtmlCredentialedPolicy credentialed,
                         bool ignore_builtin, uint32_t list_version,
                         std::span<const std::string_view> additions) {
  std::vector<std::string_view> sorted(additions.begin(), additions.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string input = "pagespeed html policy stamp 1\n";
  input += "credentialed=" + std::to_string(static_cast<int>(credentialed));
  input += "\nbuiltin=" + std::to_string(ignore_builtin ? 1 : 0);
  input += "\nlist=" + std::to_string(list_version);
  // The list this library applies, whatever version its caller was built
  // with, so a library that changes the list re-records every page.
  input += "\nlibrary_list=" + std::to_string(kHtmlCookieListVersion);
  for (std::string_view a : sorted) {
    // Length-prefixed, so no addition can look like two.
    input += "\n" + std::to_string(a.size()) + ":";
    input.append(a);
  }
  const auto digest = cyclone::crypto::SHA256::hash(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(input.data()), input.size()));
  uint64_t stamp = 0;
  for (size_t i = 0; i < 8; ++i) {
    stamp = (stamp << 8) | static_cast<uint8_t>(digest[i]);
  }
  // Zero is "no stamp" everywhere it is stored.
  return stamp == 0 ? 1 : stamp;
}

bool HtmlUnsafeMethodInvalidates(std::string_view method, int status) {
  if (status < 200 || status > 399) return false;
  return method != "GET" && method != "HEAD" && method != "OPTIONS" &&
         method != "TRACE";
}

}  // namespace pagespeed
