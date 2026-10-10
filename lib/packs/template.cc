// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/template.h"

#include <cstddef>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"

namespace pagespeed::packs {
namespace {

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

absl::StatusOr<Template> ParseTemplate(std::string_view text,
                                       const TemplateParseOptions& options) {
  Template tpl;
  tpl.source = std::string(text);
  std::string literal;

  auto flush_literal = [&]() {
    if (literal.empty()) return;
    TemplateSegment seg;
    seg.is_literal = true;
    seg.literal = std::move(literal);
    tpl.segments.push_back(std::move(seg));
    literal.clear();
  };

  size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '{') {
      literal.push_back(text[i++]);
      continue;
    }
    size_t j = i + 1;
    while (j < text.size() && IsIdentChar(text[j])) ++j;
    if (j == i + 1 || j >= text.size() || text[j] != '}') {
      literal.push_back(text[i++]);  // not a "{identifier}" token
      continue;
    }
    const std::string_view name = text.substr(i + 1, j - i - 1);

    TemplateSegment seg;
    seg.is_literal = false;
    bool page_value = false;
    if (name == "scheme") {
      seg.placeholder = Placeholder::kScheme;
    } else if (name == "host") {
      seg.placeholder = Placeholder::kHost;
    } else if (name == "path") {
      seg.placeholder = Placeholder::kPath;
    } else if (name == "query") {
      seg.placeholder = Placeholder::kQuery;
    } else if (name == "url") {
      seg.placeholder = Placeholder::kUrl;
    } else if (name.size() == 1 && name[0] >= '1' && name[0] <= '9') {
      seg.placeholder = Placeholder::kCapture;
      seg.capture = name[0] - '0';
      if (seg.capture > options.capture_groups) {
        return absl::InvalidArgumentError(absl::StrCat(
            "placeholder {", name, "} refers to capture group ", seg.capture,
            " but path_regex has ", options.capture_groups,
            options.capture_groups == 1 ? " group" : " groups"));
      }
    } else if (name == "title") {
      seg.placeholder = Placeholder::kTitle;
      page_value = true;
    } else if (name == "description") {
      seg.placeholder = Placeholder::kDescription;
      page_value = true;
    } else if (name == "canonical") {
      seg.placeholder = Placeholder::kCanonical;
      page_value = true;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("unknown placeholder {", name, "}"));
    }
    if (page_value && !options.allow_page_values) {
      return absl::InvalidArgumentError(
          absl::StrCat("placeholder {", name, "} is only allowed in jsonld"));
    }
    flush_literal();
    tpl.segments.push_back(std::move(seg));
    i = j + 1;
  }
  flush_literal();
  return tpl;
}

std::string EscapeHtmlText(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string EscapeJsonString(std::string_view s) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (u < 0x20 || u == 0x7f) {
          out += "\\u00";
          out.push_back(kHex[u >> 4]);
          out.push_back(kHex[u & 0xf]);
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

namespace {

std::string PlaceholderValue(const TemplateSegment& seg,
                             const ExpandContext& ctx) {
  switch (seg.placeholder) {
    case Placeholder::kScheme:
      return ctx.url.scheme;
    case Placeholder::kHost:
      return ctx.url.host;
    case Placeholder::kPath:
      return ctx.url.path;
    case Placeholder::kQuery:
      return ctx.url.query.empty() ? std::string() : "?" + ctx.url.query;
    case Placeholder::kUrl:
      return absl::StrCat(ctx.url.scheme, "://", ctx.url.host, ctx.url.path,
                          ctx.url.query.empty() ? "" : "?", ctx.url.query);
    case Placeholder::kCapture: {
      const size_t idx = static_cast<size_t>(seg.capture) - 1;
      return idx < ctx.captures.size() ? ctx.captures[idx] : std::string();
    }
    case Placeholder::kTitle:
      return ctx.title;
    case Placeholder::kDescription:
      return ctx.description;
    case Placeholder::kCanonical:
      return ctx.canonical;
  }
  return std::string();
}

}  // namespace

absl::StatusOr<std::string> ExpandTemplate(const Template& tpl,
                                           const ExpandContext& ctx,
                                           EscapeContext escape,
                                           size_t max_bytes) {
  std::string out;
  auto too_big = [&]() {
    return absl::ResourceExhaustedError(
        absl::StrCat("expanded value exceeds ", max_bytes, " bytes"));
  };
  for (const TemplateSegment& seg : tpl.segments) {
    if (seg.is_literal) {
      out += seg.literal;
    } else {
      std::string value = PlaceholderValue(seg, ctx);
      switch (escape) {
        case EscapeContext::kNone:
          break;
        case EscapeContext::kHtmlText:
          value = EscapeHtmlText(value);
          break;
        case EscapeContext::kJsonString:
          value = EscapeJsonString(value);
          break;
      }
      out += value;
    }
    // Every '<' can only grow under kJsonString (1 -> 6), so this is a lower
    // bound; the exact check follows the final pass.
    if (out.size() > max_bytes) return too_big();
  }
  if (escape == EscapeContext::kJsonString) {
    std::string final_out;
    final_out.reserve(out.size());
    for (char c : out) {
      if (c == '<') {
        final_out += "\\u003c";
      } else {
        final_out.push_back(c);
      }
    }
    out = std::move(final_out);
    if (out.size() > max_bytes) return too_big();
  }
  return out;
}

}  // namespace pagespeed::packs
