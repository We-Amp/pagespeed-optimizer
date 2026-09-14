// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Scanner for Worker
//
// Read-only HTML element collector. Scans HTML to collect elements,
// stylesheet links, and inline CSS without modifying the HTML.
// This is used for Critical CSS extraction.

#ifndef PAGESPEED_SRC_WORKER_HTML_SCANNER_H_
#define PAGESPEED_SRC_WORKER_HTML_SCANNER_H_

#include <string>
#include <string_view>
#include <vector>

#include "lib/base/string_util.h"

namespace net_instaweb {
class HtmlElement;
}  // namespace net_instaweb

namespace pagespeed {

// Information about a collected HTML element
struct CollectedElement {
  std::string tag_name;
  std::string id;
  std::vector<std::string> classes;
  int depth = 0;
  int element_index = 0;
};

// Information about an external stylesheet link
struct StylesheetLink {
  std::string href;
  std::string media;  // media attribute if present
};

// True when a stylesheet's media attribute is exactly "print"
// (case-insensitive, whole-value).  Such a sheet is never render-blocking
// for screen, so it must not be preload-hinted.
inline bool StylesheetMediaIsPrint(std::string_view media) {
  return net_instaweb::StringCaseEqual(media, "print");
}

// Candidate Largest Contentful Paint image detected by heuristic.
struct LcpCandidate {
  std::string src;
  std::string srcset;  // if present
  std::string sizes;   // sizes attribute if present
  int element_index = -1;
  // True when the <img> is a direct child of <picture>.  A <source> sibling
  // may win source selection, so preloading the img's src risks a double
  // download — preload emitters must skip such candidates.  fetchpriority on
  // the <img> element itself stays correct whichever source wins.
  bool in_picture = false;
};

// True for elements whose markup declares them invisible: tiny explicit
// dimensions (0/1 px — tracking beacons and 0x0 tracking iframes), the
// hidden attribute, or an inline style that removes the element from
// rendering (display:none / visibility:hidden).  Evidence-based (attributes
// only), deliberately no URL patterns; absent evidence means visible, so an
// element with no size info and no hiding markers is NOT invisible.
bool IsInvisibleElement(const net_instaweb::HtmlElement& element);

// True for images that cannot plausibly be the LCP element.  Today this is
// exactly the invisibility evidence above: a false "unlikely" merely skips
// an optimization hint, while promoting a beacon wastes the browser's
// high-priority fetch slot.
bool IsUnlikelyLcpImage(const net_instaweb::HtmlElement& element);

// A third-party origin worth a preconnect hint.
struct PreconnectOrigin {
  std::string origin;
  // True when the resource that motivated this origin is fetched in CORS mode
  // (crossorigin attribute, ES module script, or a font preload).  Browsers
  // key connection reuse on the request mode, so a preconnect must warm the
  // SAME pool the resource will use: crossorigin for CORS-mode fetches, bare
  // for plain stylesheets/scripts/images.
  bool crossorigin = false;
};

// Result of scanning HTML
struct HtmlScanResult {
  bool success = false;
  std::string error_message;

  // Collected elements in document order
  std::vector<CollectedElement> elements;

  // External stylesheet links (<link rel="stylesheet">)
  std::vector<StylesheetLink> stylesheets;

  // Combined inline CSS from all <style> tags
  std::string inline_css;

  // LCP image candidate (empty src = no candidate found)
  LcpCandidate lcp_candidate;

  // Third-party origins found in the HTML (for preconnect hints).
  // Deduplicated, capped at 4, prioritized by resource type.  Each entry
  // records whether the motivating (first-seen) resource fetches in CORS
  // mode, so preconnect emitters warm the matching connection pool.
  std::vector<PreconnectOrigin> third_party_origins;

  // Raw URLs (href/src, unresolved) of subresources referenced with an
  // integrity attribute (SRI).  Optimized variants served at the same
  // URL would fail the browser's hash check, so these URLs must be
  // excluded from text-variant optimization.  Collected from <script src>
  // and any <link href> (stylesheet, preload, modulepreload).
  std::vector<std::string> integrity_pinned_urls;

  // Raw src attributes (unresolved) of <script src> elements, in document
  // order.  Feeds the analysis resource map (analysis_resource_map.cc).
  std::vector<std::string> script_srcs;

  // True when the document carries an author <base href> — the analysis
  // pipeline must not inject a second base element.
  bool has_base_href = false;

  // The author <base href> value (raw attribute, possibly relative).  Per the
  // HTML spec only the FIRST base-with-href sets the document base URL, so
  // later ones are ignored.  Empty when has_base_href is false.
  std::string base_href;
};

// HTML Scanner that collects element information without modification.
//
// This scanner uses HtmlParse with a custom filter to collect elements
// as they are encountered. Unlike HtmlRewriter, it does NOT use
// HtmlWriterFilter and does NOT produce modified HTML output.
//
// Usage:
//   HtmlScanner scanner;
//   HtmlScanResult result = scanner.Scan("http://example.com/", html);
//   if (result.success) {
//     // Use result.elements, result.stylesheets, result.inline_css
//   }
class HtmlScanner {
 public:
  HtmlScanner();
  ~HtmlScanner() = default;

  // Scan HTML content and collect element information.
  // url: The URL of the page (used for base URL resolution)
  // html: The HTML content to scan
  // Returns the scan result with collected elements and CSS
  HtmlScanResult Scan(std::string_view url, std::string_view html);
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_HTML_SCANNER_H_
