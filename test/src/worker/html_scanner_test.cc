// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test for the HTML Scanner

#include "src/worker/html_scanner.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(HtmlScannerTest, ScanSimpleHtml) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/", "<html><body>Hello</body></html>");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.error_message.empty());
  ASSERT_EQ(2u, result.elements.size());

  EXPECT_EQ("html", result.elements[0].tag_name);
  EXPECT_EQ(0, result.elements[0].depth);
  EXPECT_EQ(0, result.elements[0].element_index);

  EXPECT_EQ("body", result.elements[1].tag_name);
  EXPECT_EQ(1, result.elements[1].depth);
  EXPECT_EQ(1, result.elements[1].element_index);
}

TEST(HtmlScannerTest, ScanEmptyHtml) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan("http://example.com/", "");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.elements.empty());
}

TEST(HtmlScannerTest, CollectsElementId) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/", "<div id=\"main\">Content</div>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  EXPECT_EQ("div", result.elements[0].tag_name);
  EXPECT_EQ("main", result.elements[0].id);
}

TEST(HtmlScannerTest, CollectsElementClasses) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<div class=\"container hero dark-mode\">Content</div>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  ASSERT_EQ(3u, result.elements[0].classes.size());
  EXPECT_EQ("container", result.elements[0].classes[0]);
  EXPECT_EQ("hero", result.elements[0].classes[1]);
  EXPECT_EQ("dark-mode", result.elements[0].classes[2]);
}

TEST(HtmlScannerTest, CollectsStylesheetLinks) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/css/main.css\">"
      "<link rel=\"stylesheet\" href=\"/css/print.css\" media=\"print\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(2u, result.stylesheets.size());

  EXPECT_EQ("/css/main.css", result.stylesheets[0].href);
  EXPECT_TRUE(result.stylesheets[0].media.empty());

  EXPECT_EQ("/css/print.css", result.stylesheets[1].href);
  EXPECT_EQ("print", result.stylesheets[1].media);
}

// Regression (duplicate preload / combined_css double / template-hash flap):
// on a revalidation pass the worker re-scans its OWN optimized output, where the
// async-CSS transform has split one origin <link rel="stylesheet"> into a
// deferred primary plus a <noscript> fallback copy of the SAME href. The
// scanner must treat the pair as the single logical origin stylesheet — the
// fallback is the worker's own artifact (marked data-pagespeed-async-fallback),
// not a second origin sheet. Collecting both doubles the preload Early-Hints,
// doubles combined_css gathering, and flaps the template hash (stylesheet count
// 1<->2 between the raw-origin and reprocessed passes). Mirrors the existing
// data-pagespeed-critical <style> skip.
TEST(HtmlScannerTest, SkipsAsyncCssFallbackLink) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
      "data-pagespeed-media=\"screen\" data-pagespeed-async=\"\">"
      "<noscript data-pagespeed-async-fallback=\"\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" "
      "data-pagespeed-async-fallback=\"\"></noscript>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // Exactly one logical stylesheet: the primary. The <noscript> fallback copy
  // must NOT be collected as a second origin sheet.
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("/a.css", result.stylesheets[0].href);
  // Both links share href=/a.css, so the surviving entry must be disambiguated:
  // the kept sheet is the deferred primary, not the fallback — proving the
  // guard skipped the fallback and kept the primary. The primary is
  // rel="preload" (not rel="stylesheet"), which is exactly why matching on
  // rel alone would drop it and leave the page with ZERO scanned stylesheets.
  // Its reported media is the RECORDED one: a deferred link carries no live
  // media attribute, and media-keyed decisions downstream (the Early-Hints
  // print skip) must see the author's value on this pass too.
  EXPECT_EQ("screen", result.stylesheets[0].media);
}

// The deferred primary is matched on rel="preload" + data-pagespeed-async,
// NOT on rel="preload" alone. The page is full of other preloads — the LCP
// image, fonts, the worker's own injected hints — and collecting any of them
// as a stylesheet would feed a non-CSS URL into combined_css gathering and
// into the stylesheet Early-Hints list.
TEST(HtmlScannerTest, OrdinaryPreloadsAreNotStylesheets) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"preload\" as=\"image\" href=\"/hero.jpg\">"
      "<link rel=\"preload\" as=\"font\" href=\"/f.woff2\" crossorigin>"
      "<link rel=\"preload\" as=\"style\" href=\"/not-ours.css\">"
      "<link rel=\"preload\" as=\"style\" href=\"/ours.css\" "
      "data-pagespeed-media=\"all\" data-pagespeed-async=\"\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  ASSERT_TRUE(result.success);
  // Only the one carrying our marker. An author's own hand-written
  // `rel=preload as=style` is a hint about a sheet declared elsewhere, not a
  // stylesheet declaration — treating it as one would double-count the sheet.
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("/ours.css", result.stylesheets[0].href);
}

