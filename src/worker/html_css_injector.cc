// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Critical CSS Injector Implementation

#include "src/worker/html_css_injector.h"

#include <string>
#include <string_view>

#include "lib/base/string_util.h"

namespace pagespeed {

using net_instaweb::LowerChar;

namespace {

// Case-insensitive comparison of two string segments.
bool CaseInsensitiveEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerChar(a[i]) != LowerChar(b[i])) {
      return false;
    }
  }
  return true;
}

// Find a case-insensitive tag (like "<head" or "<head>") in the HTML.
// Returns position of the end of the tag (after the '>').
// Enforces word boundary: <head> matches but <header> does not.
size_t FindOpenTagEnd(std::string_view html, std::string_view tag_name) {
  for (size_t i = 0; i + tag_name.size() + 1 <= html.size(); ++i) {
    if (html[i] == '<' &&
        CaseInsensitiveEqual(html.substr(i + 1, tag_name.size()), tag_name)) {
      // Verify word boundary after tag name.
      size_t after_name = i + 1 + tag_name.size();
      if (after_name < html.size()) {
        char next = html[after_name];
        if (next != '>' && next != ' ' && next != '\t' && next != '\n' &&
            next != '\r' && next != '/') {
          continue;
        }
      }
      // Found "<tag_name" — now find the closing '>'.
      size_t close = html.find('>', after_name);
      if (close != std::string_view::npos) {
        return close + 1;
      }
    }
  }
  return std::string_view::npos;
}

// Build the style tag with critical CSS.
// Returns empty string if the CSS contains a style-closing sequence
// (XSS prevention: attacker-controlled CSS must not break out of the
// <style> element).
std::string BuildStyleTag(std::string_view critical_css) {
  // Strip null bytes.
  std::string sanitized;
  sanitized.reserve(critical_css.size());
  for (char c : critical_css) {
    if (c != '\0') {
      sanitized.push_back(c);
    }
  }

  // Abort if CSS contains </style (case-insensitive).  Without the
  // closing '>' requirement, this also catches </style\t, </style ,
  // and other variants that browsers accept as closing the element.
  for (size_t i = 0; i + 7 <= sanitized.size(); ++i) {
    if (CaseInsensitiveEqual(std::string_view(sanitized.data() + i, 7),
                             "</style")) {
      return {};
    }
  }

  std::string tag;
  tag.reserve(sanitized.size() + 50);
  tag.append("<style data-pagespeed-critical>");
  tag.append(sanitized);
  tag.append("</style>");
  return tag;
}

}  // namespace

namespace html_scan {

RawTextElement ScanRawTextElement(std::string_view html, size_t pos) {
  RawTextElement out;
  if (pos >= html.size() || html[pos] != '<') return out;

  static constexpr std::string_view kRawTextTags[] = {
      "script", "style", "textarea", "title", "xmp"};

  for (const auto& tag : kRawTextTags) {
    size_t tag_end = pos + 1 + tag.size();
    if (tag_end > html.size()) continue;
    if (!CaseInsensitiveEqual(html.substr(pos + 1, tag.size()), tag)) {
      continue;
    }

    // Verify word boundary after tag name, so <styles> is not <style>.
    if (tag_end < html.size()) {
      char next = html[tag_end];
      if (next != '>' && next != ' ' && next != '\t' && next != '\n' &&
          next != '\r' && next != '/') {
        continue;
      }
    }

    out.matched = true;
    out.tag = tag;

    // Find the case-insensitive closing tag.
    for (size_t j = tag_end; j + 2 + tag.size() <= html.size(); ++j) {
      if (html[j] == '<' && html[j + 1] == '/' &&
          CaseInsensitiveEqual(html.substr(j + 2, tag.size()), tag)) {
        size_t close_end = html.find('>', j + 2 + tag.size());
        if (close_end == std::string_view::npos) return out;  // unterminated
        out.closed = true;
        out.end = close_end + 1;
        return out;
      }
    }

    // Unclosed raw text element — everything after it is inside it.
    return out;
  }

  return out;
}

bool StartsComment(std::string_view html, size_t pos) {
  return pos + 3 < html.size() && html[pos] == '<' && html[pos + 1] == '!' &&
         html[pos + 2] == '-' && html[pos + 3] == '-';
}

size_t ScanComment(std::string_view html, size_t pos) {
  size_t end = html.find("-->", pos + 4);
  if (end == std::string_view::npos) return std::string_view::npos;
  return end + 3;
}

size_t FindOutsideCommentsAndRawText(std::string_view html,
                                     std::string_view needle) {
  size_t pos = 0;
  while (pos + needle.size() <= html.size()) {
    // Skip HTML comments.
    if (StartsComment(html, pos)) {
      size_t end = ScanComment(html, pos);
      if (end == std::string_view::npos) return std::string_view::npos;
      pos = end;
      continue;
    }

    // Skip raw text elements.  An UNCLOSED one is deliberately not skipped
    // here: this scanner's job is to find an injection point, and refusing to
    // scan the rest of a document because it contains an unclosed <script>
    // would refuse injection on pages browsers render fine.
    if (html[pos] == '<') {
      RawTextElement rt = ScanRawTextElement(html, pos);
      if (rt.matched && rt.closed) {
        pos = rt.end;
        continue;
      }
    }

    // Check for needle match.
    if (CaseInsensitiveEqual(html.substr(pos, needle.size()), needle)) {
      return pos;
    }

    ++pos;
  }
  return std::string_view::npos;
}

}  // namespace html_scan

CssInjectionResult InjectCriticalCss(std::string_view html,
                                     std::string_view critical_css) {
  if (html.empty()) {
    return {false, "Empty HTML input", {}, false};
  }

  if (critical_css.empty()) {
    return {true, {}, std::string(html), false};
  }

  std::string style_tag = BuildStyleTag(critical_css);
  if (style_tag.empty()) {
    return {
        true, "CSS contains </style sequence; injection aborted", {}, false};
  }

  // Strategy 1: Insert before </head>, skipping matches inside
  // comments and raw text elements (script, style, etc.).
  size_t pos = html_scan::FindOutsideCommentsAndRawText(html, "</head>");
  if (pos != std::string_view::npos) {
    std::string result;
    result.reserve(html.size() + style_tag.size());
    result.append(html.substr(0, pos));
    result.append(style_tag);
    result.append(html.substr(pos));
    return {true, {}, std::move(result), true};
  }

  // Strategy 2: Insert before </body>.
  pos = html_scan::FindOutsideCommentsAndRawText(html, "</body>");
  if (pos != std::string_view::npos) {
    std::string result;
    result.reserve(html.size() + style_tag.size());
    result.append(html.substr(0, pos));
    result.append(style_tag);
    result.append(html.substr(pos));
    return {true, {}, std::move(result), true};
  }

  // Strategy 3: Insert after <head> or <head ...>.
  pos = FindOpenTagEnd(html, "head");
  if (pos != std::string_view::npos) {
    std::string result;
    result.reserve(html.size() + style_tag.size());
    result.append(html.substr(0, pos));
    result.append(style_tag);
    result.append(html.substr(pos));
    return {true, {}, std::move(result), true};
  }

  // Strategy 4: Insert at document start.
  std::string result;
  result.reserve(html.size() + style_tag.size());
  result.append(style_tag);
  result.append(html);
  return {true, {}, std::move(result), true};
}

}  // namespace pagespeed
