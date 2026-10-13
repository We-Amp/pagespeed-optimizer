// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/html_cache_decision.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "lib/cache/html_cache_policy.h"

namespace pagespeed {
namespace {

constexpr std::array<std::string_view, 19> kKeys = {
    "ts_ms",  "host",    "path",    "mode",       "action",
    "reason", "status",  "size",    "lifetime_s", "validators",
    "cc",     "vary",    "sidecar", "request",    "personalisation",
    "before", "variant", "binding", "age_s"};

// Keys whose value is a count, written as a JSON number.
bool IsNumericKey(std::string_view key) {
  return key == "ts_ms" || key == "status" || key == "size" ||
         key == "lifetime_s" || key == "age_s";
}

// Digits only, no leading zero, at most 20 digits (a uint64_t fits).
bool IsCanonicalInteger(std::string_view v) {
  if (v.empty() || v.size() > 20) return false;
  if (v.size() > 1 && v.front() == '0') return false;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// Index of `key` in the canonical order, or kKeys.size() for an unknown key.
size_t KeyIndex(std::string_view key) {
  for (size_t i = 0; i < kKeys.size(); ++i) {
    if (kKeys[i] == key) return i;
  }
  return kKeys.size();
}

// Every C0 control byte (0x00 to 0x1F) and DEL.  Refusing them, rather than
// escaping, keeps the line one line, keeps strict JSON parsers happy, and
// keeps the dedup separator (0x1F) out of every value.
bool HasForbiddenByte(std::string_view v) {
  for (char c : v) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= 0x1F || u == 0x7F) return true;
  }
  return false;
}

// ---- the value grammar, by key --------------------------------------------
//
// Fixed codes and counts only.  A value is a member of a closed name set, a
// canonical integer, a list of such, or (for a header name or a Vary token)
// an RFC 9110 token.  Nothing below admits free text.

bool IsAlnum(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
         (u >= 'A' && u <= 'Z');
}

// RFC 9110 section 5.6.2 tchar.
bool IsTchar(char c) {
  if (IsAlnum(c)) return true;
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

bool IsToken(std::string_view v) {
  if (v.empty()) return false;
  for (char c : v) {
    if (!IsTchar(c)) return false;
  }
  return true;
}

bool IsMember(std::string_view v, std::span<const std::string_view> set) {
  for (std::string_view s : set) {
    if (s == v) return true;
  }
  return false;
}

constexpr size_t kMaxHostBytes = 255;

// Hostname characters: a name, an address, an IPv6 literal, a port.
bool IsHost(std::string_view v) {
  if (v.empty() || v.size() > kMaxHostBytes) return false;
  for (char c : v) {
    if (!IsAlnum(c) && c != '.' && c != '-' && c != '_' && c != ':' &&
        c != '[' && c != ']') {
      return false;
    }
  }
  return true;
}

// The path the cache key was built from: bounded, and never a query string or
// a fragment, so a token in a query can never reach the line.
bool IsPath(std::string_view v) {
  return !v.empty() && v.size() <= kMaxHtmlCacheDecisionPathBytes &&
         v.find_first_of("?#") == std::string_view::npos;
}

bool IsMode(std::string_view v) {
  for (int i = 0; i <= static_cast<int>(HtmlCacheMode::kServeCachedHtml); ++i) {
    if (HtmlCacheModeName(static_cast<HtmlCacheMode>(i)) == v) return true;
  }
  return false;
}

// What a front end records happened, plus the policy's own verdict names, so
// a line can carry either the outcome or the verdict.
constexpr std::array<std::string_view, 9> kOutcomes = {
    "would_store",     "stored",    "skipped", "would_serve", "served_html",
    "served_markdown", "validated", "purged",  "invalidated"};

bool IsAction(std::string_view v) {
  if (IsMember(v, kOutcomes)) return true;
  for (int i = 0; i <= static_cast<int>(HtmlStoreAction::kStoreNoNotify); ++i) {
    if (HtmlStoreActionName(static_cast<HtmlStoreAction>(i)) == v) return true;
  }
  for (int i = 0; i <= static_cast<int>(HtmlServeAction::kValidate); ++i) {
    if (HtmlServeActionName(static_cast<HtmlServeAction>(i)) == v) return true;
  }
  return false;
}

// Reason codes a front end records that are not policy verdicts.
constexpr std::array<std::string_view, 4> kFrontEndReasons = {
    "validated", "validation_failed", "sidecar_write_failed", "invalidated"};

bool IsBareReason(std::string_view v) {
  for (uint8_t i = 0; i < kHtmlStoreReasonCount; ++i) {
    if (HtmlStoreReasonName(static_cast<HtmlStoreReason>(i)) == v) return true;
  }
  for (uint8_t i = 0; i < kHtmlServeReasonCount; ++i) {
    if (HtmlServeReasonName(static_cast<HtmlServeReason>(i)) == v) return true;
  }
  return IsMember(v, kFrontEndReasons);
}

// A bare code, or `headers:<name>` / `vary:<token>` as
// HtmlStoreVerdict::reason_code writes them: the detail is a header name or
// a Vary token, so it is a token, bounded as the verdict bounds it.
bool IsReason(std::string_view v) {
  const size_t colon = v.find(':');
  if (colon == std::string_view::npos) return IsBareReason(v);
  const std::string_view bare = v.substr(0, colon);
  const std::string_view detail = v.substr(colon + 1);
  if (bare != "headers" && bare != "vary") return false;
  return detail.size() <= kMaxHtmlStoreDetailBytes && IsToken(detail);
}

// `eligible_block`, `eligible_empty`, `fall_through`,
// `fall_through:<header-name>`, `never:nonce_csp`.
bool IsSidecar(std::string_view v) {
  if (v == "eligible_block" || v == "eligible_empty" || v == "fall_through" ||
      v == "never:nonce_csp") {
    return true;
  }
  constexpr std::string_view kPrefix = "fall_through:";
  if (v.substr(0, kPrefix.size()) != kPrefix) return false;
  const std::string_view name = v.substr(kPrefix.size());
  return name.size() <= kMaxHtmlStoreDetailBytes && IsToken(name);
}

// `none`, or a comma-separated subset of `names` in their order, each at
// most once: the fixed boolean grammar (a name present means true).
bool IsOrderedSubset(std::string_view v,
                     std::span<const std::string_view> names) {
  if (v == "none") return true;
  if (v.empty()) return false;
  size_t next = 0;
  while (true) {
    const size_t comma = v.find(',');
    const std::string_view item = v.substr(0, comma);
    size_t i = next;
    while (i < names.size() && names[i] != item) ++i;
    if (i == names.size()) return false;
    next = i + 1;
    if (comma == std::string_view::npos) return true;
    v = v.substr(comma + 1);
    if (v.empty()) return false;
  }
}

constexpr std::array<std::string_view, 2> kValidatorNames = {"etag",
                                                             "last_modified"};
constexpr std::array<std::string_view, 4> kRequestNames = {
    "authorization", "cookie", "no_cache", "accept_markdown"};

// The response directive names RFC 9111 section 5.2.2 and RFC 5861 define,
// plus `immutable` (RFC 8246); the four that take a number may carry one.
constexpr std::array<std::string_view, 12> kDirectives = {
    "immutable", "max-age",      "must-revalidate", "no-cache",
    "no-store",  "no-transform", "private",         "proxy-revalidate",
    "public",    "s-maxage",     "stale-if-error",  "stale-while-revalidate"};
constexpr std::array<std::string_view, 4> kNumericDirectives = {
    "max-age", "s-maxage", "stale-if-error", "stale-while-revalidate"};

// A comma-separated list of known directive names, each at most once, with
// `=<integer>` allowed only on the numeric ones.  Never a value a directive
// could carry as text.
bool IsCacheControl(std::string_view v) {
  if (v.empty()) return false;
  std::array<bool, kDirectives.size()> seen{};
  while (true) {
    const size_t comma = v.find(',');
    const std::string_view item = v.substr(0, comma);
    const size_t eq = item.find('=');
    const std::string_view name = item.substr(0, eq);
    size_t index = kDirectives.size();
    for (size_t i = 0; i < kDirectives.size(); ++i) {
      if (kDirectives[i] == name) index = i;
    }
    if (index == kDirectives.size() || seen[index]) return false;
    seen[index] = true;
    if (eq != std::string_view::npos &&
        (!IsMember(name, kNumericDirectives) ||
         !IsCanonicalInteger(item.substr(eq + 1)))) {
      return false;
    }
    if (comma == std::string_view::npos) return true;
    v = v.substr(comma + 1);
  }
}

// A comma-separated list of tokens.
bool IsVaryList(std::string_view v) {
  if (v.empty()) return false;
  while (true) {
    const size_t comma = v.find(',');
    if (!IsToken(v.substr(0, comma))) return false;
    if (comma == std::string_view::npos) return true;
    v = v.substr(comma + 1);
  }
}

// An FNV-1a hash as decimal or lowercase hex digits.
bool IsHash(std::string_view v) {
  if (v.empty() || v.size() > 20) return false;
  for (char c : v) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool ValueFits(std::string_view key, std::string_view value) {
  if (value.empty() || HasForbiddenByte(value)) return false;
  if (IsNumericKey(key)) return IsCanonicalInteger(value);
  if (key == "host") return IsHost(value);
  if (key == "path") return IsPath(value);
  if (key == "mode") return IsMode(value);
  if (key == "action") return IsAction(value);
  if (key == "reason") return IsReason(value);
  if (key == "validators") return IsOrderedSubset(value, kValidatorNames);
  if (key == "cc") return IsCacheControl(value);
  if (key == "vary") return IsVaryList(value);
  if (key == "sidecar") return IsSidecar(value);
  if (key == "request") return IsOrderedSubset(value, kRequestNames);
  if (key == "personalisation") {
    return IsMember(value, HtmlPersonalisationSignalNames());
  }
  if (key == "before") return IsHash(value);
  if (key == "variant") return value == "0x7C" || value == "markdown";
  if (key == "binding") return value == "live" || value == "stale";
  return false;
}

// Quote and backslash are escaped; bytes from 0x80 up are written as \u00XX
// (each byte as its own code point), so the line is pure ASCII and always
// valid JSON whatever bytes the caller held.  Only `path` can reach here with
// anything outside the grammar's ASCII; the rest is escaped all the same.
void AppendEscaped(std::string* out, std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (char c : value) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(c);
    } else if (u >= 0x80) {
      out->append("\\u00");
      out->push_back(kHex[u >> 4]);
      out->push_back(kHex[u & 0xF]);
    } else {
      out->push_back(c);
    }
  }
}

}  // namespace