// Regression (template-hash element-tree flap): on a revalidation pass the
// worker re-scans its OWN optimized output, which carries nodes the raw-origin
// HTML never had — the injected critical <style data-pagespeed-critical>, the
// async <noscript data-pagespeed-async-fallback> + its child <link>, the
// async-loader <script data-pagespeed-async-loader>, and preconnect/preload
// <link data-pagespeed-hint>. TemplateDetector::HashStructure hashes the full
// element tree, so collecting these extra nodes flaps the template hash between
// the raw and reprocessed passes (a spurious re-analysis + duplicate profile).
// The scanner must exclude its OWN injected markers from result.elements so the
// element list is a fixed point. Original customer nodes that merely carry a
// worker marker (data-pagespeed-async / -media on the deferred primary
// <link>, data-pagespeed-defer on a real <script>) are NOT injected and MUST be
// kept — they exist in the raw scan too.
TEST(HtmlScannerTest, ExcludesWorkerInjectedNodesFromElements) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      // Original customer <link>, async-swapped in place (KEEP).
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
      "data-pagespeed-media=\"screen\" data-pagespeed-async=\"\">"
      // Injected critical CSS (SKIP).
      "<style data-pagespeed-critical=\"\">.x{color:red}</style>"
      // Injected async <noscript> + child fallback <link> (SKIP both).
      "<noscript data-pagespeed-async-fallback=\"\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" "
      "data-pagespeed-async-fallback=\"\"></noscript>"
      // Injected async-CSS loader <script> (SKIP).
      "<script src=\"/pagespeed_static/async_css.js\" defer "
      "data-pagespeed-async-loader=\"\"></script>"
      // Injected preconnect hint on a <link> (SKIP).
      "<link rel=\"preconnect\" href=\"https://cdn.example.com\" "
      "data-pagespeed-hint=\"\">"
      "</head><body>"
      "<h1>Hi</h1>"
      "<p>x</p>"
      // Injected speculation-rules hint — the SAME data-pagespeed-hint marker
      // as the <link> hints, but on a <script> and sitting in <body> (not
      // <head>). The skip is tag-agnostic, so this must be dropped too; the
      // deferred customer <script> after it must keep its contiguous index,
      // which also exercises the depth balance for an injected node mid-body.
      "<script type=\"speculationrules\" data-pagespeed-hint=\"\">"
      "{\"prefetch\":[{\"source\":\"list\",\"urls\":[\"/next\"]}]}</script>"
      // Original customer <script>, deferred in place (KEEP).
      "<script src=\"/app.js\" data-pagespeed-defer=\"\"></script>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  ASSERT_TRUE(result.success);

  // Only the 7 original-document elements survive; all 6 injected nodes (style,
  // noscript, fallback link, loader script, hint <link>, hint <script>) drop.
  std::vector<std::string> tags;
  tags.reserve(result.elements.size());
  for (const auto& elem : result.elements) {
    tags.push_back(elem.tag_name);
  }
  EXPECT_EQ(tags, (std::vector<std::string>{"html", "head", "link", "body",
                                            "h1", "p", "script"}));

  // The kept <link> is the deferred primary (proves we skipped only the
  // injected markers, not the original element that carries -async/-media).
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("screen", result.stylesheets[0].media);

  // element_index must stay contiguous (0..N-1): skipped injected nodes must
  // NOT advance the counter, or real-element indices would shift on the
  // reprocessed pass and CriticalCssExtractor's "first N elements" cutoff
  // (element_index < max_elements) would no longer be a fixed point.
  for (size_t i = 0; i < result.elements.size(); ++i) {
    EXPECT_EQ(static_cast<int>(i), result.elements[i].element_index)
        << "at position " << i;
  }
}

