// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/html_cache_policy.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using Meta = AlternateMetadata;

// ASCII-only folds, so nothing here changes meaning with the process locale.
char AsciiLowerChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= 'A' && u <= 'Z') ? static_cast<char>(u | 0x20) : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (AsciiLowerChar(a[i]) != AsciiLowerChar(b[i])) return false;
  }
  return true;
}

bool StartsWithIgnoreCase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         EqualsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  const size_t last = haystack.size() - needle.size();
  for (size_t i = 0; i <= last; ++i) {
    if (EqualsIgnoreCase(haystack.substr(i, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = AsciiLowerChar(c);
  return out;
}

// RFC 9110 §5.6.3 optional whitespace: SP and HTAB.
std::string_view TrimOws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

// A detail string safe for a log line: printable ASCII only, bounded.
std::string SanitizeDetail(std::string_view s) {
  std::string out;
  out.reserve(std::min(s.size(), kMaxHtmlStoreDetailBytes));
  for (char c : s) {
    if (out.size() >= kMaxHtmlStoreDetailBytes) break;
    const auto u = static_cast<unsigned char>(c);
    out.push_back((u >= 0x21 && u <= 0x7E) ? c : '?');
  }
  return out;
}

const HeaderField* FindHeader(std::span<const HeaderField> headers,
                              std::string_view name) {
  for (const HeaderField& h : headers) {
    if (EqualsIgnoreCase(h.name, name)) return &h;
  }
  return nullptr;
}

size_t CountHeader(std::span<const HeaderField> headers,
                   std::string_view name) {
  size_t n = 0;
  for (const HeaderField& h : headers) {
    if (EqualsIgnoreCase(h.name, name)) ++n;
  }
  return n;
}

// Does this Cache-Control line carry a `max-age` directive?  The shared
// parser records the value but not whether the directive was there, and an
// absent max-age and `max-age=0` must be told apart.
bool HasMaxAgeDirective(std::string_view line) {
  while (!line.empty()) {
    const size_t comma = line.find(',');
    std::string_view token = line.substr(0, comma);
    line = comma == std::string_view::npos ? std::string_view()
                                           : line.substr(comma + 1);
    const size_t eq = token.find('=');
    token = TrimOws(token.substr(0, eq));
    if (EqualsIgnoreCase(token, "max-age")) return true;
  }
  return false;
}

HtmlStoreVerdict Refuse(HtmlStoreVerdict v, HtmlStoreReason reason,
                        std::string detail = std::string()) {
  v.action = HtmlStoreAction::kRefuse;
  v.reason = reason;
  v.detail = std::move(detail);
  return v;
}

// ---- date arithmetic -----------------------------------------------------

// Days from 1970-01-01 to the given civil date (proleptic Gregorian).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool IsLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned DaysInMonth(int64_t y, unsigned m) {
  static constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30,
                                       31, 31, 30, 31, 30, 31};
  return (m == 2 && IsLeap(y)) ? 29 : kDays[m - 1];
}

bool ParseDigits(std::string_view s, unsigned* out) {
  unsigned v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  *out = v;
  return true;
}

uint32_t SaturateU32(int64_t v) {
  if (v <= 0) return 0;
  if (v > static_cast<int64_t>(UINT32_MAX)) return UINT32_MAX;
  return static_cast<uint32_t>(v);
}

// ---- personalisation scan ------------------------------------------------

constexpr size_t kMaxTagBytes = 4096;

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// The text of the tag starting at body[pos] == '<', through its closing '>',
// bounded by kMaxTagBytes.  A '>' inside a quoted attribute value does not
// close the tag.
std::string_view TagText(std::string_view body, size_t pos) {
  const size_t limit = std::min(body.size(), pos + kMaxTagBytes);
  char quote = 0;
  for (size_t i = pos + 1; i < limit; ++i) {
    const char c = body[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return body.substr(pos, i - pos + 1);
    }
  }
  return body.substr(pos, limit - pos);
}

// The value of attribute `name` in `tag` (the whole tag text, starting at
// '<'), or nullopt.  `tag_name_len` is the length of "<meta" and friends.
std::optional<std::string_view> FindAttribute(std::string_view tag,
                                              size_t tag_name_len,
                                              std::string_view name) {
  size_t i = tag_name_len;
  const size_t n = tag.size();
  while (i < n) {
    while (i < n && (IsAsciiSpace(tag[i]) || tag[i] == '/')) ++i;
    if (i >= n || tag[i] == '>') return std::nullopt;
    const size_t name_start = i;
    while (i < n && !IsAsciiSpace(tag[i]) && tag[i] != '=' && tag[i] != '>' &&
           tag[i] != '/') {
      ++i;
    }
    const std::string_view attr = tag.substr(name_start, i - name_start);
    while (i < n && IsAsciiSpace(tag[i])) ++i;
    std::string_view value;
    if (i < n && tag[i] == '=') {
      ++i;
      while (i < n && IsAsciiSpace(tag[i])) ++i;
      if (i < n && (tag[i] == '"' || tag[i] == '\'')) {
        const char q = tag[i++];
        const size_t v_start = i;
        while (i < n && tag[i] != q) ++i;
        value = tag.substr(v_start, i - v_start);
        if (i < n) ++i;
      } else {
        const size_t v_start = i;
        while (i < n && !IsAsciiSpace(tag[i]) && tag[i] != '>') ++i;
        value = tag.substr(v_start, i - v_start);
      }
    }
    if (!attr.empty() && EqualsIgnoreCase(attr, name)) return value;
  }
  return std::nullopt;
}

// True when `tag` begins with `<name` followed by a character that ends a tag
// name, so `<inputs` and `<formal` are not tags of interest.
bool IsTagNamed(std::string_view tag, std::string_view name_with_bracket) {
  if (!StartsWithIgnoreCase(tag, name_with_bracket)) return false;
  if (tag.size() == name_with_bracket.size()) return true;
  const char next = tag[name_with_bracket.size()];
  return IsAsciiSpace(next) || next == '/' || next == '>';
}

std::optional<std::string_view> SignalForTag(std::string_view tag) {
  if (IsTagNamed(tag, "<meta")) {
    const auto name = FindAttribute(tag, 5, "name");
    if (name.has_value()) {
      if (EqualsIgnoreCase(*name, "csrf-token")) return "meta_csrf_token";
      if (EqualsIgnoreCase(*name, "csrf-param")) return "meta_csrf_param";
    }
    return std::nullopt;
  }
  if (IsTagNamed(tag, "<input")) {
    const auto type = FindAttribute(tag, 6, "type");
    if (!type.has_value() || !EqualsIgnoreCase(*type, "hidden")) {
      return std::nullopt;
    }
    const auto name = FindAttribute(tag, 6, "name");
    if (!name.has_value()) return std::nullopt;
    if (EqualsIgnoreCase(*name, "csrfmiddlewaretoken")) {
      return "hidden_csrfmiddlewaretoken";
    }
    if (EqualsIgnoreCase(*name, "_token")) return "hidden_token";
    if (StartsWithIgnoreCase(*name, "csrf")) return "hidden_csrf";
    if (EqualsIgnoreCase(*name, "authenticity_token")) {
      return "hidden_authenticity";
    }
    if (EqualsIgnoreCase(*name, "__RequestVerificationToken")) {
      return "hidden_request_verification";
    }
    return std::nullopt;
  }
  if (IsTagNamed(tag, "<form")) {
    const auto method = FindAttribute(tag, 5, "method");
    if (!method.has_value() || !EqualsIgnoreCase(*method, "post")) {
      return std::nullopt;
    }
    const auto action = FindAttribute(tag, 5, "action");
    if (!action.has_value()) return std::nullopt;
    if (ContainsIgnoreCase(*action, "login") ||
        ContainsIgnoreCase(*action, "signin")) {
      return "post_form_login";
    }
    if (ContainsIgnoreCase(*action, "checkout") ||
        ContainsIgnoreCase(*action, "cart")) {
      return "post_form_checkout";
    }
    return std::nullopt;
  }
  return std::nullopt;
}

// The detail for a sidecar fall-through: the first reason's header name.
std::string SidecarDetail(const SidecarClassification& sidecar) {
  if (sidecar.reasons.empty()) return std::string();
  std::string_view reason = sidecar.reasons.front();
  if (reason == "sidecar-too-large") return "sidecar-too-large";
  const size_t colon = reason.find(':');
  if (colon == std::string_view::npos) return SanitizeDetail(reason);
  const std::string_view kind = reason.substr(0, colon);
  const std::string_view rest = reason.substr(colon + 1);
  if (kind == "vary-unstorable") return "vary";
  return SanitizeDetail(rest);
}

}  // namespace