std::span<const std::string_view> HtmlCacheDecisionKeys() { return kKeys; }

bool HtmlCacheDecisionValueFits(std::string_view key, std::string_view value) {
  return KeyIndex(key) != kKeys.size() && ValueFits(key, value);
}

std::optional<std::string> FormatHtmlCacheDecisionLine(
    std::span<const HtmlCacheDecisionField> fields) {
  // Slot per canonical key: where the value is, or nothing.
  std::array<std::optional<std::string_view>, kKeys.size()> slots;
  for (const HtmlCacheDecisionField& field : fields) {
    const size_t index = KeyIndex(field.key);
    if (index == kKeys.size()) return std::nullopt;
    if (slots[index].has_value()) return std::nullopt;
    if (!ValueFits(kKeys[index], field.value)) return std::nullopt;
    slots[index] = field.value;
  }

  std::string line;
  line.push_back('{');
  bool first = true;
  for (size_t i = 0; i < kKeys.size(); ++i) {
    if (!slots[i].has_value()) continue;
    if (!first) line.push_back(',');
    first = false;
    line.push_back('"');
    line.append(kKeys[i]);
    line.append("\":");
    const std::string_view value = *slots[i];
    if (IsNumericKey(kKeys[i])) {
      line.append(value);
    } else {
      line.push_back('"');
      AppendEscaped(&line, value);
      line.push_back('"');
    }
  }
  line.push_back('}');
  return line;
}

uint64_t HtmlCacheFnv1a64(std::string_view bytes) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (char c : bytes) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

std::string HtmlCacheDecisionDedupKey(
    std::span<const HtmlCacheDecisionField> fields) {
  static constexpr std::array<std::string_view, 7> kDedupKeys = {
      "host", "path", "mode", "action", "reason", "sidecar", "before"};
  std::string out;
  bool first = true;
  for (std::string_view key : kDedupKeys) {
    if (!first) out.push_back('\x1f');
    first = false;
    for (const HtmlCacheDecisionField& field : fields) {
      if (field.key == key) {
        out.append(field.value);
        break;
      }
    }
  }
  return out;
}

}  // namespace pagespeed