TEST(HtmlScannerTest, IgnoresNonStylesheetLinks) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"icon\" href=\"/favicon.ico\">"
      "<link rel=\"preload\" href=\"/font.woff2\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.stylesheets.empty());
}

TEST(HtmlScannerTest, CollectsInlineCss) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<style>.container { max-width: 1200px; }</style>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(".container { max-width: 1200px; }", result.inline_css);
}

TEST(HtmlScannerTest, CollectsMultipleInlineStyleTags) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<style>.a { color: red; }</style>"
      "<style>.b { color: blue; }</style>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // Multiple style blocks should be joined with newlines
  EXPECT_EQ(".a { color: red; }\n.b { color: blue; }", result.inline_css);
}

TEST(HtmlScannerTest, TracksDepthCorrectly) {
  HtmlScanner scanner;
  std::string html =
      "<html>"
      "<body>"
      "<div>"
      "<p>Text</p>"
      "</div>"
      "</body>"
      "</html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.elements.size());

  EXPECT_EQ("html", result.elements[0].tag_name);
  EXPECT_EQ(0, result.elements[0].depth);

  EXPECT_EQ("body", result.elements[1].tag_name);
  EXPECT_EQ(1, result.elements[1].depth);

  EXPECT_EQ("div", result.elements[2].tag_name);
  EXPECT_EQ(2, result.elements[2].depth);

  EXPECT_EQ("p", result.elements[3].tag_name);
  EXPECT_EQ(3, result.elements[3].depth);
}

TEST(HtmlScannerTest, HandlesNestedElements) {
  HtmlScanner scanner;
  std::string html =
      "<section id=\"hero\" class=\"header-section\">"
      "<div class=\"container\">"
      "<nav class=\"main-nav\">"
      "<ul><li><a href=\"/\">Home</a></li></ul>"
      "</nav>"
      "</div>"
      "</section>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(6u, result.elements.size());

  // section
  EXPECT_EQ("section", result.elements[0].tag_name);
  EXPECT_EQ("hero", result.elements[0].id);
  ASSERT_EQ(1u, result.elements[0].classes.size());
  EXPECT_EQ("header-section", result.elements[0].classes[0]);

  // div.container
  EXPECT_EQ("div", result.elements[1].tag_name);
  ASSERT_EQ(1u, result.elements[1].classes.size());
  EXPECT_EQ("container", result.elements[1].classes[0]);

  // nav.main-nav
  EXPECT_EQ("nav", result.elements[2].tag_name);
  ASSERT_EQ(1u, result.elements[2].classes.size());
  EXPECT_EQ("main-nav", result.elements[2].classes[0]);
}

TEST(HtmlScannerTest, HandlesElementsWithoutAttributes) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/", "<p>Plain paragraph</p>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  EXPECT_EQ("p", result.elements[0].tag_name);
  EXPECT_TRUE(result.elements[0].id.empty());
  EXPECT_TRUE(result.elements[0].classes.empty());
}

TEST(HtmlScannerTest, AssignsElementIndexesSequentially) {
  HtmlScanner scanner;
  std::string html = "<div></div><span></span><p></p><a></a>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.elements.size());
  EXPECT_EQ(0, result.elements[0].element_index);
  EXPECT_EQ(1, result.elements[1].element_index);
  EXPECT_EQ(2, result.elements[2].element_index);
  EXPECT_EQ(3, result.elements[3].element_index);
}

TEST(HtmlScannerTest, HandlesWhitespaceInClassAttribute) {
  HtmlScanner scanner;
  // Test various whitespace characters between classes
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<div class=\"  a  \t  b  \n  c  \">Content</div>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  ASSERT_EQ(3u, result.elements[0].classes.size());
  EXPECT_EQ("a", result.elements[0].classes[0]);
  EXPECT_EQ("b", result.elements[0].classes[1]);
  EXPECT_EQ("c", result.elements[0].classes[2]);
}

TEST(HtmlScannerTest, ScanMultipleCalls) {
  // Test that multiple scan calls work correctly
  HtmlScanner scanner;

  HtmlScanResult result1 =
      scanner.Scan("http://a.com/", "<div id=\"a\"></div>");
  EXPECT_TRUE(result1.success);
  EXPECT_EQ("a", result1.elements[0].id);

  HtmlScanResult result2 =
      scanner.Scan("http://b.com/", "<div id=\"b\"></div>");
  EXPECT_TRUE(result2.success);
  EXPECT_EQ("b", result2.elements[0].id);
}

