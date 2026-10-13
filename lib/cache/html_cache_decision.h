// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_HTML_CACHE_DECISION_H_
#define PAGESPEED_LIB_CACHE_HTML_CACHE_DECISION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pagespeed {

// The one-line record of an HTML cache decision (html_cache_policy.h): what
// was decided for which URL and why, as a single JSON object a log can carry.
//
// A FORMATTER ONLY.  It owns no file, opens no socket and reads no clock; the
// caller supplies every value, including the timestamp, and decides where the
// line goes and how often.  `HtmlCacheDecisionDedupKey` is the key a caller
// can use to avoid writing the same decision twice.
//
// PRIVACY IS A PROPERTY OF THE KEY SET.  The closed list of keys below has no
// key for a request or response body, a header value, a cookie, a client
// address or a user agent, and the formatter refuses any key outside the list.
// A caller therefore cannot add one by accident, and a reviewer can check the
// whole surface by reading HtmlCacheDecisionKeys().  `path` is the path the
// cache key was built from (key-safe paths carry no query string), and the
// formatter bounds it.

struct HtmlCacheDecisionField {
  std::string_view key;
  std::string_view value;
};

// The closed key set, in canonical output order:
//   ts_ms, host, path, mode, action, reason, status, size, lifetime_s,
//   validators, cc, vary, sidecar, request, personalisation, before, variant,
//   binding, age_s
std::span<const std::string_view> HtmlCacheDecisionKeys();

// Longest `path` value the formatter accepts, in bytes.  The caller truncates.
inline constexpr size_t kMaxHtmlCacheDecisionPathBytes = 300;

// One JSON object on one line, keys in canonical order, only the keys given,
// values escaped (quote and backslash with a backslash, bytes 0x80 and above as \u00XX).  The keys ts_ms, status, size,
// lifetime_s and age_s are emitted as numbers when the value is a canonical
// decimal integer (digits only, no leading zero, at most 20 digits), else as
// strings.
//
// Returns nullopt for: an unknown key; a duplicate key; a value containing
// any C0 control byte (0x00 to 0x1F) or DEL (0x7F); a `path` longer than kMaxHtmlCacheDecisionPathBytes; or a
// `host` that is empty.
std::optional<std::string> FormatHtmlCacheDecisionLine(
    std::span<const HtmlCacheDecisionField> fields);

// FNV-1a, 64 bit, over `bytes`: the `before` hash of stored bytes.  Not a
// security hash; it exists so a log line can say "the stored bytes changed"
// without carrying them.
uint64_t HtmlCacheFnv1a64(std::string_view bytes);

// The dedup key: host, path, mode, action, reason, sidecar and before, joined
// with '\x1f'.  A key that was not given contributes an empty component.
std::string HtmlCacheDecisionDedupKey(
    std::span<const HtmlCacheDecisionField> fields);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_HTML_CACHE_DECISION_H_