// ---- names ---------------------------------------------------------------

std::string_view HtmlStoreActionName(HtmlStoreAction action) {
  switch (action) {
    case HtmlStoreAction::kRefuse:
      return "refuse";
    case HtmlStoreAction::kStore:
      return "store";
    case HtmlStoreAction::kStoreNoNotify:
      return "store_no_notify";
  }
  return "unknown";
}

std::string_view HtmlStoreReasonName(HtmlStoreReason reason) {
  static constexpr std::array<std::string_view, kHtmlStoreReasonCount> kNames =
      {"none",        "method",       "status",
       "incomplete",  "auth_request", "cookie_request",
       "set_cookie",  "private",      "no_store",
       "no_lifetime", "vary",         "headers",
       "nonce_csp",   "content_type", "encoding",
       "size",        "url",          "personalisation_signal",
       "no_transform"};
  const auto index = static_cast<size_t>(reason);
  return index < kNames.size() ? kNames[index] : "unknown";
}

std::string_view HtmlServeActionName(HtmlServeAction action) {
  switch (action) {
    case HtmlServeAction::kFallThrough:
      return "fall_through";
    case HtmlServeAction::kServeHtml:
      return "serve_html";
    case HtmlServeAction::kServeMarkdown:
      return "serve_markdown";
    case HtmlServeAction::kValidate:
      return "validate";
  }
  return "unknown";
}

