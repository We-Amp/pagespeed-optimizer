// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/markdown_coding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace pagespeed {

namespace {

bool IsOws(char c) { return c == ' ' || c == '\t'; }

std::string_view TrimOws(std::string_view s) {
  while (!s.empty() && IsOws(s.front())) s.remove_prefix(1);
  while (!s.empty() && IsOws(s.back())) s.remove_suffix(1);
  return s;
}

// RFC 9110 section 5.6.2 tchar.
bool IsTchar(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
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

bool IsToken(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), IsTchar);
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x | 0x20);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y | 0x20);
    if (x != y) return false;
  }
  return true;
}

// RFC 9110 section 12.4.2: qvalue = ( "0" [ "." 0*3DIGIT ] )
//                                 / ( "1" [ "." 0*3("0") ] ).
// In thousandths, or nullopt when it does not parse.
std::optional<int> ParseQvalue(std::string_view v) {
  if (v.empty() || (v[0] != '0' && v[0] != '1')) return std::nullopt;
  const bool one = v[0] == '1';
  if (v.size() == 1) return one ? 1000 : 0;
  if (v[1] != '.' || v.size() > 5) return std::nullopt;
  int frac = 0;
  int digits = 0;
  for (size_t i = 2; i < v.size(); ++i) {
    if (v[i] < '0' || v[i] > '9') return std::nullopt;
    if (one && v[i] != '0') return std::nullopt;
    frac = frac * 10 + (v[i] - '0');
    ++digits;
  }
  for (; digits < 3; ++digits) frac *= 10;
  return one ? 1000 : frac;
}

// What one element names, or nullopt for an element that is dropped.
struct Element {
  std::string_view coding;
  int q = 1000;
};

std::optional<Element> ParseElement(std::string_view item) {
  const size_t semi = item.find(';');
  Element e;
  e.coding = TrimOws(item.substr(0, semi));
  if (!IsToken(e.coding)) return std::nullopt;
  if (semi == std::string_view::npos) return e;
  std::string_view params = item.substr(semi + 1);
  while (true) {
    const size_t next = params.find(';');
    const std::string_view param = TrimOws(params.substr(0, next));
    const size_t eq = param.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    const std::string_view name = TrimOws(param.substr(0, eq));
    const std::string_view value = TrimOws(param.substr(eq + 1));
    if (EqualsIgnoreCase(name, "q")) {
      const std::optional<int> q = ParseQvalue(value);
      if (!q.has_value()) return std::nullopt;
      e.q = *q;
    } else if (!IsToken(name)) {
      return std::nullopt;
    }
    if (next == std::string_view::npos) break;
    params.remove_prefix(next + 1);
  }
  return e;
}

// Slot order is the tie-break order: br, gzip, identity.
constexpr MarkdownCoding kTieOrder[kMarkdownCodingCount] = {
    MarkdownCoding::kBrotli, MarkdownCoding::kGzip, MarkdownCoding::kIdentity};

std::optional<size_t> SlotOf(std::string_view coding) {
  if (EqualsIgnoreCase(coding, "br")) return 0;
  if (EqualsIgnoreCase(coding, "gzip") || EqualsIgnoreCase(coding, "x-gzip")) {
    return 1;
  }
  if (EqualsIgnoreCase(coding, "identity")) return 2;
  return std::nullopt;
}

}  // namespace

std::string_view MarkdownCodingName(MarkdownCoding coding) {
  switch (coding) {
    case MarkdownCoding::kGzip:
      return "gzip";
    case MarkdownCoding::kBrotli:
      return "br";
    case MarkdownCoding::kIdentity:
      break;
  }
  return "identity";
}

std::optional<MarkdownCoding> MarkdownCodingOfCodedId(AlternateId id) {
  if (id == MarkdownCopyId(MarkdownCoding::kGzip)) return MarkdownCoding::kGzip;
  if (id == MarkdownCopyId(MarkdownCoding::kBrotli)) {
    return MarkdownCoding::kBrotli;
  }
  return std::nullopt;
}

MarkdownCodingOrder SelectMarkdownCoding(
    std::span<const std::string_view> accept_encoding_lines) {
  std::optional<int> named[kMarkdownCodingCount];
  std::optional<int> star;
  for (std::string_view line : accept_encoding_lines) {
    std::string_view rest = line;
    while (true) {
      const size_t comma = rest.find(',');
      const std::string_view item = TrimOws(rest.substr(0, comma));
      if (!item.empty()) {
        if (const std::optional<Element> e = ParseElement(item)) {
          if (e->coding == "*") {
            star = star.has_value() ? std::min(*star, e->q) : e->q;
          } else if (const std::optional<size_t> slot = SlotOf(e->coding)) {
            std::optional<int>& q = named[*slot];
            q = q.has_value() ? std::min(*q, e->q) : e->q;
          }
        }
      }
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }

  // Effective weight per slot; -1 is not acceptable.  Identity that nothing
  // names stays acceptable at weight 0, which sorts it after every coding
  // the request asked for.
  int weight[kMarkdownCodingCount];
  for (size_t i = 0; i < kMarkdownCodingCount; ++i) {
    const std::optional<int> q = named[i].has_value() ? named[i] : star;
    if (q.has_value()) {
      weight[i] = *q > 0 ? *q : -1;
    } else {
      weight[i] = kTieOrder[i] == MarkdownCoding::kIdentity ? 0 : -1;
    }
  }

  MarkdownCodingOrder out;
  size_t slots[kMarkdownCodingCount] = {0, 1, 2};
  std::stable_sort(
      std::begin(slots), std::end(slots),
      [&weight](size_t a, size_t b) { return weight[a] > weight[b]; });
  for (size_t slot : slots) {
    if (weight[slot] < 0) continue;
    out.order[out.count++] = kTieOrder[slot];
  }
  return out;
}

}  // namespace pagespeed
