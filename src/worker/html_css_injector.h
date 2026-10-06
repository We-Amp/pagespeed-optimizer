// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// cache variant.

#ifndef PAGESPEED_SRC_WORKER_HTML_CSS_INJECTOR_H_
#define PAGESPEED_SRC_WORKER_HTML_CSS_INJECTOR_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace pagespeed {

// The HTML scanning this file needs in order to inject safely: skipping
// comments and raw-text elements so a `</head>` written inside a comment or a
// `<script>` string is not mistaken for the real one.
//
// Exposed because the critical-CSS validator has to strip the very elements
// this scanner already knows how to recognise, and a second scanner would
// disagree with this one about where a `<style>` ends — which is exactly the
// kind of drift that produces a document nobody meant to render.
namespace html_scan {

// Raw-text elements: everything between the tags is text, not markup.
struct RawTextElement {
  // html[pos] opens one of script/style/textarea/title/xmp.
  bool matched = false;
  // Its closing tag was found. When false the element runs to end-of-input.
  bool closed = false;
  // One past the '>' of the closing tag. Only meaningful when `closed`.
  size_t end = 0;
  // The tag name, lowercase, borrowed from a static table.
  std::string_view tag;
};

// Scan a raw-text element starting at html[pos] (which must be '<').
RawTextElement ScanRawTextElement(std::string_view html, size_t pos);

// True when html[pos] opens an HTML comment.
bool StartsComment(std::string_view html, size_t pos);

// One past the "-->" closing the comment opened at html[pos], or npos when the
// comment is never closed. This is the serve path's HtmlLexer reading, kept
// for placement parity: unlike a browser, it runs `<!-->` and
// `<!--->` on to the next "-->" and does not end a comment at `--!>`.
size_t ScanComment(std::string_view html, size_t pos);

// Position of the first case-insensitive `needle` that is NOT inside a comment
// or a raw-text element, or npos.
size_t FindOutsideCommentsAndRawText(std::string_view html,
                                     std::string_view needle);

}  // namespace html_scan

// Result of CSS injection into HTML.
struct CssInjectionResult {
  bool success = false;
  std::string error_message;
  // The modified HTML with critical CSS injected. Empty on failure
  // or if no injection was needed (empty CSS).
  std::string html;
  // True if the CSS was actually injected (not just returned unchanged).
  bool injected = false;
};

// Inject critical CSS into HTML as a <style data-pagespeed-critical> tag.
//
// Injection points (tried in order):
// 1. Before </head> (preferred, skipping </head> inside HTML comments)
// 2. Before </body> (fallback)
// 3. After <head> or <head ...> (fallback)
// 4. At document start (last resort)
//
// If critical_css is empty, returns the original HTML unchanged with
// injected=false.
//
// If html is empty, returns failure.
//
// This string-level injector only knows the </head> / </body> order. The
// SERVE path is HtmlTransformFilter::InjectCriticalCss, which places the block
// before the page's first stylesheet source when that source precedes </head>;
// the critical-CSS validator reproduces that placement with
// CriticalCssFallbackOffset + InjectCriticalCssAt below.
CssInjectionResult InjectCriticalCss(std::string_view html,
                                     std::string_view critical_css);

// The offset InjectCriticalCss injects at: before the first </head> outside
// comments and raw-text elements, else before such a </body>, else just after
// the <head ...> open tag, else 0.
size_t CriticalCssFallbackOffset(std::string_view html);

// True when `css` may name a cascade layer: an `@layer <name>` block or an
// `@layer a, b;` statement (anything after `@layer` other than an opening
// brace), or an `@import ... layer` rule. Matching is textual and
// case-insensitive, and deliberately over-inclusive — a mention inside a
// comment or a string counts too — because the answer only ever makes the
// block's placement MORE conservative.
//
// Why placement cares: a layer's position in the cascade is
// fixed by its first mention in the document. A block placed before the
// stylesheets mentions its layers first, in the COMBINED sheet's order (inline
// <style> bodies first, then external sheets that were fetched), which is not
// the document order when a <style> follows a <link>, and is incomplete when a
// layered sheet is missing from the combined sheet (a cross-origin sheet, a
// cold cache). So a block that names a layer goes first only behind the
// page's proven layer order; this function is the first question
// DecideCriticalCssLayerPlacement (cascade_layer_order.h) asks, and that is
// what HtmlTransformFilter::InjectCriticalCss and the validator's placement
// both apply.
bool CriticalCssNamesCascadeLayer(std::string_view css);

// As InjectCriticalCss, but at byte offset `pos` of `html` (0 <= pos <=
// html.size()), with the same sanitization and the same `</style` refusal
// (success=true, injected=false, empty html). Fails when `pos` is past the
// end. The caller owns the choice of position: this does no scanning.
CssInjectionResult InjectCriticalCssAt(std::string_view html, size_t pos,
                                       std::string_view critical_css);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_HTML_CSS_INJECTOR_H_