std::string_view HtmlServeReasonName(HtmlServeReason reason) {
  static constexpr std::array<std::string_view, kHtmlServeReasonCount> kNames =
      {"fresh",           "mode",
       "auth_request",    "cookie_request",
       "client_no_cache", "needs_validation",
       "stale",           "headers_not_reproducible",
       "sidecar_missing", "purged",
       "absent",          "binding_stale",
       "parent_stale"};
  const auto index = static_cast<size_t>(reason);
  return index < kNames.size() ? kNames[index] : "unknown";
}

std::string_view HtmlCacheModeName(HtmlCacheMode mode) {
  switch (mode) {
    case HtmlCacheMode::kOff:
      return "off";
    case HtmlCacheMode::kReportOnly:
      return "report-only";
    case HtmlCacheMode::kServeVariants:
      return "serve-variants";
    case HtmlCacheMode::kServeCachedHtml:
      return "serve-cached-html";
  }
  return "unknown";
}

std::string HtmlStoreVerdict::reason_code() const {
  std::string code(HtmlStoreReasonName(reason));
  if ((reason == HtmlStoreReason::kHeaders ||
       reason == HtmlStoreReason::kVary) &&
      !detail.empty()) {
    code.push_back(':');
    code.append(detail);
  }
  return code;
}

std::string HtmlServeVerdict::reason_code() const {
  return std::string(HtmlServeReasonName(reason));
}

// ---- date ----------------------------------------------------------------

std::optional<int64_t> ParseHttpDateSeconds(std::string_view value) {
  // "Sun, 06 Nov 1994 08:49:37 GMT": exactly 29 bytes.
  if (value.size() != 29) return std::nullopt;
  if (value[3] != ',' || value[4] != ' ' || value[7] != ' ' ||
      value[11] != ' ' || value[16] != ' ' || value[19] != ':' ||
      value[22] != ':' || value[25] != ' ' || value.substr(26) != "GMT") {
    return std::nullopt;
  }
  static constexpr std::string_view kDays[] = {"Sun", "Mon", "Tue", "Wed",
                                               "Thu", "Fri", "Sat"};
  static constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr",
                                                 "May", "Jun", "Jul", "Aug",
                                                 "Sep", "Oct", "Nov", "Dec"};
  int weekday = -1;
  for (int i = 0; i < 7; ++i) {
    if (value.substr(0, 3) == kDays[i]) weekday = i;
  }
  if (weekday < 0) return std::nullopt;
  unsigned month = 0;
  for (unsigned i = 0; i < 12; ++i) {
    if (value.substr(8, 3) == kMonths[i]) month = i + 1;
  }
  if (month == 0) return std::nullopt;
  unsigned day = 0;
  unsigned year = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!ParseDigits(value.substr(5, 2), &day) ||
      !ParseDigits(value.substr(12, 4), &year) ||
      !ParseDigits(value.substr(17, 2), &hour) ||
      !ParseDigits(value.substr(20, 2), &minute) ||
      !ParseDigits(value.substr(23, 2), &second)) {
    return std::nullopt;
  }
  if (day < 1 || day > DaysInMonth(year, month)) return std::nullopt;
  if (hour > 23 || minute > 59 || second > 59) return std::nullopt;
  const int64_t days = DaysFromCivil(year, month, day);
  // 1970-01-01 was a Thursday (weekday index 4 with Sunday = 0).
  const int64_t expected = ((days % 7) + 7 + 4) % 7;
  if (expected != weekday) return std::nullopt;
  return days * 86400 + static_cast<int64_t>(hour) * 3600 +
         static_cast<int64_t>(minute) * 60 + static_cast<int64_t>(second);
}

