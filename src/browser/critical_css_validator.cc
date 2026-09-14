// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — Critical-CSS validator implementation.
//
// See the header for what this is for and what it deliberately cannot see.

#include "src/browser/critical_css_validator.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "lib/base/string_util.h"
#include "src/browser/visual_regression_gate.h"
#include "src/worker/html_css_injector.h"

namespace pagespeed {

using net_instaweb::LowerChar;

namespace {

constexpr std::string_view kStyleOpen = "<style data-pagespeed-critical>";
constexpr std::string_view kStyleClose = "</style>";

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerChar(a[i]) != LowerChar(b[i])) return false;
  }
  return true;
}

bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// html[pos] is '<'. True when it opens a <link ...> tag (word boundary
// enforced, so <linkbox> is not one).
bool StartsLinkTag(std::string_view html, size_t pos) {
  static constexpr std::string_view kLink = "link";
  size_t after = pos + 1 + kLink.size();
  if (after > html.size()) return false;
  if (!EqualsIgnoreCase(html.substr(pos + 1, kLink.size()), kLink)) {
    return false;
  }
  if (after == html.size()) return true;
  char next = html[after];
  return next == '>' || next == '/' || IsHtmlSpace(next);
}

// One past the '>' that closes the tag starting at `pos`, honouring quoted
// attribute values so `title="a>b"` does not end it early. npos when the tag is
// never closed.
size_t FindTagEnd(std::string_view html, size_t pos) {
  char quote = '\0';
  for (size_t i = pos + 1; i < html.size(); ++i) {
    char c = html[i];
    if (quote != '\0') {
      if (c == quote) quote = '\0';
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '>') return i + 1;
  }
  return std::string_view::npos;
}

// True when the tag text (`<link ...>`) declares rel="stylesheet".  Parsed
// rather than substring-matched: `href="/stylesheet.css"` on a rel=preload is
// not a stylesheet link, and `rel="alternate stylesheet"` is.
bool LinkTagIsStylesheet(std::string_view tag) {
  size_t i = 0;
  // Skip "<link".
  while (i < tag.size() && tag[i] != '>' && !IsHtmlSpace(tag[i])) ++i;

  while (i < tag.size()) {
    while (i < tag.size() && (IsHtmlSpace(tag[i]) || tag[i] == '/')) ++i;
    if (i >= tag.size() || tag[i] == '>') break;

    size_t name_start = i;
    while (i < tag.size() && !IsHtmlSpace(tag[i]) && tag[i] != '=' &&
           tag[i] != '>' && tag[i] != '/') {
      ++i;
    }
    std::string_view name = tag.substr(name_start, i - name_start);

    while (i < tag.size() && IsHtmlSpace(tag[i])) ++i;
    std::string_view value;
    if (i < tag.size() && tag[i] == '=') {
      ++i;
      while (i < tag.size() && IsHtmlSpace(tag[i])) ++i;
      if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
        char quote = tag[i++];
        size_t value_start = i;
        while (i < tag.size() && tag[i] != quote) ++i;
        value = tag.substr(value_start, i - value_start);
        if (i < tag.size()) ++i;  // closing quote
      } else {
        size_t value_start = i;
        while (i < tag.size() && !IsHtmlSpace(tag[i]) && tag[i] != '>') ++i;
        value = tag.substr(value_start, i - value_start);
      }
    }

    if (!EqualsIgnoreCase(name, "rel")) continue;

    // rel is a space-separated token list.
    size_t t = 0;
    while (t < value.size()) {
      while (t < value.size() && IsHtmlSpace(value[t])) ++t;
      size_t token_start = t;
      while (t < value.size() && !IsHtmlSpace(value[t])) ++t;
      if (EqualsIgnoreCase(value.substr(token_start, t - token_start),
                           "stylesheet")) {
        return true;
      }
    }
    return false;
  }
  return false;
}

struct StripResult {
  bool ok = false;
  std::string error;
  std::string html;
  size_t links_removed = 0;
  size_t styles_removed = 0;
};

// Remove every stylesheet <link> and every <style> element, leaving everything
// else — including markup that only LOOKS like one because it sits inside a
// comment or a <script> string — exactly where it was.
StripResult StripStylesheetSources(std::string_view html) {
  StripResult out;
  out.html.reserve(html.size());

  size_t pos = 0;
  while (pos < html.size()) {
    if (html_scan::StartsComment(html, pos)) {
      size_t end = html_scan::ScanComment(html, pos);
      if (end == std::string_view::npos) {
        out.error = "unterminated HTML comment";
        return out;
      }
      out.html.append(html.substr(pos, end - pos));
      pos = end;
      continue;
    }

    if (html[pos] == '<') {
      html_scan::RawTextElement rt = html_scan::ScanRawTextElement(html, pos);
      if (rt.matched) {
        if (!rt.closed) {
          // The rest of the document is inside it. Neither keeping nor dropping
          // the remainder produces a document that means anything.
          out.error = absl::StrCat("unterminated <", rt.tag, "> element");
          return out;
        }
        if (EqualsIgnoreCase(rt.tag, "style")) {
          ++out.styles_removed;
        } else {
          out.html.append(html.substr(pos, rt.end - pos));
        }
        pos = rt.end;
        continue;
      }

      if (StartsLinkTag(html, pos)) {
        size_t end = FindTagEnd(html, pos);
        if (end == std::string_view::npos) {
          out.error = "unterminated <link> tag";
          return out;
        }
        if (LinkTagIsStylesheet(html.substr(pos, end - pos))) {
          ++out.links_removed;
        } else {
          out.html.append(html.substr(pos, end - pos));
        }
        pos = end;
        continue;
      }
    }

    out.html.push_back(html[pos]);
    ++pos;
  }

  out.ok = true;
  return out;
}

