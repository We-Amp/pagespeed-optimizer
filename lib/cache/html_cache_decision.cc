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

#include "lib/base/string_util.h"

namespace pagespeed {
namespace {

constexpr std::array<std::string_view, 19> kKeys = {
    "ts_ms",  "host",    "path",    "mode",       "action",
    "reason", "status",  "size",    "lifetime_s", "validators",
    "cc",     "vary",    "sidecar", "request",    "personalisation",
    "before", "variant", "binding", "age_s"};

// Keys whose value is a number when it is a canonical decimal integer.
bool IsNumericKey(std::string_view key) {
  return key == "ts_ms" || key == "status" || key == "size" ||
         key == "lifetime_s" || key == "age_s";
}

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

bool HasForbiddenByte(std::string_view v) {
  return v.find_first_of(std::string_view("\r\n\0", 3)) !=
         std::string_view::npos;
}

}  // namespace

std::span<const std::string_view> HtmlCacheDecisionKeys() { return kKeys; }

std::optional<std::string> FormatHtmlCacheDecisionLine(
    std::span<const HtmlCacheDecisionField> fields) {
  // Slot per canonical key: where the value is, or nothing.
  std::array<std::optional<std::string_view>, kKeys.size()> slots;
  for (const HtmlCacheDecisionField& field : fields) {
    const size_t index = KeyIndex(field.key);
    if (index == kKeys.size()) return std::nullopt;
    if (slots[index].has_value()) return std::nullopt;
    if (HasForbiddenByte(field.value)) return std::nullopt;
    if (kKeys[index] == "path" &&
        field.value.size() > kMaxHtmlCacheDecisionPathBytes) {
      return std::nullopt;
    }
    if (kKeys[index] == "host" && field.value.empty()) return std::nullopt;
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
    if (IsNumericKey(kKeys[i]) && IsCanonicalInteger(value)) {
      line.append(value);
    } else {
      line.push_back('"');
      line.append(net_instaweb::JsonEscapeMinimal(value));
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