TEST(HtmlScannerTest, HandlesCompleteHtmlDocument) {
  HtmlScanner scanner;
  std::string html =
      "<!DOCTYPE html>\n"
      "<html lang=\"en\">\n"
      "<head>\n"
      "<meta charset=\"UTF-8\">\n"
      "<title>Test Page</title>\n"
      "<link rel=\"stylesheet\" href=\"/styles.css\">\n"
      "<style>body { margin: 0; }</style>\n"
      "</head>\n"
      "<body>\n"
      "<header id=\"header\" class=\"site-header\">\n"
      "<nav class=\"main-nav\">Nav</nav>\n"
      "</header>\n"
      "<main class=\"content\">Main content</main>\n"
      "<footer>Footer</footer>\n"
      "</body>\n"
      "</html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);

  // Check stylesheets
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("/styles.css", result.stylesheets[0].href);

  // Check inline CSS
  EXPECT_EQ("body { margin: 0; }", result.inline_css);

  // Check that key elements are found
  bool found_header = false;
  bool found_nav = false;
  bool found_main = false;
  bool found_footer = false;

  for (const auto& elem : result.elements) {
    if (elem.tag_name == "header" && elem.id == "header") {
      found_header = true;
      EXPECT_EQ(1u, elem.classes.size());
      EXPECT_EQ("site-header", elem.classes[0]);
    }
    if (elem.tag_name == "nav" && !elem.classes.empty() &&
        elem.classes[0] == "main-nav") {
      found_nav = true;
    }
    if (elem.tag_name == "main") {
      found_main = true;
    }
    if (elem.tag_name == "footer") {
      found_footer = true;
    }
  }

  EXPECT_TRUE(found_header);
  EXPECT_TRUE(found_nav);
  EXPECT_TRUE(found_main);
  EXPECT_TRUE(found_footer);
}

// ========== LCP Candidate Detection Tests ==========

TEST(HtmlScannerTest, DetectsLcpCandidateInHeroSection) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div>Some content</div>"
      "<section class=\"hero\">"
      "<img src=\"/images/hero.jpg\">"
      "</section>"
      "<img src=\"/images/other.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, DetectsLcpCandidateInHeader) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<header>"
      "<img src=\"/images/logo.png\">"
      "</header>"
      "<img src=\"/images/content.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/logo.png", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, FallsBackToFirstBodyImg) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"/images/first.jpg\"></div>"
      "<div><img src=\"/images/second.jpg\"></div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/first.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, HeroCandidateOverridesFallback) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"/images/first.jpg\"></div>"
      "<section class=\"hero-banner\">"
      "<img src=\"/images/hero.jpg\">"
      "</section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src)
      << "Hero candidate should override the fallback first-img candidate";
}

TEST(HtmlScannerTest, SkipsDataUrlsForLcp) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<img src=\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP\">"
      "<img src=\"/images/real.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/real.jpg", result.lcp_candidate.src)
      << "data: URLs should be skipped for LCP candidate";
}

TEST(HtmlScannerTest, NoLcpCandidateWithoutImages) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div>Just text content</div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.lcp_candidate.src.empty());
}

TEST(HtmlScannerTest, DetectsLcpInArticleTag) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<article>"
      "<img src=\"/images/article-hero.jpg\">"
      "</article>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/article-hero.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, DetectsLcpBySrcset) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\">"
      "<img src=\"/images/hero.jpg\" srcset=\"/images/hero-2x.jpg 2x\">"
      "</section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
  EXPECT_EQ("/images/hero-2x.jpg 2x", result.lcp_candidate.srcset);
}

// ========== M1: LCP sizes attribute detection ==========

TEST(HtmlScannerTest, DetectsLcpWithSizes) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\">"
      "<img src=\"hero.jpg\" "
      "srcset=\"hero-800.jpg 800w\" "
      "sizes=\"(max-width: 600px) 100vw, 50vw\">"
      "</section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("hero.jpg", result.lcp_candidate.src);
  EXPECT_EQ("hero-800.jpg 800w", result.lcp_candidate.srcset);
  EXPECT_EQ("(max-width: 600px) 100vw, 50vw", result.lcp_candidate.sizes);
}

// ========== M2: Hero class word-boundary matching ==========