// ---- personalisation -----------------------------------------------------

std::optional<std::string_view> FindPersonalisationSignal(
    std::string_view body_prefix) {
  if (body_prefix.size() > kPersonalisationScanBytes) {
    body_prefix = body_prefix.substr(0, kPersonalisationScanBytes);
  }
  size_t pos = 0;
  while (pos < body_prefix.size()) {
    pos = body_prefix.find('<', pos);
    if (pos == std::string_view::npos) break;
    const std::string_view rest = body_prefix.substr(pos);
    if (!IsTagNamed(rest, "<meta") && !IsTagNamed(rest, "<input") &&
        !IsTagNamed(rest, "<form")) {
      ++pos;
      continue;
    }
    const std::string_view tag = TagText(body_prefix, pos);
    if (auto signal = SignalForTag(tag)) return signal;
    // The tag's text is attribute text, not markup: a '<' inside it opens
    // nothing.  Resuming after it also keeps the scan linear.
    pos += tag.size();
  }
  return std::nullopt;
}

// ---- store ---------------------------------------------------------------

HtmlStoreVerdict ClassifyHtmlForStore(const HtmlStoreRequest& request,
                                      const HtmlStoreResponse& response,
                                      const HtmlStorePolicy& policy) {
  HtmlStoreVerdict v;
  const std::span<const HeaderField> headers = response.headers;

  // 1. Method, status, completeness.
  if (request.method != "GET")
    return Refuse(std::move(v), HtmlStoreReason::kMethod);
  if (response.status != 200)
    return Refuse(std::move(v), HtmlStoreReason::kStatus);
  if (!response.body_complete ||
      FindHeader(headers, "content-range") != nullptr) {
    return Refuse(std::move(v), HtmlStoreReason::kIncomplete);
  }

  // 2. Key-safe path.
  if (request.url_path.empty() || request.url_path.front() != '/' ||
      request.url_path.find_first_of("?%#") != std::string_view::npos) {
    return Refuse(std::move(v), HtmlStoreReason::kUrl);
  }

  // 3. Content type and encoding.
  if (CountHeader(headers, "content-type") != 1) {
    return Refuse(std::move(v), HtmlStoreReason::kContentType);
  }
  {
    std::string_view type = FindHeader(headers, "content-type")->value;
    type = TrimOws(type.substr(0, type.find(';')));
    if (!EqualsIgnoreCase(type, "text/html")) {
      return Refuse(std::move(v), HtmlStoreReason::kContentType);
    }
  }
  for (const HeaderField& h : headers) {
    if (EqualsIgnoreCase(h.name, "content-encoding") &&
        !EqualsIgnoreCase(TrimOws(h.value), "identity")) {
      return Refuse(std::move(v), HtmlStoreReason::kEncoding);
    }
  }

  // 4. No credentials.
  if (request.has_authorization || request.authenticated_user) {
    return Refuse(std::move(v), HtmlStoreReason::kAuthRequest);
  }

  // 5. No Set-Cookie.
  if (FindHeader(headers, "set-cookie") != nullptr ||
      FindHeader(headers, "set-cookie2") != nullptr) {
    return Refuse(std::move(v), HtmlStoreReason::kSetCookie);
  }

  // 6. Cache-Control.
  OriginCacheControl cc;
  bool max_age_present = false;
  for (const HeaderField& h : headers) {
    if (!EqualsIgnoreCase(h.name, "cache-control")) continue;
    AccumulateOriginCacheControl(h.value, &cc);
    if (HasMaxAgeDirective(h.value)) max_age_present = true;
  }
  v.cc_flags = cc.cc_flags;
  v.explicit_shared =
      (cc.cc_flags & (Meta::kCCOriginPublic | Meta::kCCOriginSMaxagePresent)) !=
      0;
  if ((cc.cc_flags & Meta::kCCOriginPrivate) != 0) {
    return Refuse(std::move(v), HtmlStoreReason::kPrivate);
  }
  if ((cc.cc_flags & Meta::kCCOriginNoStore) != 0) {
    return Refuse(std::move(v), HtmlStoreReason::kNoStore);
  }

  // 7. Cookie request.
  if (request.has_cookie && !v.explicit_shared) {
    return Refuse(std::move(v), HtmlStoreReason::kCookieRequest);
  }

  // 8. Vary: only Accept-Encoding is admitted.
  {
    std::string joined;
    for (const HeaderField& h : headers) {
      if (!EqualsIgnoreCase(h.name, "vary")) continue;
      if (!joined.empty()) joined.append(", ");
      joined.append(h.value);
    }
    std::string_view rest = joined;
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      const std::string_view token = TrimOws(rest.substr(0, comma));
      rest = comma == std::string_view::npos ? std::string_view()
                                             : rest.substr(comma + 1);
      if (!token.empty()) v.vary_tokens.push_back(Lower(token));
    }
    for (const std::string& token : v.vary_tokens) {
      if (token != "accept-encoding") {
        std::string detail = SanitizeDetail(token);
        return Refuse(std::move(v), HtmlStoreReason::kVary, std::move(detail));
      }
    }
  }

  // 9. Header reproducibility.
  v.sidecar = ClassifyForHeadersSidecar(headers);
  v.sidecar_evaluated = true;
  if (v.sidecar.verdict == SidecarVerdict::kNeverOptimized) {
    return Refuse(std::move(v), HtmlStoreReason::kNonceCsp);
  }
  if (v.sidecar.verdict == SidecarVerdict::kFallThrough) {
    std::string detail = SidecarDetail(v.sidecar);
    return Refuse(std::move(v), HtmlStoreReason::kHeaders, std::move(detail));
  }

  // 10. Explicit freshness, never heuristic.
  {
    const HeaderField* expires = FindHeader(headers, "expires");
    const bool no_cache = (cc.cc_flags & Meta::kCCOriginNoCache) != 0;
    if ((cc.cc_flags & Meta::kCCOriginSMaxagePresent) != 0) {
      v.lifetime_s = cc.s_maxage;
      v.lifetime_explicit = true;
    } else if (max_age_present) {
      v.lifetime_s = cc.max_age;
      v.lifetime_explicit = true;
    } else if (expires != nullptr) {
      v.lifetime_explicit = true;
      int64_t date = policy.now_seconds;
      if (const HeaderField* date_header = FindHeader(headers, "date")) {
        if (auto parsed = ParseHttpDateSeconds(date_header->value)) {
          date = *parsed;
        }
      }
      if (auto parsed = ParseHttpDateSeconds(expires->value)) {
        v.lifetime_s = SaturateU32(*parsed - date);
      }
    }
    v.needs_validation = v.lifetime_s == 0 || no_cache;
    for (const HeaderField& h : headers) {
      if ((EqualsIgnoreCase(h.name, "etag") ||
           EqualsIgnoreCase(h.name, "last-modified")) &&
          !TrimOws(h.value).empty()) {
        v.has_validators = true;
      }
    }
    if ((!v.lifetime_explicit && !v.has_validators) ||
        (v.needs_validation && !v.has_validators)) {
      return Refuse(std::move(v), HtmlStoreReason::kNoLifetime);
    }
  }

  // 11. Size.
  if (response.body_bytes >
      std::min(policy.max_body_bytes, kHtmlStoreHardMaxBodyBytes)) {
    return Refuse(std::move(v), HtmlStoreReason::kSize);
  }

  // 12. Personalisation signals.
  if (auto signal = FindPersonalisationSignal(response.body_prefix)) {
    return Refuse(std::move(v), HtmlStoreReason::kPersonalisationSignal,
                  std::string(*signal));
  }

  // 13. no-transform.
  if ((cc.cc_flags & Meta::kCCOriginNoTransform) != 0) {
    v.action = HtmlStoreAction::kStoreNoNotify;
    v.reason = HtmlStoreReason::kNoTransform;
  } else {
    v.action = HtmlStoreAction::kStore;
    v.reason = HtmlStoreReason::kNone;
  }
  return v;
}