ValidationDocuments Refuse(std::string error) {
  ValidationDocuments out;
  out.ok = false;
  out.error = std::move(error);
  return out;
}

ValidationVerdict NotValidated(std::string reason) {
  ValidationVerdict v;
  v.validated = false;
  v.diff_ratio = -1.0f;
  v.failure_reason = std::move(reason);
  return v;
}

}  // namespace

bool DocumentsDifferOnlyInStyleBody(std::string_view reference,
                                    std::string_view candidate) {
  size_t ref_open = reference.find(kStyleOpen);
  size_t cand_open = candidate.find(kStyleOpen);
  if (ref_open == std::string_view::npos ||
      cand_open == std::string_view::npos) {
    return false;
  }
  // Exactly one injected block per document, or "the body" is ambiguous.
  if (reference.find(kStyleOpen, ref_open + 1) != std::string_view::npos ||
      candidate.find(kStyleOpen, cand_open + 1) != std::string_view::npos) {
    return false;
  }
  if (reference.substr(0, ref_open) != candidate.substr(0, cand_open)) {
    return false;
  }
  size_t ref_close = reference.find(kStyleClose, ref_open + kStyleOpen.size());
  size_t cand_close =
      candidate.find(kStyleClose, cand_open + kStyleOpen.size());
  if (ref_close == std::string_view::npos ||
      cand_close == std::string_view::npos) {
    return false;
  }
  return reference.substr(ref_close) == candidate.substr(cand_close);
}

ValidationDocuments BuildValidationDocuments(std::string_view pre_inline_html,
                                             std::string_view full_css,
                                             std::string_view critical_css,
                                             size_t max_document_bytes) {
  if (pre_inline_html.empty()) return Refuse("empty page markup");
  // An empty reference stylesheet renders the same unstyled fold as an empty
  // candidate: the diff would be zero and the page would validate on nothing.
  if (full_css.empty())
    return Refuse("no combined stylesheet to validate against");
  if (critical_css.empty()) return Refuse("no critical block to validate");

  StripResult stripped = StripStylesheetSources(pre_inline_html);
  if (!stripped.ok) return Refuse(std::move(stripped.error));

  const size_t largest_css = full_css.size() > critical_css.size()
                                 ? full_css.size()
                                 : critical_css.size();
  if (stripped.html.size() + largest_css + kStyleOpen.size() +
          kStyleClose.size() >
      max_document_bytes) {
    return Refuse("validation document too large to render");
  }

  CssInjectionResult reference = InjectCriticalCss(stripped.html, full_css);
  CssInjectionResult candidate = InjectCriticalCss(stripped.html, critical_css);

  // The injector reports its `</style` refusal as success=TRUE with an EMPTY
  // document (html_css_injector.cc). Checking `success` alone would synthesize
  // a blank page and diff it against a real one.
  for (const auto* r : {&reference, &candidate}) {
    if (!r->success || !r->injected || r->html.empty()) {
      return Refuse(absl::StrCat("critical CSS injection refused: ",
                                 r->error_message.empty()
                                     ? "no injection performed"
                                     : r->error_message));
    }
  }

  if (!DocumentsDifferOnlyInStyleBody(reference.html, candidate.html)) {
    return Refuse(
        "reference and candidate differ outside the injected style body");
  }

  ValidationDocuments out;
  out.ok = true;
  out.reference = std::move(reference.html);
  out.candidate = std::move(candidate.html);
  out.stylesheet_links_removed = stripped.links_removed;
  out.style_blocks_removed = stripped.styles_removed;
  return out;
}

void ValidateCriticalCss(VisualRegressionGate* gate,
                         const ValidationDocuments& docs,
                         uint32_t viewport_width, uint32_t viewport_height,
                         float threshold,
                         std::function<void(ValidationVerdict)> callback) {
  if (!docs.ok) {
    callback(NotValidated(
        absl::StrCat("no comparable documents: ",
                     docs.error.empty() ? "not built" : docs.error)));
    return;
  }
  if (gate == nullptr) {
    callback(NotValidated("no browser available to validate against"));
    return;
  }

  // The gate is BORROWED, and deliberately NOT kept alive by this callback.
  // Its owner tears it down together with the CDP client it was built on; a
  // validation that outlived that teardown would hold a freed client through a
  // 60 s timer, and once the client is gone that timer is the only thing left
  // that can complete the capture. Cancellation arrives here as a refusal,
  // which is the correct answer: a browser that went away confirmed nothing.
  gate->Compare(
      docs.reference, docs.candidate, viewport_width, viewport_height,
      [callback =
           std::move(callback)](absl::StatusOr<RegressionResult> result) {
        if (!result.ok()) {
          callback(NotValidated(
              absl::StrCat("validation render failed: ",
                           std::string(result.status().message()))));
          return;
        }
        // "Nothing differed because nothing was compared" is not a
        // measurement. CompareScreenshots reports passed=true with
        // total_pixels==0 for a degenerate compare region.
        if (result->total_pixels == 0) {
          callback(NotValidated("validation compared no pixels"));
          return;
        }
        ValidationVerdict verdict;
        verdict.diff_ratio = result->diff_ratio;
        verdict.validated = result->passed;
        if (!verdict.validated) {
          verdict.failure_reason =
              "above-the-fold appearance changed beyond the allowed threshold";
        }
        callback(std::move(verdict));
      },
      threshold);
}

}  // namespace pagespeed