TEST(HtmlScannerTest, SuperheroClassNotTreatedAsHero) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div class=\"superhero-card\"><img src=\"villain.jpg\"></div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "superhero-card" should NOT match hero pattern; the image should be
  // selected only as the first-body-img fallback, not as a hero candidate.
  EXPECT_EQ("villain.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, HeroPrefixWithHyphenMatches) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"other.jpg\"></div>"
      "<section class=\"hero-section\"><img src=\"real-hero.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "hero-section" should match as hero (prefix + hyphen)
  EXPECT_EQ("real-hero.jpg", result.lcp_candidate.src)
      << "hero-section class should be detected as hero container";
}

TEST(HtmlScannerTest, CookiebannerNotTreatedAsHero) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div class=\"cookiebanner\"><img src=\"close.png\"></div>"
      "<div><img src=\"real.jpg\"></div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "cookiebanner" should NOT match "banner" pattern; fallback picks first img
  EXPECT_EQ("close.png", result.lcp_candidate.src)
      << "cookiebanner should not be treated as hero; first img is fallback";
}

TEST(HtmlScannerTest, CaseInsensitiveHeroClass) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"other.jpg\"></div>"
      "<section class=\"HERO\"><img src=\"upper.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("upper.jpg", result.lcp_candidate.src)
      << "HERO (uppercase) should match as hero container";
}

TEST(HtmlScannerTest, DetectsLcpInMainTag) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<main><img src=\"main-hero.jpg\"></main>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("main-hero.jpg", result.lcp_candidate.src)
      << "<main> should be detected as hero container";
}

TEST(HtmlScannerTest, NestedHeroContainerPropagates) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<main><section class=\"hero\"><div class=\"wrapper\">"
      "<img src=\"deep.jpg\">"
      "</div></section></main>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("deep.jpg", result.lcp_candidate.src)
      << "Hero ancestor status should propagate through nested containers";
}

TEST(HtmlScannerTest, MultipleHeroSectionsPicksFirst) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\"><img src=\"first-hero.jpg\"></section>"
      "<section class=\"hero\"><img src=\"second-hero.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("first-hero.jpg", result.lcp_candidate.src)
      << "First hero section image should be selected";
}

// ========== <picture> and non-LCP image handling ==========

TEST(HtmlScannerTest, LcpCandidateInsidePictureIsFlagged) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\"><picture>"
      "<source srcset=\"/images/hero.avif\" type=\"image/avif\">"
      "<img src=\"/images/hero.jpg\">"
      "</picture></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
  EXPECT_TRUE(result.lcp_candidate.in_picture)
      << "An <img> child of <picture> must be flagged: a <source> sibling "
         "may win selection, so preloading its src risks a double download";
}

TEST(HtmlScannerTest, LcpCandidateOutsidePictureNotFlagged) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\"><img src=\"/images/hero.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
  EXPECT_FALSE(result.lcp_candidate.in_picture);
}

TEST(HtmlScannerTest, LcpFallbackSkipsTrackingPixel) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<img src=\"/pixel.gif\" width=\"1\" height=\"1\">"
      "<img src=\"/images/real.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/real.jpg", result.lcp_candidate.src)
      << "A 1x1 beacon must not become the LCP candidate";
}

TEST(HtmlScannerTest, LcpHeroSkipsHiddenImage) {
  HtmlScanner scanner;
  std::string html =
      "<html><body><section class=\"hero\">"
      "<img src=\"/spacer.gif\" hidden>"
      "<img src=\"/lazy-placeholder.png\" style=\"display: none\">"
      "<img src=\"/images/hero.jpg\">"
      "</section></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src)
      << "hidden / display:none images are not rendered and must not "
         "become the LCP candidate";
}

// ========== Third-Party Origin Extraction Tests ==========

TEST(HtmlScannerTest, ExtractsThirdPartyOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/style.css\">"
      "<script src=\"https://analytics.example.com/track.js\"></script>"
      "</head><body>"
      "<img src=\"https://images.example.com/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(3u, result.third_party_origins.size());
  // Render-blocking resources should come first (stylesheet, sync script).
  EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin);
  EXPECT_EQ("https://analytics.example.com",
            result.third_party_origins[1].origin);
  EXPECT_EQ("https://images.example.com", result.third_party_origins[2].origin);
}