// ---- serve ---------------------------------------------------------------

HtmlServeVerdict ClassifyHtmlForServe(const HtmlServeRequest& request,
                                      const StoredHtmlFacts& stored) {
  HtmlServeVerdict v;
  v.enforce = request.mode == HtmlCacheMode::kServeVariants ||
              request.mode == HtmlCacheMode::kServeCachedHtml;
  auto done = [&v](HtmlServeAction action, HtmlServeReason reason) {
    v.action = action;
    v.reason = reason;
    return v;
  };
  constexpr HtmlServeAction kFall = HtmlServeAction::kFallThrough;

  // 1. Mode.
  if (request.mode == HtmlCacheMode::kOff) {
    return done(kFall, HtmlServeReason::kMode);
  }
  // Report-only reports what the strongest mode would do.
  const bool cached_html = request.mode == HtmlCacheMode::kServeCachedHtml ||
                           request.mode == HtmlCacheMode::kReportOnly;

  // 2. Absent entry.
  if (!stored.present) return done(kFall, HtmlServeReason::kAbsent);

  const int64_t elapsed =
      std::max<int64_t>(0, static_cast<int64_t>(request.now_seconds) -
                               static_cast<int64_t>(stored.stored_at));
  v.age_s = SaturateU32(static_cast<int64_t>(stored.age_at_store) + elapsed);
  const int64_t age = v.age_s;

  // 3. Credentials.
  if (request.has_authorization || request.authenticated_user) {
    return done(kFall, HtmlServeReason::kAuthRequest);
  }
  // 4. Cookie request.
  if (request.has_cookie && !stored.explicit_shared) {
    return done(kFall, HtmlServeReason::kCookieRequest);
  }
  // 5. Client no-cache.
  if (request.cache_control_no_cache ||
      (request.pragma_no_cache && !request.has_cache_control)) {
    return done(kFall, HtmlServeReason::kClientNoCache);
  }
  // 6. Purged.
  if (stored.purged) return done(kFall, HtmlServeReason::kPurged);
  // 7. Header replay.
  if (stored.headers_not_reproducible) {
    return done(kFall, HtmlServeReason::kHeadersNotReproducible);
  }
  if (stored.sidecar_required && !stored.sidecar_present) {
    return done(kFall, HtmlServeReason::kSidecarMissing);
  }

  // 8. Freshness.  Stale entries are never served.
  const bool must_validate = stored.needs_validation || stored.lifetime_s == 0;
  bool fresh = !must_validate && age < static_cast<int64_t>(stored.lifetime_s);
  if (request.max_age >= 0 && age > request.max_age) fresh = false;
  if (request.min_fresh >= 0 &&
      age + request.min_fresh >= static_cast<int64_t>(stored.lifetime_s)) {
    fresh = false;
  }
  const bool parent_ok = fresh || stored.validated_this_request;
  if (!parent_ok) {
    return done(cached_html ? HtmlServeAction::kValidate : kFall,
                must_validate ? HtmlServeReason::kNeedsValidation
                              : HtmlServeReason::kStale);
  }

  // 9. Markdown.  A copy confirmed this request is bounded by its own
  // lifetime only, counted from confirmation.
  if (request.wants_markdown) {
    if (!stored.variant_present) return done(kFall, HtmlServeReason::kAbsent);
    if (!stored.binding_live)
      return done(kFall, HtmlServeReason::kBindingStale);
    const bool validated = stored.validated_this_request;
    const int64_t copy_age = validated ? 0 : age;
    const int64_t bound =
        validated ? static_cast<int64_t>(stored.render_ttl_s)
                  : std::min<int64_t>(stored.lifetime_s, stored.render_ttl_s);
    if (copy_age >= bound) return done(kFall, HtmlServeReason::kParentStale);
    return done(HtmlServeAction::kServeMarkdown, HtmlServeReason::kFresh);
  }

  // 10. HTML.
  if (cached_html) {
    return done(HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  }
  return done(kFall, HtmlServeReason::kMode);
}

}  // namespace pagespeed