// Preconnect ordering must not flap between the raw and reprocessed passes.
// Origins are emitted best-first (stable_sort on a priority bucket), and a
// cross-origin stylesheet earns the render-blocking bucket. Once deferred, that
// same <link> reads rel="preload" — so unless the classifier recognises the
// deferred primary, the reprocessed pass drops it to the ordinary head-resource
// bucket and the preconnect list comes out in a different ORDER than the raw
// pass produced. Same page, same resources, different hints depending on who
// last processed it.
//
// The <link rel="icon"> is what makes the demotion observable and must come
// FIRST: it is a head resource in both worlds, so on a correct scan the
// stylesheet's higher bucket lifts it above the icon, while a demoted
// stylesheet ties with the icon and the stable sort leaves the icon in front.
// (An image would not work here — body resources sort after head ones either
// way, so the order would be identical under the bug.)
TEST(HtmlScannerTest, DeferredPrimaryKeepsItsRenderBlockingPreconnectRank) {
  HtmlScanner scanner;
  const char* kRawSheet =
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/style.css\">";
  const char* kDeferredSheet =
      "<link rel=\"preload\" as=\"style\" "
      "href=\"https://cdn.example.com/style.css\" "
      "data-pagespeed-media=\"all\" data-pagespeed-async=\"\">";

  for (const char* sheet : {kRawSheet, kDeferredSheet}) {
    std::string html =
        std::string("<html><head>") +
        "<link rel=\"icon\" href=\"https://icons.example.com/f.ico\">" + sheet +
        "</head><body>Hello</body></html>";

    HtmlScanResult result = scanner.Scan("http://example.com/", html);

    ASSERT_TRUE(result.success) << sheet;
    ASSERT_EQ(2u, result.third_party_origins.size()) << sheet;
    EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin)
        << "the deferred stylesheet's origin must keep the render-blocking "
           "rank it had before deferral, ahead of a mere head resource: "
        << sheet;
    EXPECT_EQ("https://icons.example.com", result.third_party_origins[1].origin)
        << sheet;
  }
}

TEST(HtmlScannerTest, FiltersSameOrigin) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
      "<script src=\"https://cdn.other.com/lib.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("https://cdn.other.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, DeduplicatesOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/a.css\">"
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/b.css\">"
      "<script src=\"https://cdn.example.com/app.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, CapsOriginsAtFour) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://cdn1.example.com/a.css\">"
      "<link rel=\"stylesheet\" href=\"https://cdn2.example.com/b.css\">"
      "<script src=\"https://cdn3.example.com/c.js\"></script>"
      "<script src=\"https://cdn4.example.com/d.js\"></script>"
      "<script src=\"https://cdn5.example.com/e.js\"></script>"
      "<script src=\"https://cdn6.example.com/f.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(4u, result.third_party_origins.size());
}

TEST(HtmlScannerTest, SkipsRelativeUrlsForOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/css/style.css\">"
      "<script src=\"/js/app.js\"></script>"
      "</head><body>"
      "<img src=\"/images/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.third_party_origins.empty())
      << "Relative URLs should not produce origins";
}

TEST(HtmlScannerTest, PrioritizesRenderBlockingResources) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<script defer src=\"https://defer.example.com/d.js\"></script>"
      "<link rel=\"stylesheet\" href=\"https://css.example.com/style.css\">"
      "<script src=\"https://sync.example.com/s.js\"></script>"
      "</head><body>"
      "<img src=\"https://img.example.com/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.third_party_origins.size());
  // Render-blocking (stylesheet, sync script) should come before
  // deferred script and body image.
  EXPECT_EQ("https://css.example.com", result.third_party_origins[0].origin);
  EXPECT_EQ("https://sync.example.com", result.third_party_origins[1].origin);
}

TEST(HtmlScannerTest, ProtocolRelativeOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<script src=\"//cdn.example.com/lib.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("//cdn.example.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, RecordsCorsModeEvidencePerOrigin) {
  HtmlScanner scanner;
  // Plain stylesheets/scripts/images fetch no-cors; a crossorigin attribute,
  // an ES module script, or a font preload marks the origin CORS-mode.
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://css.example.com/style.css\">"
      "<script src=\"https://cors.example.com/app.js\" crossorigin>"
      "</script>"
      "<script type=\"module\" src=\"https://mod.example.com/m.js\"></script>"
      "<link rel=\"preload\" as=\"font\" "
      "href=\"https://fonts.example.com/a.woff2\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.third_party_origins.size());
  // Render-blocking (stylesheet + the sync crossorigin script) sort first.
  EXPECT_EQ("https://css.example.com", result.third_party_origins[0].origin);
  EXPECT_FALSE(result.third_party_origins[0].crossorigin)
      << "A plain stylesheet fetches no-cors";
  EXPECT_EQ("https://cors.example.com", result.third_party_origins[1].origin);
  EXPECT_TRUE(result.third_party_origins[1].crossorigin)
      << "An explicit crossorigin attribute marks the origin CORS-mode";
  EXPECT_EQ("https://mod.example.com", result.third_party_origins[2].origin);
  EXPECT_TRUE(result.third_party_origins[2].crossorigin)
      << "ES module scripts always fetch in CORS mode";
  EXPECT_EQ("https://fonts.example.com", result.third_party_origins[3].origin);
  EXPECT_TRUE(result.third_party_origins[3].crossorigin)
      << "Font fetches are always CORS-mode";
}

TEST(HtmlScannerTest, PlainImageOriginIsNoCors) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<img src=\"https://images.example.com/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_FALSE(result.third_party_origins[0].crossorigin)
      << "A plain <img> fetches no-cors";
}

TEST(HtmlScannerTest, MixedModeOriginFirstSeenDecides) {
  // An origin fetched in BOTH modes keeps the crossorigin bit of the
  // first-seen resource: warming one of its pools still beats the old
  // always-crossorigin behavior, and one entry per origin keeps the
  // 4-entry budget simple.
  HtmlScanner scanner;
  std::string plain_first =
      "<html><head>"
      "<script src=\"https://mixed.example.com/app.js\"></script>"
      "<link rel=\"preload\" as=\"font\" "
      "href=\"https://mixed.example.com/a.woff2\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", plain_first);
  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_FALSE(result.third_party_origins[0].crossorigin)
      << "Plain script seen first: the origin stays no-cors";

  std::string cors_first =
      "<html><head>"
      "<link rel=\"preload\" as=\"font\" "
      "href=\"https://mixed.example.com/a.woff2\">"
      "<script src=\"https://mixed.example.com/app.js\"></script>"
      "</head><body></body></html>";

  result = scanner.Scan("http://example.com/", cors_first);
  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_TRUE(result.third_party_origins[0].crossorigin)
      << "Font preload seen first: the origin is CORS-mode";
}

TEST(HtmlScannerTest, NoOriginsInEmptyPage) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan("http://example.com/",
                                       "<html><body>Just text</body></html>");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.third_party_origins.empty());
}

// ========== Coverage: URL edge cases ==========

TEST(HtmlScannerTest, UrlWithoutTrailingSlash) {
  HtmlScanner scanner;
  // URL without a path (no trailing slash). ExtractOrigin should return
  // the full URL as the origin.
  std::string html =
      "<html><head>"
      "<script src=\"https://cdn.example.com\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // The origin for "https://cdn.example.com" (no path) should be
  // "https://cdn.example.com" itself.
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, InvalidUrlInSrcAttribute) {
  HtmlScanner scanner;
  // Malformed/relative URLs should not produce third-party origins.
  std::string html =
      "<html><head>"
      "<script src=\"not-a-url\"></script>"
      "<link rel=\"stylesheet\" href=\"also/not/absolute\">"
      "</head><body>"
      "<img src=\"relative/image.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.third_party_origins.empty())
      << "Malformed/relative URLs should not produce origins";
}

// ========== Coverage: HasHeroClass dash-prefix branch ==========

// When a non-hero tag (e.g. <div>) has a hero-prefixed class like
// "hero-banner", the HasHeroClass function must detect it via the
// starts_with + dash check (lines 307-309 of html_scanner.cc).
// Previous tests used <section class="hero-section"> which triggered
// the hero check via the tag keyword, short-circuiting HasHeroClass.
TEST(HtmlScannerTest, DivWithHeroDashClassIsHeroContainer) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"fallback.jpg\"></div>"
      "<div class=\"hero-carousel\">"
      "<img src=\"hero-img.jpg\">"
      "</div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "hero-carousel" on a <div> should match via HasHeroClass prefix+dash.
  // The hero candidate should override the fallback first-img.
  EXPECT_EQ("hero-img.jpg", result.lcp_candidate.src)
      << "div.hero-carousel should be detected as hero via class prefix";
}

// Test "banner-" prefix on a non-hero tag.
TEST(HtmlScannerTest, DivWithBannerDashClassIsHeroContainer) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"fallback.jpg\"></div>"
      "<div class=\"banner-top\">"
      "<img src=\"banner-img.jpg\">"
      "</div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("banner-img.jpg", result.lcp_candidate.src)
      << "div.banner-top should be detected as hero via class prefix";
}

// ========== Coverage: Empty URL causes StartParse failure ==========

TEST(HtmlScannerTest, EmptyUrlFailsParsing) {
  HtmlScanner scanner;
  // Passing an empty URL should cause StartParse to return false,
  // triggering the error path at lines 364-366.
  HtmlScanResult result = scanner.Scan("", "<html><body>Hello</body></html>");

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Failed to start parsing: invalid URL");
  EXPECT_TRUE(result.elements.empty());
}

// ========== SRI: integrity-pinned subresources (issue #656) ==========

TEST(HtmlScannerTest, CollectsIntegrityPinnedUrls) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<script src=\"/pinned.js\" integrity=\"sha384-abc\"></script>"
      "<link rel=\"stylesheet\" href=\"/pinned.css\" "
      "integrity=\"sha384-def\">"
      "<link rel=\"preload\" as=\"script\" href=\"/preload.js\" "
      "integrity=\"sha384-ghi\">"
      "<script src=\"/free.js\"></script>"
      "<link rel=\"stylesheet\" href=\"/free.css\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(3u, result.integrity_pinned_urls.size());
  EXPECT_EQ("/pinned.js", result.integrity_pinned_urls[0]);
  EXPECT_EQ("/pinned.css", result.integrity_pinned_urls[1]);
  EXPECT_EQ("/preload.js", result.integrity_pinned_urls[2]);
}

TEST(HtmlScannerTest, NoIntegrityNoPinnedUrls) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head><script src=\"/app.js\"></script>"
      "<link rel=\"stylesheet\" href=\"/style.css\"></head></html>");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.integrity_pinned_urls.empty());
}

// ========== Script src collection + author <base> detection ==========

TEST(HtmlScannerTest, CollectsScriptSrcsInDocumentOrder) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head><script src=\"/a.js\"></script>"
      "<script>var inline = 1;</script>"
      "<script src=\"https://cdn.example.net/b.js\"></script></head>"
      "<body><script src=\"c.js\"></script></body></html>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(3u, result.script_srcs.size());
  EXPECT_EQ("/a.js", result.script_srcs[0]);
  EXPECT_EQ("https://cdn.example.net/b.js", result.script_srcs[1]);
  EXPECT_EQ("c.js", result.script_srcs[2]);
}

TEST(HtmlScannerTest, DetectsAuthorBaseHref) {
  HtmlScanner scanner;
  HtmlScanResult with_base = scanner.Scan(
      "http://example.com/",
      "<html><head><base href=\"/sub/\"></head><body></body></html>");
  EXPECT_TRUE(with_base.success);
  EXPECT_TRUE(with_base.has_base_href);
  EXPECT_EQ("/sub/", with_base.base_href);

  HtmlScanner scanner2;
  HtmlScanResult without_href = scanner2.Scan(
      "http://example.com/",
      "<html><head><base target=\"_blank\"></head><body></body></html>");
  EXPECT_TRUE(without_href.success);
  // An href-less <base> sets no base URL and must not count.
  EXPECT_FALSE(without_href.has_base_href);
  EXPECT_TRUE(without_href.base_href.empty());

  HtmlScanner scanner3;
  HtmlScanResult no_base = scanner3.Scan(
      "http://example.com/", "<html><head></head><body></body></html>");
  EXPECT_TRUE(no_base.success);
  EXPECT_FALSE(no_base.has_base_href);
}

// Only the FIRST base-with-href sets the document base URL (HTML spec); an
// href-less <base target> before it must not block the capture.
TEST(HtmlScannerTest, FirstBaseHrefWins) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<html><head><base target=\"_blank\">"
                   "<base href=\"/first/\"><base href=\"/second/\"></head>"
                   "<body></body></html>");
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.has_base_href);
  EXPECT_EQ("/first/", result.base_href);
}

}  // namespace
}  // namespace pagespeed
