// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test for the Critical CSS Extractor

#include "src/worker/critical_css_extractor.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "gtest/gtest.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {
namespace {

// Helper to create test elements
CollectedElement MakeElement(const std::string& tag, int depth, int index,
                             const std::string& id = "",
                             const std::vector<std::string>& classes = {}) {
  CollectedElement elem;
  elem.tag_name = tag;
  elem.depth = depth;
  elem.element_index = index;
  elem.id = id;
  elem.classes = classes;
  return elem;
}

TEST(CriticalCssExtractorTest, ExtractFromEmptyCss) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  CriticalCssResult result = extractor.Extract(elements, "");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.critical_css.empty());
  EXPECT_EQ(0, result.total_rules);
  EXPECT_EQ(0, result.critical_rules);
}

TEST(CriticalCssExtractorTest, IncludesUniversalSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "* { box-sizing: border-box; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("* {"), std::string::npos);
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, IncludesHtmlBodySelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "html { font-size: 16px; }\n"
      "body { margin: 0; }\n"
      ":root { --color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("html {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(":root {"), std::string::npos);
  EXPECT_EQ(3, result.critical_rules);
}

TEST(CriticalCssExtractorTest, MatchesClassSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"container"}));

  std::string css =
      ".container { max-width: 1200px; }\n"
      ".footer { margin-top: 50px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".container {"), std::string::npos);
  // Footer should not be included (excluded pattern)
  EXPECT_EQ(result.critical_css.find(".footer {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MatchesIdSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "main-content"));

  std::string css =
      "#main-content { padding: 20px; }\n"
      "#sidebar { width: 300px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#main-content {"), std::string::npos);
  // Sidebar not in elements, should not be included
  EXPECT_EQ(result.critical_css.find("#sidebar {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MatchesTagSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("header", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1));

  std::string css =
      "header { background: #fff; }\n"
      "nav { display: flex; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("header {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("nav {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MatchesCombinedSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "hero", {"container", "dark"}));

  std::string css = "div#hero.container.dark { color: white; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div#hero.container.dark {"),
            std::string::npos);
}

TEST(CriticalCssExtractorTest, ExcludesMediaPrintRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "body { margin: 0; }\n"
      "@media print { body { font-size: 12pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body { margin"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("@media print"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesFontFaceRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@font-face { font-family: 'Custom'; src: url('font.woff2'); }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@font-face"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesKeyframesRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@keyframes fadeIn"), std::string::npos);
}

TEST(CriticalCssExtractorTest, ExcludesElementsWithExcludedClassPattern) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"hero"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"lazy-load"}));
  elements.push_back(MakeElement("footer", 2, 2, "", {"site-footer"}));

  std::string css =
      ".hero { height: 100vh; }\n"
      ".lazy-load { opacity: 0; }\n"
      ".site-footer { margin-top: 50px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".hero {"), std::string::npos);
  // lazy-load and footer should be excluded
  EXPECT_EQ(result.critical_css.find(".lazy-load {"), std::string::npos);
  EXPECT_EQ(result.critical_css.find(".site-footer {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesHeaderNavHeroPatterns) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("section", 0, 0, "", {"hero-section"}));
  elements.push_back(MakeElement("div", 1, 1, "header-wrapper"));
  elements.push_back(MakeElement("nav", 2, 2, "main-nav"));

  std::string css =
      ".hero-section { min-height: 100vh; }\n"
      "#header-wrapper { position: fixed; }\n"
      "#main-nav { display: flex; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // These should all be included due to pattern matching
  EXPECT_NE(result.critical_css.find(".hero-section {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("#header-wrapper {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("#main-nav {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, RespectsMaxElements) {
  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"first"}));
  elements.push_back(MakeElement("div", 0, 1, "", {"second"}));
  elements.push_back(MakeElement("div", 0, 2, "", {"third"}));  // Beyond limit

  std::string css =
      ".first { color: red; }\n"
      ".second { color: blue; }\n"
      ".third { color: green; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".first {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".second {"), std::string::npos);
  // Third is beyond max_elements and doesn't have include patterns
  // (but since max_depth is still 10, it might still be included)
}

TEST(CriticalCssExtractorTest, RespectsMaxDepth) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 100;  // Don't limit by elements
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "", {"shallow"}));
  elements.push_back(MakeElement("div", 5, 1, "", {"deep"}));  // Beyond depth

  std::string css =
      ".shallow { color: red; }\n"
      ".deep { color: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".shallow {"), std::string::npos);
  // Deep element is excluded due to depth
  EXPECT_EQ(result.critical_css.find(".deep {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesCommaSeparatedSelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("h1", 0, 0));
  elements.push_back(MakeElement("h2", 0, 1));

  std::string css = "h1, h2, h3, h4 { font-weight: bold; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should be included because h1 and h2 are in elements
  EXPECT_NE(result.critical_css.find("h1, h2, h3, h4 {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesDescendantSelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("header", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1));
  elements.push_back(MakeElement("a", 2, 2));

  std::string css = "header nav a { color: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should match because 'a' is in elements
  EXPECT_NE(result.critical_css.find("header nav a {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesChildSelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("ul", 0, 0, "", {"menu"}));
  elements.push_back(MakeElement("li", 1, 1));

  std::string css = "ul.menu > li { list-style: none; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should match because 'li' is in elements
  EXPECT_NE(result.critical_css.find("ul.menu > li {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesCssComments) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "/* This is a comment */\n"
      "body { margin: 0; }\n"
      "/* Another comment */";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HandlesNestedBraces) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  std::string css =
      "@media screen {\n"
      "  div { padding: 10px; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // @media screen should be included (not print)
  EXPECT_NE(result.critical_css.find("@media screen"), std::string::npos);
}

TEST(CriticalCssExtractorTest, PreservesOriginalCssFormatting) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "body {\n  margin: 0;\n  padding: 0;\n}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Body content should be preserved
  EXPECT_NE(result.critical_css.find("margin: 0"), std::string::npos);
  EXPECT_NE(result.critical_css.find("padding: 0"), std::string::npos);
}

TEST(CriticalCssExtractorTest, StatisticsAreAccurate) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"content"}));

  std::string css =
      "body { margin: 0; }\n"
      ".content { padding: 20px; }\n"
      ".footer { margin-top: 50px; }\n"
      "@media print { body { font-size: 12pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(4, result.total_rules);
  // body and .content should be included, .footer and @media print excluded
  EXPECT_EQ(2, result.critical_rules);
}

// --- Viewport-aware @media filtering tests ---

TEST(CriticalCssExtractorTest, ExcludesDesktopMediaForMobile) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "body { margin: 0; }\n"
      "@media (min-width: 1024px) { .desktop-only { display: block; } }";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("@media (min-width: 1024px)"),
            std::string::npos)
      << "Desktop-only @media should be excluded for mobile";
}

TEST(CriticalCssExtractorTest, ExcludesMobileMediaForDesktop) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "body { margin: 0; }\n"
      "@media (max-width: 479px) { .mobile-only { display: block; } }";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("@media (max-width: 479px)"),
            std::string::npos)
      << "Mobile-only @media should be excluded for desktop";
}

TEST(CriticalCssExtractorTest, IncludesOverlappingMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // min-width: 400px overlaps with Mobile (0-479) and Tablet (480-1023)
  std::string css = "@media (min-width: 400px) { .wide { display: flex; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_NE(result_mobile.critical_css.find("@media (min-width: 400px)"),
            std::string::npos)
      << "Should be included for mobile (overlaps 400-479)";
  EXPECT_NE(result_tablet.critical_css.find("@media (min-width: 400px)"),
            std::string::npos)
      << "Should be included for tablet (overlaps 480-1023)";
}

TEST(CriticalCssExtractorTest, IncludesUnrecognizedMediaConservatively) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@media screen and (hover: hover) { a { text-decoration: none; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media screen"), std::string::npos)
      << "Unrecognized media query should be conservatively included (mobile)";
  EXPECT_NE(result_desktop.critical_css.find("@media screen"),
            std::string::npos)
      << "Unrecognized media query should be conservatively included "
         "(desktop)";
}

TEST(CriticalCssExtractorTest, IncludesMinMaxOverlap) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Tablet-only range: 768-1023
  std::string css =
      "@media (min-width: 768px) and (max-width: 1023px) "
      "{ .tablet-only { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "768-1023 range should be excluded for mobile (0-479)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "768-1023 range should be included for tablet (480-1279)";
  EXPECT_EQ(result_desktop.critical_css.find("@media"), std::string::npos)
      << "768-1023 range should be excluded for desktop (1280-65535)";
}

TEST(CriticalCssExtractorTest, DefaultViewportIsDesktop) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Mobile-only query
  std::string css = "@media (max-width: 479px) { .mobile { display: block; } }";

  // Default (no viewport arg) should behave as desktop
  CriticalCssResult result_default = extractor.Extract(elements, css);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_default.critical_css, result_desktop.critical_css)
      << "Default viewport should match desktop behavior";
  // Both should exclude the mobile-only query
  EXPECT_EQ(result_default.critical_css.find("@media"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesEmUnitsInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 64em = 64 * 16 = 1024px — should match desktop but not mobile
  std::string css =
      "@media (min-width: 64em) { .desktop-em { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "64em (1024px) should be excluded for mobile (0-479)";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "64em (1024px) should be included for desktop (1024-65535)";
}

TEST(CriticalCssExtractorTest, HandlesRemUnitsInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 48rem = 48 * 16 = 768px — should match tablet but not mobile
  std::string css =
      "@media (min-width: 48rem) { .tablet-rem { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "48rem (768px) should be excluded for mobile (0-479)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "48rem (768px) should be included for tablet (480-1023)";
}

TEST(CriticalCssExtractorTest, HandlesMediaQueryList) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Comma-separated list: mobile OR desktop (not tablet-only range)
  std::string css =
      "@media (max-width: 479px), (min-width: 1280px) "
      "{ .responsive { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Should include for mobile (first subquery matches 0-479)";
  EXPECT_EQ(result_tablet.critical_css.find("@media"), std::string::npos)
      << "Should exclude for tablet (neither subquery overlaps 480-1279)";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Should include for desktop (second subquery matches 1280+)";
}

TEST(CriticalCssExtractorTest, MediaQueryListWithUnrecognizedSubquery) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // One parseable subquery that doesn't match + one unrecognized
  std::string css =
      "@media (max-width: 479px), (orientation: portrait) "
      "{ .mixed { display: block; } }";

  // Should conservatively include for all viewports because one
  // subquery is unrecognized.
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Should conservatively include when any subquery is unrecognized";
}

// --- New tests for overflow bug and edge cases ---

TEST(CriticalCssExtractorTest, HugeValueInMediaQueryDoesNotOverflow) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // A huge value like 999999999999px would overflow a 32-bit int during
  // parsing.  ParseCssLength must handle this gracefully (return -1)
  // so the query is conservatively included rather than causing UB.
  std::string css =
      "@media (min-width: 999999999999px) { .huge { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  // Because the value is unparseable (overflow), it should be conservatively
  // included for all viewports.
  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Huge value should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Huge value should be conservatively included for desktop";
}

TEST(CriticalCssExtractorTest, HandlesFractionalEmInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 47.9375em * 16 = 767px -- should be excluded for mobile (0-479),
  // included for tablet (480-1023).
  std::string css =
      "@media (min-width: 47.9375em) { .frac-em { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "47.9375em (767px) should be excluded for mobile (0-479)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "47.9375em (767px) should be included for tablet (480-1023)";

  // 1.5em * 16 = 24px -- should be included for all viewports.
  std::string css2 =
      "@media (min-width: 1.5em) { .small-em { display: block; } }";

  CriticalCssResult result2_mobile =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result2_desktop =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result2_mobile.critical_css.find("@media"), std::string::npos)
      << "1.5em (24px) should be included for mobile";
  EXPECT_NE(result2_desktop.critical_css.find("@media"), std::string::npos)
      << "1.5em (24px) should be included for desktop";
}

TEST(CriticalCssExtractorTest, HandlesCombinedScreenPrefixWithEm) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "screen and" prefix + em unit: 64em = 1024px
  std::string css =
      "@media screen and (min-width: 64em) "
      "{ .screen-em { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "screen and 64em (1024px) should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "screen and 64em (1024px) should be included for desktop";
}

TEST(CriticalCssExtractorTest, NotKeywordConservativelyIncluded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "not" keyword is not handled -- should be conservatively included
  // for ALL viewports since ParseCssLength returns -1.
  std::string css =
      "@media not (max-width: 479px) { .not-mobile { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "'not' keyword should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "'not' keyword should be conservatively included for desktop";

  // Also test "not screen and ..." variant
  std::string css2 =
      "@media not screen and (min-width: 1024px) "
      "{ .not-screen { display: block; } }";

  CriticalCssResult result2_mobile =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result2_desktop =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result2_mobile.critical_css.find("@media"), std::string::npos)
      << "'not screen and' should be conservatively included for mobile";
  EXPECT_NE(result2_desktop.critical_css.find("@media"), std::string::npos)
      << "'not screen and' should be conservatively included for desktop";
}

TEST(CriticalCssExtractorTest, HandlesThreeSubqueries) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Three subqueries: each covers one viewport class exactly.
  std::string css =
      "@media (max-width: 479px), "
      "(min-width: 480px) and (max-width: 1279px), "
      "(min-width: 1280px) "
      "{ .all-viewports { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "First subquery (max-width: 479px) should match mobile";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "Second subquery (480px-1279px) should match tablet";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Third subquery (min-width: 1280px) should match desktop";
}

TEST(CriticalCssExtractorTest, HandlesOnlyScreenPrefix) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "only screen and" prefix should be stripped, leaving min-width: 1024px
  std::string css =
      "@media only screen and (min-width: 1024px) "
      "{ .only-screen { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "only screen and 1024px should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "only screen and 1024px should be included for desktop";
}

// ========== Phase 3.4: prefers-color-scheme preservation ==========

TEST(CriticalCssExtractorTest, PrefersColorSchemeDarkPreserved) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "body { background: #fff; }\n"
      "@media (prefers-color-scheme: dark) {\n"
      "  body { background: #111; color: #eee; }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  // prefers-color-scheme is not a viewport media query — it should be
  // conservatively included since the extractor can't evaluate it.
  EXPECT_NE(result.critical_css.find("prefers-color-scheme"), std::string::npos)
      << "prefers-color-scheme media query should be preserved in critical CSS";
  EXPECT_NE(result.critical_css.find("background: #111"), std::string::npos)
      << "Dark mode styles should be included";
}

TEST(CriticalCssExtractorTest, PrefersColorSchemeLightPreserved) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "@media (prefers-color-scheme: light) {\n"
      "  body { background: #fff; color: #333; }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("prefers-color-scheme"), std::string::npos)
      << "prefers-color-scheme: light should be preserved";
}

// ========== Phase 3.5: prefers-reduced-motion preservation ==========

TEST(CriticalCssExtractorTest, PrefersReducedMotionPreserved) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "@media (prefers-reduced-motion: reduce) {\n"
      "  *, *::before, *::after {\n"
      "    animation-duration: 0.01ms !important;\n"
      "    transition-duration: 0.01ms !important;\n"
      "  }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("prefers-reduced-motion"),
            std::string::npos)
      << "prefers-reduced-motion should be preserved in critical CSS";
  EXPECT_NE(result.critical_css.find("animation-duration"), std::string::npos)
      << "Reduced motion styles should be included";
}

// ========== Additional coverage: unclosed braces, selectors, edge cases ==========

TEST(CriticalCssExtractorTest, UnclosedBracesFallthrough) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with an unclosed brace: FindBlockEnd should reach end-of-string.
  std::string css = "body { margin: 0;\n .content { padding: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  // Should not crash; the unclosed brace causes the entire rest of the
  // input to be consumed as one rule's body.
  EXPECT_TRUE(result.success);
  // At least 1 rule should be parsed (body rule with unclosed brace
  // consuming remainder).
  EXPECT_GE(result.total_rules, 1);
}

TEST(CriticalCssExtractorTest, MediaFootprintNotMatchedAsPrint) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "footprint" contains "print" but should NOT be matched as @media print
  // because HasWordBoundary requires a word boundary around "print".
  std::string css = "@media footprint { .special { display: block; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "footprint" is not "print" at a word boundary, so the rule should NOT
  // be excluded as a print rule. It is an unrecognized @media and should
  // be conservatively included.
  EXPECT_NE(result.critical_css.find("@media footprint"), std::string::npos)
      << "@media footprint should not be excluded (not a print rule)";
}

TEST(CriticalCssExtractorTest, MediaBlueprintNotMatchedAsPrint) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "blueprint" contains "print" but no word boundary before it.
  std::string css = "@media blueprint { .special { display: block; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@media blueprint"), std::string::npos)
      << "@media blueprint should not be excluded";
}

TEST(CriticalCssExtractorTest, UniversalSelectorStarMatches) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"container"}));

  std::string css =
      "* { box-sizing: border-box; }\n"
      "*, *::before, *::after { box-sizing: inherit; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("* {"), std::string::npos);
  // The selector starting with * should also be included.
  EXPECT_NE(result.critical_css.find("*, *::before, *::after"),
            std::string::npos);
}

TEST(CriticalCssExtractorTest, ComplexSelectorWithChildCombinator) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));
  elements.push_back(MakeElement("span", 1, 1, "", {"bar"}));

  // "div > .foo + .bar" — final selector after combinators is ".bar".
  std::string css = "div > .foo + .bar { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The extractor uses the last part after combinators to match.
  // ".bar" matches the span with class "bar".
  EXPECT_NE(result.critical_css.find("div > .foo + .bar"), std::string::npos);
}

TEST(CriticalCssExtractorTest, SiblingCombinatorSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("p", 0, 0));

  // "h1 ~ p" — final selector is "p", which matches.
  std::string css = "h1 ~ p { margin-top: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("h1 ~ p"), std::string::npos);
}

TEST(CriticalCssExtractorTest, AttributeSelectorSkipped) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("input", 0, 0));

  // "input[type=text]" — the attribute selector is not matched by
  // SimpleSelectorMatchesElement (it breaks at '['), but the tag name
  // "input" should still match.
  std::string css = "input[type=text] { border: 1px solid; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The tag name "input" should be extracted before the '['.
  EXPECT_NE(result.critical_css.find("input[type=text]"), std::string::npos)
      << "input tag should match even with attribute selector";
}

TEST(CriticalCssExtractorTest, PseudoSelectorSkipped) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("a", 0, 0));

  // "a:hover" — the pseudo selector is not matched but the tag name "a"
  // should still match.
  std::string css =
      "a:hover { color: blue; }\n"
      "a:visited { color: purple; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The 'a' tag matches both rules.
  EXPECT_NE(result.critical_css.find("a:hover"), std::string::npos);
  EXPECT_NE(result.critical_css.find("a:visited"), std::string::npos);
}

// A `:where()`/`:is()` wrapper that holds the only DOM-matchable token must be
// evaluated even when an unhandled pseudo (`:hover`, `:focus-visible`, ...)
// precedes it. The parser used to `break` on the leading pseudo before reaching
// the wrapper, so `:hover:where(.foo)` was dropped even when an above-the-fold
// element carried class "foo" — defeating the wrapper-desugaring path's purpose.
TEST(CriticalCssExtractorTest, WhereAfterNonWherePseudoStillEvaluated) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));

  std::string css = ":hover:where(.foo) { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(1, result.critical_rules);
  EXPECT_NE(result.critical_css.find(":where(.foo)"), std::string::npos)
      << "rule whose only matchable token is inside :where() after a leading "
         "pseudo must be retained";
}

TEST(CriticalCssExtractorTest, ClassWithPseudoSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("button", 0, 0, "", {"btn"}));

  // ".btn:focus" — class ".btn" should be parsed before ':'.
  std::string css = ".btn:focus { outline: 2px solid blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".btn:focus"), std::string::npos)
      << ".btn class should match even with :focus pseudo-selector";
}

TEST(CriticalCssExtractorTest, IdWithPseudoSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "main"));

  // "#main:first-child" — ID "main" should be parsed before ':'.
  std::string css = "#main:first-child { padding: 20px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#main:first-child"), std::string::npos);
}

TEST(CriticalCssExtractorTest, EmptySelectorIgnored) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // A comma-separated list with an empty segment.
  std::string css = "body, , div { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should still match because "body" is in elements.
  EXPECT_NE(result.critical_css.find("body,"), std::string::npos);
}

TEST(CriticalCssExtractorTest, StringInCssBodyNotAffectsBlockParsing) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"quote"}));

  // A CSS rule containing a string with braces inside it.
  std::string css =
      ".quote { content: \"{ braces }\"; color: red; }\n"
      ".other { padding: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The braces inside the string should not affect block parsing.
  EXPECT_NE(result.critical_css.find(".quote {"), std::string::npos);
  // .other should be a separate rule, not swallowed.
  EXPECT_EQ(result.total_rules, 2);
}

TEST(CriticalCssExtractorTest, MediaPrintCaseInsensitive) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Various cases of @media print.
  std::string css =
      "@MEDIA PRINT { body { font-size: 12pt; } }\n"
      "@Media Print { body { background: white; } }\n"
      "body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // All print variants should be excluded.
  EXPECT_EQ(result.critical_css.find("PRINT"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("Print"), std::string::npos);
  // Regular rule should be included.
  EXPECT_NE(result.critical_css.find("body { margin"), std::string::npos);
  EXPECT_EQ(result.critical_rules, 1);
}

TEST(CriticalCssExtractorTest, IncludesImportRule) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@import url('fonts.css') { }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@import"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesCharsetRule) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@charset \"UTF-8\" { }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@charset"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesWebkitKeyframes) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@-webkit-keyframes spin { from { transform: rotate(0); } "
      "to { transform: rotate(360deg); } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@-webkit-keyframes"), std::string::npos);
}

// ========== Coverage: malformed CSS, unrecognized units, attribute selectors ==========

TEST(CriticalCssExtractorTest, MalformedCssNoBrace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with no opening brace at all — ParseCssRules should not crash
  // and should produce 0 rules (no brace found).
  std::string css = "body margin: 0;";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 0);
  EXPECT_TRUE(result.critical_css.empty());
}

TEST(CriticalCssExtractorTest, UnrecognizedLengthUnit) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "vw" is not a recognized unit for ParseCssLength (only px, em, rem).
  // The query should be conservatively included for all viewports.
  std::string css =
      "@media (min-width: 100vw) { .viewport-width { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result_mobile.success);
  EXPECT_TRUE(result_desktop.success);
  // Since "vw" is unrecognized, the query should be conservatively included.
  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'vw' should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'vw' should be conservatively included for desktop";
}

TEST(CriticalCssExtractorTest, SelectorWithAttributeBracket) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("input", 0, 0));

  // Attribute selector with quoted value: input[type="text"]
  // The parser should extract "input" before the '['.
  std::string css = "input[type=\"text\"] { border: 1px solid #ccc; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("input[type=\"text\"]"), std::string::npos)
      << "Attribute selector with quotes should match via tag name";
}

// =============================================================================
// Additional Coverage: @-moz-keyframes, exclude patterns, include patterns,
// always_include_selectors, depth limit, media prefix stripping, combinators
// =============================================================================

TEST(CriticalCssExtractorTest, IncludesMozKeyframes) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@-moz-keyframes slide { from { left: 0; } to { left: 100px; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@-moz-keyframes"), std::string::npos)
      << "@-moz-keyframes should always be included";
}

TEST(CriticalCssExtractorTest, ExcludesElementByIdPattern) {
  CriticalCssConfig config;
  config.exclude_id_patterns = {"footer", "below-fold"};
  config.max_elements = 100;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "main-content"));
  elements.push_back(MakeElement("div", 1, 1, "footer-wrapper"));

  std::string css =
      "#main-content { padding: 20px; }\n"
      "#footer-wrapper { margin-top: 50px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#main-content"), std::string::npos);
  // "footer-wrapper" matches the "footer" exclude ID pattern.
  EXPECT_EQ(result.critical_css.find("#footer-wrapper"), std::string::npos)
      << "Element with ID matching exclude_id_patterns should be excluded";
}

TEST(CriticalCssExtractorTest, ExcludesElementByTagPattern) {
  CriticalCssConfig config;
  config.exclude_tag_patterns = {"footer"};
  config.max_elements = 100;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("header", 1, 0));
  elements.push_back(MakeElement("footer", 1, 1));

  std::string css =
      "header { background: #fff; }\n"
      "footer { padding: 20px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("header"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("footer"), std::string::npos)
      << "Element with tag matching exclude_tag_patterns should be excluded";
}

TEST(CriticalCssExtractorTest, IncludesElementByClassPattern) {
  // Deep element (beyond max_depth) should be included if it matches
  // include_class_patterns.
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 0;  // No elements by index
  config.include_class_patterns = {"hero", "banner"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // This element is at depth 1 (within max_depth) but beyond max_elements.
  // Include via class pattern.
  elements.push_back(MakeElement("div", 1, 100, "", {"hero-section"}));
  // This element is beyond both limits but matches include pattern.
  elements.push_back(MakeElement("div", 1, 101, "", {"banner-top"}));

  std::string css =
      ".hero-section { min-height: 100vh; }\n"
      ".banner-top { background: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".hero-section"), std::string::npos)
      << "Element matching include_class_patterns should be included";
  EXPECT_NE(result.critical_css.find(".banner-top"), std::string::npos)
      << "Element matching include_class_patterns should be included";
}

TEST(CriticalCssExtractorTest, IncludesElementByIdPattern) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 0;
  config.include_id_patterns = {"hero", "header", "nav"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Deep element beyond max_elements but with matching include ID pattern.
  elements.push_back(MakeElement("div", 1, 100, "nav-main"));

  std::string css = "#nav-main { display: flex; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#nav-main"), std::string::npos)
      << "Element matching include_id_patterns should be included";
}

TEST(CriticalCssExtractorTest, IncludesElementByTagPatternOverride) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 0;
  config.include_tag_patterns = {"header", "nav", "main"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Beyond max_elements but matches include_tag_patterns.
  elements.push_back(MakeElement("main", 1, 100));

  std::string css = "main { padding: 20px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("main"), std::string::npos)
      << "Element matching include_tag_patterns should be included";
}

TEST(CriticalCssExtractorTest, AlwaysIncludeSelectorCommaVariant) {
  // Test always_include_selectors matching via comma-separated lists.
  CriticalCssConfig config;
  config.always_include_selectors = {"*", "html", "body", ":root"};
  config.max_elements = 0;
  config.max_depth = 0;
  CriticalCssExtractor extractor(config);

  // No elements that would match via element matching.
  std::vector<CollectedElement> elements;

  // Selector lists containing always_include_selectors:
  // "html, body" contains "html" followed by ","
  std::string css =
      "html, body { margin: 0; }\n"
      "body, div { padding: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "html," matches the pattern_lower + "," check.
  EXPECT_NE(result.critical_css.find("html, body"), std::string::npos)
      << "Selector list containing always-include pattern should be included";
}

TEST(CriticalCssExtractorTest, DepthLimitExcludesDeepElements) {
  CriticalCssConfig config;
  config.max_depth = 3;
  config.max_elements = 100;
  // Clear include patterns so only depth matters for exclusion.
  config.include_tag_patterns = {};
  config.include_class_patterns = {};
  config.include_id_patterns = {};
  config.exclude_tag_patterns = {};
  config.exclude_class_patterns = {};
  config.exclude_id_patterns = {};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 2, 0, "", {"shallow"}));
  elements.push_back(MakeElement("div", 5, 1, "", {"deep-element"}));

  std::string css =
      ".shallow { color: red; }\n"
      ".deep-element { color: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".shallow"), std::string::npos);
  EXPECT_EQ(result.critical_css.find(".deep-element"), std::string::npos)
      << "Element exceeding max_depth should be excluded";
}

TEST(CriticalCssExtractorTest, MediaAllAndPrefix) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "all and" prefix should be stripped, leaving min-width: 1024px.
  std::string css =
      "@media all and (min-width: 1024px) "
      "{ .all-prefix { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "'all and' prefix 1024px should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "'all and' prefix 1024px should be included for desktop";
}

TEST(CriticalCssExtractorTest, MaxWidthWithSpaceBeforeColon) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "max-width :" with space before colon.
  std::string css =
      "@media (max-width : 479px) { .spaced-colon { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "max-width with space before colon should be included for mobile";
  EXPECT_EQ(result_desktop.critical_css.find("@media"), std::string::npos)
      << "max-width: 479px should be excluded for desktop";
}

TEST(CriticalCssExtractorTest, MinWidthWithSpaceBeforeColon) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "min-width :" with space before colon.
  std::string css =
      "@media (min-width : 1024px) { .spaced-min { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "min-width: 1024px should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "min-width: 1024px should be included for desktop";
}

TEST(CriticalCssExtractorTest, SelectorNoMatchReturnsEmpty) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Elements that don't match any CSS selector.
  elements.push_back(MakeElement("div", 0, 0, "main", {"wrapper"}));

  // CSS with selectors that don't match the elements.
  std::string css = ".nonexistent { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_rules, 0);
  EXPECT_TRUE(result.critical_css.empty());
}

TEST(CriticalCssExtractorTest, MultipleClassesAllMustMatch) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Element with only one of the two required classes.
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));

  // Selector requires both .foo and .bar.
  std::string css = ".foo.bar { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should NOT match because element lacks class "bar".
  EXPECT_EQ(result.critical_css.find(".foo.bar"), std::string::npos)
      << "Selector requiring multiple classes should not match element with "
         "only one";
}

TEST(CriticalCssExtractorTest, MultipleClassesAllPresent) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo", "bar", "baz"}));

  std::string css = ".foo.bar { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".foo.bar"), std::string::npos)
      << "Selector should match when all required classes are present";
}

TEST(CriticalCssExtractorTest, TagAndIdCombinedNoMatch) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Element tag is "div" but selector requires "span".
  elements.push_back(MakeElement("div", 0, 0, "main"));

  std::string css = "span#main { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("span#main"), std::string::npos)
      << "Tag name mismatch should prevent match even if ID matches";
}

TEST(CriticalCssExtractorTest, IdMismatchPreventsMatch) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "sidebar"));

  std::string css = "#main { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("#main"), std::string::npos)
      << "ID mismatch should prevent match";
}

TEST(CriticalCssExtractorTest, CaseInsensitiveTagMatching) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("DIV", 0, 0));

  std::string css = "div { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div"), std::string::npos)
      << "Tag matching should be case-insensitive";
}

TEST(CriticalCssExtractorTest, ExcludeClassMultipleClasses) {
  // Element has multiple classes, one of which matches exclude pattern.
  CriticalCssConfig config;
  config.exclude_class_patterns = {"lazy"};
  config.max_elements = 100;
  config.max_depth = 10;
  config.include_tag_patterns = {};
  config.include_class_patterns = {};
  config.include_id_patterns = {};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(
      MakeElement("img", 1, 0, "", {"hero-image", "lazyload-fallback"}));

  std::string css = ".hero-image { width: 100%; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "lazyload-fallback" contains "lazy" pattern.
  EXPECT_EQ(result.critical_css.find(".hero-image"), std::string::npos)
      << "Element with any class matching exclude pattern should be excluded";
}

TEST(CriticalCssExtractorTest, ContainsPatternCaseInsensitive) {
  // Verify ContainsPattern is case-insensitive.
  CriticalCssConfig config;
  config.include_class_patterns = {"HERO"};
  config.max_depth = 10;
  config.max_elements = 0;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 100, "", {"hero-section"}));

  std::string css = ".hero-section { height: 100vh; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".hero-section"), std::string::npos)
      << "Pattern matching should be case-insensitive";
}

TEST(CriticalCssExtractorTest, EmptyNoElementsNoRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;

  std::string css = ".something { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 1);
  // No elements to match against, so no critical rules (except always-include).
  EXPECT_EQ(result.critical_rules, 0);
}

TEST(CriticalCssExtractorTest, SingleCharComment) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with multiple comments interspersed.
  std::string css =
      "/* comment 1 */\n"
      "/* comment 2 */\n"
      "body { margin: 0; }\n"
      "/* trailing comment */";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 1);
  EXPECT_EQ(result.critical_rules, 1);
}

TEST(CriticalCssExtractorTest, MediaPrintAndScreen) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // @media print, screen — contains print as word boundary.
  // This is a print rule and should be excluded.
  std::string css = "@media print, screen { body { font-size: 14pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("@media print"), std::string::npos)
      << "@media print, screen should be excluded (it's a print rule)";
}

TEST(CriticalCssExtractorTest, SingleQuoteInCssBody) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"icon"}));

  // CSS with single-quoted string containing braces.
  std::string css =
      ".icon { content: '{ icon }'; display: inline; }\n"
      ".other { padding: 5px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".icon"), std::string::npos);
  EXPECT_EQ(result.total_rules, 2);
}

TEST(CriticalCssExtractorTest, EscapedQuoteInString) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"escaped"}));

  // CSS with escaped quote inside a string.
  std::string css = R"(.escaped { content: "he said \"hello\" { }"; })";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_GE(result.total_rules, 1);
}

// =============================================================================
// Coverage: double-backslash in CSS string should not confuse block parser.
// "test\\" ends the string (the backslash is escaped, not the quote).
// =============================================================================

TEST(CriticalCssExtractorTest, DoubleBackslashInString) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"dbs"}));

  // The CSS contains a value with a double-backslash before the closing quote.
  // The parser must recognize that the quote after \\ is real (not escaped).
  std::string css = R"(.dbs { content: "test\\"; color: red; })";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_GE(result.total_rules, 1);
  // The rule should be extracted correctly despite the \\ in the string.
  EXPECT_NE(result.critical_css.find("color: red"), std::string::npos);
}

// =============================================================================
// Coverage: ch unit in media queries (unrecognized, should be conservatively
// included).
// =============================================================================

TEST(CriticalCssExtractorTest, UnrecognizedChUnitInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "ch" is not a recognized unit for ParseCssLength (only px, em, rem).
  std::string css =
      "@media (min-width: 80ch) { .char-width { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result_mobile.success);
  EXPECT_TRUE(result_desktop.success);
  // "ch" is unrecognized → conservatively included for all viewports.
  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'ch' should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'ch' should be conservatively included for desktop";
}

// =============================================================================
// Coverage: ex unit in media queries (unrecognized, should be conservatively
// included).
// =============================================================================

TEST(CriticalCssExtractorTest, UnrecognizedExUnitInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@media (max-width: 50ex) { .ex-width { display: block; } }";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'ex' should be conservatively included";
}

// =============================================================================
// Coverage: Null byte in CSS content (XSS prevention at injection layer).
// The extractor itself should handle null bytes without crashing.
// =============================================================================

TEST(CriticalCssExtractorTest, NullByteInCssContent) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with embedded null byte in the body.
  std::string css = "body { margin: 0; }";
  css.insert(css.find("margin"), 1, '\0');

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The rule should still be extracted (null byte is in the body, not selector).
  EXPECT_GE(result.total_rules, 1);
}

TEST(CriticalCssExtractorTest, NullByteInSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with null byte in the selector — should not crash.
  std::string css = std::string("bo") + '\0' + "dy { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The selector with null byte may or may not match, but must not crash.
}

// =============================================================================
// Coverage: Empty viewport (no elements at all).
// =============================================================================

TEST(CriticalCssExtractorTest, EmptyElementsWithComplexCss) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // No elements at all.

  std::string css =
      "div { color: red; }\n"
      "body { margin: 0; }\n"
      "@font-face { font-family: 'Test'; src: url('test.woff2'); }\n"
      "@keyframes fade { from { opacity: 0; } to { opacity: 1; } }\n"
      "@media (min-width: 1024px) { .wide { display: block; } }\n"
      "@media print { body { font-size: 12pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // @font-face and @keyframes should be included regardless of elements.
  EXPECT_NE(result.critical_css.find("@font-face"), std::string::npos)
      << "@font-face should always be included even with no elements";
  EXPECT_NE(result.critical_css.find("@keyframes"), std::string::npos)
      << "@keyframes should always be included even with no elements";
  // @media rules (non-print) should be included (they're @media, not element-
  // matched).
  EXPECT_NE(result.critical_css.find("@media (min-width"), std::string::npos)
      << "@media screen rules should be included";
  // @media print should be excluded.
  EXPECT_EQ(result.critical_css.find("@media print"), std::string::npos);
  // div and body are in always_include_selectors for body, but div is not.
  // Since there are no elements, div should NOT match (no element matching).
  EXPECT_EQ(result.critical_css.find("div {"), std::string::npos)
      << "div should not be included with no elements";
}

// =============================================================================
// Coverage: Malformed selector starting with special characters.
// =============================================================================

TEST(CriticalCssExtractorTest, MalformedSelectorSpecialChars) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  // Selector starting with non-standard characters.
  std::string css =
      "!!! { color: red; }\n"
      "div { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The malformed selector should not crash the parser.
  EXPECT_GE(result.total_rules, 2);
  // div should still match.
  EXPECT_NE(result.critical_css.find("div {"), std::string::npos);
}

// =============================================================================
// Coverage: Very large CSS input. Exercises performance without crashing.
// =============================================================================

TEST(CriticalCssExtractorTest, VeryLargeCssInput) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"container"}));

  // Generate a large CSS string with many rules.
  std::string css;
  for (int i = 0; i < 1000; ++i) {
    css += ".class-" + std::to_string(i) + " { color: red; }\n";
  }
  // Add a matching rule at the end.
  css += ".container { padding: 20px; }\n";
  css += "body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should have parsed all rules.
  EXPECT_EQ(result.total_rules, 1002);
  // body and .container should be included.
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".container {"), std::string::npos);
  // Most .class-N rules should not be included (no matching elements).
  EXPECT_EQ(result.critical_css.find(".class-500 {"), std::string::npos);
}

// =============================================================================
// Coverage: Media query with "screen" alone (no "and"), should be
// conservatively included for all viewports.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaScreenAloneConservativelyIncluded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@media screen { body { background: white; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result_mobile.success);
  EXPECT_TRUE(result_desktop.success);
  // "screen" alone is not parseable as min/max-width, should be conservatively
  // included.
  EXPECT_NE(result_mobile.critical_css.find("@media screen"), std::string::npos)
      << "@media screen should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media screen"),
            std::string::npos)
      << "@media screen should be conservatively included for desktop";
}

// =============================================================================
// Coverage: Media query with zero-width value (min-width: 0px).
// =============================================================================

TEST(CriticalCssExtractorTest, MediaZeroWidthMinWidth) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // min-width: 0px should match all viewports.
  std::string css = "@media (min-width: 0px) { .zero-min { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "min-width: 0px should match mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "min-width: 0px should match desktop";
}

// =============================================================================
// Coverage: Media query with max-width: 0px. Should only match nothing (0px
// viewport doesn't exist in practice), effectively excluded for all.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaZeroWidthMaxWidth) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@media (max-width: 0px) { .zero-max { display: none; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  // Mobile range is 0-479px, max-width: 0px range is 0-0px. This overlaps.
  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "max-width: 0px should overlap with mobile (0-479)";
  // Tablet is 480-1279, so 0px doesn't overlap.
  EXPECT_EQ(result_tablet.critical_css.find("@media"), std::string::npos)
      << "max-width: 0px should not overlap with tablet (480-1279)";
  EXPECT_EQ(result_desktop.critical_css.find("@media"), std::string::npos)
      << "max-width: 0px should not overlap with desktop (1280+)";
}

// =============================================================================
// Coverage: Selector with only a pseudo-class (e.g., ":root").
// =============================================================================

TEST(CriticalCssExtractorTest, PseudoClassOnlySelectorRoot) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));

  std::string css = ":root { --primary: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // :root is in always_include_selectors.
  EXPECT_NE(result.critical_css.find(":root"), std::string::npos)
      << ":root should always be included";
}

// =============================================================================
// Coverage: always_include_selectors comma variant — ", body" (with space).
// =============================================================================

TEST(CriticalCssExtractorTest, AlwaysIncludeSelectorCommaSpaceVariant) {
  CriticalCssConfig config;
  config.always_include_selectors = {"*", "html", "body", ":root"};
  config.max_elements = 0;
  config.max_depth = 0;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;

  // Selector with ", body" pattern.
  std::string css = "div, body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should be included because "body" appears after ", ".
  EXPECT_NE(result.critical_css.find("div, body"), std::string::npos)
      << "Selector list with ', body' should match always_include_selectors";
}

// =============================================================================
// Coverage: always_include_selectors comma variant — ",body" (no space).
// =============================================================================

TEST(CriticalCssExtractorTest, AlwaysIncludeSelectorCommaNoSpaceVariant) {
  CriticalCssConfig config;
  config.always_include_selectors = {"*", "html", "body", ":root"};
  config.max_elements = 0;
  config.max_depth = 0;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;

  // Selector with ",body" (no space after comma).
  std::string css = "div,body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div,body"), std::string::npos)
      << "Selector list with ',body' should match always_include_selectors";
}

// =============================================================================
// Coverage: Element with empty ID (should not match ID patterns).
// =============================================================================

TEST(CriticalCssExtractorTest, ElementWithEmptyIdSkipsIdPatternCheck) {
  CriticalCssConfig config;
  config.exclude_id_patterns = {"footer"};
  config.max_elements = 100;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Element with empty ID should not trigger ID pattern matching.
  elements.push_back(MakeElement("div", 1, 0));

  std::string css = "div { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div"), std::string::npos)
      << "Element with empty ID should not be excluded by ID patterns";
}

// =============================================================================
// Coverage: CSS with only comments (no rules).
// =============================================================================

TEST(CriticalCssExtractorTest, OnlyComments) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "/* comment 1 */ /* comment 2 */";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 0);
  EXPECT_TRUE(result.critical_css.empty());
}

// =============================================================================
// Coverage: CSS with unclosed comment.
// =============================================================================

TEST(CriticalCssExtractorTest, UnclosedComment) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "body { margin: 0; } /* unclosed comment";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The first rule should be parsed correctly.
  EXPECT_GE(result.total_rules, 1);
  EXPECT_NE(result.critical_css.find("body"), std::string::npos);
}

TEST(CriticalCssExtractorTest, UnclosedCommentDoesNotSilentlyDropRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("h1", 0, 0));

  // Rule before the unterminated comment should be preserved.
  // Rule after should NOT appear (comment consumes it via npos).
  std::string css = "body { margin: 0; } /* unterminated h1 { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body"), std::string::npos);
  // h1 rule is inside the unterminated comment — must NOT be extracted.
  EXPECT_EQ(result.critical_css.find("h1"), std::string::npos);
}

// =============================================================================
// Coverage: Element deep in DOM exceeding both max_elements AND max_depth,
// but matching include pattern — should still be included.
// =============================================================================

TEST(CriticalCssExtractorTest, DeepElementIncludedByPattern) {
  CriticalCssConfig config;
  config.max_depth = 3;
  config.max_elements = 2;
  config.include_class_patterns = {"hero"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Beyond max_elements AND within max_depth — included by index/depth OR pattern.
  elements.push_back(MakeElement("div", 1, 0, "", {"normal"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"normal2"}));
  // Beyond max_elements (index=100) but within max_depth (depth=2) — included.
  elements.push_back(MakeElement("div", 2, 100, "", {"hero-banner"}));

  std::string css =
      ".normal { color: red; }\n"
      ".normal2 { color: blue; }\n"
      ".hero-banner { min-height: 100vh; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // All three should be included (first two by index, third by pattern).
  EXPECT_NE(result.critical_css.find(".normal {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".hero-banner"), std::string::npos)
      << "Deep element matching include_class_patterns should be included";
}

// =============================================================================
// Coverage: SimpleSelectorMatchesElement with empty selector.
// =============================================================================

TEST(CriticalCssExtractorTest, EmptyIndividualSelectorInList) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("span", 0, 0));

  // Multiple commas with empty segments.
  std::string css = ",,span,, { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "span" in the selector list should still match.
  EXPECT_NE(result.critical_css.find("span"), std::string::npos)
      << "span should match even with empty selector segments";
}

// =============================================================================
// Coverage: Media query with min-width exactly at viewport boundary.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaQueryAtExactBoundary) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Mobile range is 0-479px.
  // min-width: 480px should exclude mobile, include tablet (480-1023).
  std::string css =
      "@media (min-width: 480px) { .tablet-up { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "min-width: 480px should exclude mobile (max 479)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "min-width: 480px should include tablet (starts at 480)";
}

// =============================================================================
// Coverage: Media query with max-width at exact desktop boundary.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaQueryMaxWidthAtDesktopBoundary) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // max-width: 1023px should include tablet (480-1279), exclude desktop (1280+).
  std::string css =
      "@media (max-width: 1023px) { .not-desktop { display: block; } }";

  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "max-width: 1023px should include tablet";
  EXPECT_EQ(result_desktop.critical_css.find("@media"), std::string::npos)
      << "max-width: 1023px should exclude desktop (starts at 1280)";
}

// =============================================================================
// Coverage: Media query with fractional rem value.
// =============================================================================

TEST(CriticalCssExtractorTest, HandlesFractionalRemInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 30.5rem * 16 = 488px — should include tablet (480-1023) but not mobile.
  std::string css =
      "@media (min-width: 30.5rem) { .half-rem { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "30.5rem (488px) should exclude mobile (0-479)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "30.5rem (488px) should include tablet (480-1023)";
}

// =============================================================================
// Coverage: Selector with tag name containing hyphens (custom elements).
// =============================================================================

TEST(CriticalCssExtractorTest, CustomElementTagWithHyphens) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("my-component", 0, 0));

  std::string css = "my-component { display: block; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("my-component"), std::string::npos)
      << "Custom element with hyphens should match";
}

// =============================================================================
// Coverage: Selector with underscore in class name.
// =============================================================================

TEST(CriticalCssExtractorTest, UnderscoreInClassName) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"my_class_name"}));

  std::string css = ".my_class_name { padding: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".my_class_name"), std::string::npos)
      << "Class with underscores should match";
}

// =============================================================================
// Coverage: Multiple @media print variants should all be excluded.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaPrintWithCommaAndOther) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "@media screen, print" — contains "print" at word boundary → excluded.
  std::string css = "@media screen, print { body { font-size: 14pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("@media"), std::string::npos)
      << "@media screen, print should be excluded (contains 'print')";
}

// =============================================================================
// Coverage: is_critical logic — element at index < max_elements but
// depth > max_depth. The OR logic means it should still be included
// (element_index < max_elements OR depth <= max_depth OR IsElementIncluded).
// =============================================================================

TEST(CriticalCssExtractorTest, ElementCriticalByIndexDespiteDeepDepth) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 10;
  // Clear include/exclude patterns.
  config.include_tag_patterns = {};
  config.include_class_patterns = {};
  config.include_id_patterns = {};
  config.exclude_tag_patterns = {};
  config.exclude_class_patterns = {};
  config.exclude_id_patterns = {};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Index 0 (< max_elements=10), depth 5 (> max_depth=2).
  // But depth > max_depth triggers IsElementExcluded → returns true.
  // So this element IS excluded due to depth.
  elements.push_back(MakeElement("div", 5, 0, "", {"deep-but-first"}));

  std::string css = ".deep-but-first { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Element at depth 5 > max_depth 2 → IsElementExcluded returns true.
  EXPECT_EQ(result.critical_css.find(".deep-but-first"), std::string::npos)
      << "Element exceeding max_depth should be excluded even if index < "
         "max_elements";
}

TEST(CriticalCssExtractorTest, ElementBeyondMaxButIncludedByPattern) {
  CriticalCssConfig config;
  config.max_elements = 2;
  config.max_depth = 10;
  config.include_class_patterns = {"hero"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "", {"first"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"second"}));
  // Index 5 > max_elements(2), but "hero-banner" matches include pattern.
  elements.push_back(MakeElement("section", 1, 5, "", {"hero-banner"}));

  std::string css =
      ".first { color: red; }\n"
      ".second { color: blue; }\n"
      ".hero-banner { min-height: 100vh; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".first {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".second {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".hero-banner {"), std::string::npos)
      << "Element beyond max_elements but matching include pattern should be "
         "included";
}

TEST(CriticalCssExtractorTest, ElementBeyondMaxNotIncludedWithoutPattern) {
  CriticalCssConfig config;
  config.max_elements = 2;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "", {"first"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"second"}));
  // Index 5 > max_elements(2) and not matching any include pattern.
  elements.push_back(MakeElement("section", 1, 5, "", {"footer"}));

  std::string css =
      ".first { color: red; }\n"
      ".second { color: blue; }\n"
      ".footer { margin-top: 40px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".first {"), std::string::npos);
  // Element at index 5 should NOT be critical (beyond max and no include).
  EXPECT_EQ(result.critical_css.find(".footer {"), std::string::npos)
      << "Element beyond max_elements without include pattern should be "
         "excluded";
}

// ========== @layer / @supports container at-rule tests ==========

TEST(CriticalCssExtractorTest, LayerWithMatchingInnerRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"btn"}));

  std::string css =
      "@layer utilities {\n"
      "  .btn { color: red; }\n"
      "  .hidden { display: none; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer utilities"), std::string::npos)
      << "@layer wrapper should be present";
  EXPECT_NE(result.critical_css.find(".btn {"), std::string::npos)
      << "Matching inner rule should be included";
  EXPECT_EQ(result.critical_css.find(".hidden {"), std::string::npos)
      << "Non-matching inner rule should be excluded";
}

TEST(CriticalCssExtractorTest, LayerWithNoMatchingInnerRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer utilities {\n"
      "  .hidden { display: none; }\n"
      "  .invisible { visibility: hidden; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("@layer"), std::string::npos)
      << "Empty @layer block (no surviving inner rules) should be omitted";
}

TEST(CriticalCssExtractorTest, NestedLayerAndSupports) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"grid"}));

  // Tailwind v4 pattern: @layer { @supports { selector { ... } } }
  std::string css =
      "@layer utilities {\n"
      "  @supports (display: grid) {\n"
      "    .grid { display: grid; }\n"
      "  }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer utilities"), std::string::npos)
      << "Outer @layer should be present";
  EXPECT_NE(result.critical_css.find("@supports (display: grid)"),
            std::string::npos)
      << "Inner @supports should be present";
  EXPECT_NE(result.critical_css.find(".grid {"), std::string::npos)
      << "Matching inner rule should be included";
}

TEST(CriticalCssExtractorTest, MultipleLayerBlocks) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));
  elements.push_back(MakeElement("nav", 2, 2));

  std::string css =
      "@layer theme {\n"
      "  :root { --color-primary: blue; }\n"
      "}\n"
      "@layer base {\n"
      "  body { margin: 0; }\n"
      "}\n"
      "@layer utilities {\n"
      "  .hidden { display: none; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer theme"), std::string::npos)
      << "@layer theme should be present (contains :root)";
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "@layer base should be present (contains body)";
  EXPECT_EQ(result.critical_css.find("@layer utilities"), std::string::npos)
      << "@layer utilities should be omitted (no matching rules)";
}

TEST(CriticalCssExtractorTest, FontFaceInsideLayer) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer base {\n"
      "  @font-face { font-family: 'Inter'; src: url('inter.woff2'); }\n"
      "  .hidden { display: none; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "@layer should be present (contains @font-face)";
  EXPECT_NE(result.critical_css.find("@font-face"), std::string::npos)
      << "@font-face inside @layer should be always included";
  EXPECT_EQ(result.critical_css.find(".hidden {"), std::string::npos)
      << "Non-matching rule inside @layer should be excluded";
}

TEST(CriticalCssExtractorTest, MediaPrintInsideLayer) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer base {\n"
      "  @media print { body { font-size: 12pt; } }\n"
      "  body { margin: 0; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "@layer should be present (contains body)";
  EXPECT_EQ(result.critical_css.find("@media print"), std::string::npos)
      << "@media print inside @layer should be excluded";
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos)
      << "body rule inside @layer should be included";
}

TEST(CriticalCssExtractorTest, RecursionDepthBounded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Create deeply nested @layer blocks (7 levels deep, beyond limit of 5)
  std::string css =
      "@layer a { @layer b { @layer c { @layer d { @layer e { @layer f {"
      " body { color: red; }"
      " } } } } } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // At depth 5, recursion stops — innermost layers are dropped
  EXPECT_EQ(result.critical_css.find("color: red"), std::string::npos)
      << "Rules beyond recursion depth should be dropped";
}

TEST(CriticalCssExtractorTest, LayerWithViewportMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"container"}));

  std::string css =
      "@layer utilities {\n"
      "  .container { max-width: 100%; }\n"
      "  @media (min-width: 1024px) { .container { max-width: 1200px; } }\n"
      "}";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  // Both viewports should include .container
  EXPECT_NE(result_mobile.critical_css.find("max-width: 100%"),
            std::string::npos);
  EXPECT_NE(result_desktop.critical_css.find("max-width: 100%"),
            std::string::npos);

  // Only desktop should include the @media block
  EXPECT_EQ(result_mobile.critical_css.find("max-width: 1200px"),
            std::string::npos)
      << "Desktop-only @media inside @layer should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("max-width: 1200px"),
            std::string::npos)
      << "Desktop-only @media inside @layer should be included for desktop";
}

TEST(CriticalCssExtractorTest, StandaloneSupportsBlock) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  std::string css =
      "@supports (display: grid) {\n"
      "  div { display: grid; }\n"
      "  .unknown-class { color: blue; }\n"
      "}\n";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  // div matches, .unknown-class does not → only div survives
  EXPECT_NE(result.critical_css.find("@supports (display: grid)"),
            std::string::npos);
  EXPECT_NE(result.critical_css.find("display: grid"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("unknown-class"), std::string::npos);
}

TEST(CriticalCssExtractorTest, AnonymousLayerBlock) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer {\n"
      "  body { margin: 0; }\n"
      "}\n";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer"), std::string::npos);
  EXPECT_NE(result.critical_css.find("margin: 0"), std::string::npos);
}

TEST(CriticalCssExtractorTest, ContainerAtRuleWordBoundary) {
  // "@layerX" should NOT be treated as a container at-rule.
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  std::string css =
      "@layerX {\n"
      "  div { color: red; }\n"
      "}\n";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  // @layerX is not a known at-rule, so it should NOT be recursed into
  // and should not match any element selector check.
  // The selector "@layerX" won't match ShouldIncludeSelector, so it
  // should be dropped entirely.
  EXPECT_EQ(result.critical_css.find("color: red"), std::string::npos);
}

TEST(CriticalCssExtractorTest, EscapedColonInClassSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  std::string css = R"(.lg\:grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\:grid {)"), std::string::npos)
      << "Escaped colon in class selector should match element with class "
         "'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, EscapedBracketsInSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"w-[100px]"}));

  std::string css = R"(.w-\[100px\] { width: 100px; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.w-\[100px\] {)"), std::string::npos)
      << "Escaped brackets in class selector should match element with class "
         "'w-[100px]'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, MultipleEscapedClasses) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid-cols-[240px]"}));

  std::string css =
      R"(.lg\:grid-cols-\[240px\] { grid-template-columns: 240px; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\:grid-cols-\[240px\] {)"),
            std::string::npos)
      << "Multiple escaped characters in a single class selector should match";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, EscapedBackslash) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"class\\name"}));

  std::string css = R"(.class\\name { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.class\\name {)"), std::string::npos)
      << "Double backslash in selector should match element with literal "
         "backslash in class name";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, NoEscapeNeeded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"normal-class"}));

  std::string css = ".normal-class { color: blue; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".normal-class {"), std::string::npos)
      << "Normal class selectors without escapes should still work";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, EscapedSelectorDoesNotMatchPartialClass) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Element has class "lg" (no colon), NOT "lg:grid".
  elements.push_back(MakeElement("div", 0, 0, "", {"lg"}));

  std::string css = R"(.lg\:grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(R"(.lg\:grid {)"), std::string::npos)
      << "Escaped selector .lg\\:grid should NOT match element with class 'lg'";
  EXPECT_EQ(0, result.critical_rules);
}

TEST(CriticalCssExtractorTest, CompoundEscapedAndNormalClasses) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid", "mt-4"}));

  // Compound selector: both classes must match.
  std::string css = R"(.lg\:grid.mt-4 { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\:grid.mt-4 {)"), std::string::npos)
      << "Compound escaped+normal class selector should match element with "
         "both classes";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HoverPrefixEscapedSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"hover:bg-red-500"}));

  std::string css = R"(.hover\:bg-red-500 { background: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.hover\:bg-red-500 {)"),
            std::string::npos)
      << "Escaped hover prefix selector should match";
  EXPECT_EQ(1, result.critical_rules);
}

// =============================================================================
// CSS hex escape tests
// =============================================================================

TEST(CriticalCssExtractorTest, HexEscapeColonInClassSelectorNoSpace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \3agrid — no space after hex digits, 'g' is a hex digit so it becomes
  // part of the codepoint.  Use \3a grid (with space consumed) instead.
  // This test uses \3a grid where the space is consumed by the hex escape.
  std::string css = R"(.lg\3a grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3a grid {)"), std::string::npos)
      << "Hex escape \\3a with consumed trailing space should match class "
         "'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeColonInClassSelectorNoTrailingSpace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \3agrid — 'g' is a hex digit, so this actually parses as codepoint 0x3AG..
  // Wait, 'r' is not hex. So \3a consumes '3' and 'a', stops at 'g'... no,
  // 'g' is NOT a hex digit. So \3a stops, no trailing space, then 'grid'.
  std::string css = R"(.lg\3agrid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3agrid {)"), std::string::npos)
      << "Hex escape \\3a immediately followed by non-hex 'grid' should match "
         "class 'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeBracketInSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"w-[100px]"}));

  // \5b = '[' (U+005B), \5d = ']' (U+005D).
  // Two spaces before { — first consumed by \5d hex escape, second is the
  // separator.  The parser trims whitespace, so output has one space before {.
  std::string css = R"(.w-\5b 100px\5d  { width: 100px; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.w-\5b 100px\5d {)"), std::string::npos)
      << "Hex escapes \\5b and \\5d should match class 'w-[100px]'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeUpperCase) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \3A (uppercase) should work the same as \3a
  std::string css = R"(.lg\3A grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3A grid {)"), std::string::npos)
      << "Uppercase hex escape \\3A should match class 'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeSixDigits) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \00003a = ':' (U+003A) — full 6 hex digits, no trailing space needed
  std::string css = R"(.lg\00003agrid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\00003agrid {)"), std::string::npos)
      << "Six-digit hex escape \\00003a should match class 'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, MixedHexAndLiteralEscapes) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid-cols-[240px]"}));

  // \3a for colon (hex escape), \[ and \] for brackets (literal escapes)
  std::string css = R"(.lg\3a grid-cols-\[240px\] { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3a grid-cols-\[240px\] {)"),
            std::string::npos)
      << "Mixed hex (\\3a) and literal (\\[, \\]) escapes should match class "
         "'lg:grid-cols-[240px]'";
  EXPECT_EQ(1, result.critical_rules);
}

// =============================================================================
// @media wholesale inclusion threshold tests (inside @layer)
// =============================================================================

TEST(CriticalCssExtractorTest, SmallMediaBlockIncludedWholesale) {
  // A small @media block inside @layer should be included entirely when the
  // viewport overlaps, without per-rule filtering.
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"container"}));
  elements.push_back(MakeElement("span", 1, 1, "", {"badge"}));

  // ~200 bytes — well under the 4096 default threshold.
  std::string css =
      "@layer utilities {\n"
      "  @media (min-width: 64rem) {\n"
      "    .container { max-width: 1200px; }\n"
      "    .badge { font-size: 0.75rem; }\n"
      "    .no-match-class { color: red; }\n"
      "  }\n"
      "}";

  // Tablet viewport overlaps min-width:64rem (1024px).
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  // The entire @media block should be included wholesale — even .no-match-class
  // which does not match any collected element.
  EXPECT_NE(result.critical_css.find("max-width: 1200px"), std::string::npos)
      << ".container rule should be present";
  EXPECT_NE(result.critical_css.find("font-size: 0.75rem"), std::string::npos)
      << ".badge rule should be present";
  EXPECT_NE(result.critical_css.find("no-match-class"), std::string::npos)
      << ".no-match-class should be present (wholesale inclusion)";
}

TEST(CriticalCssExtractorTest, LargeMediaBlockFilteredPerRule) {
  // A large @media block inside @layer should be recursed into and
  // filtered per-rule when it exceeds the wholesale size threshold.
  CriticalCssConfig config;
  config.max_wholesale_media_bytes = 256;  // Low threshold to trigger filtering
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"target"}));

  // Build a large @media block (>256 bytes) with many rules, only one of
  // which matches a collected element.
  std::string inner_rules = "    .target { color: green; }\n";
  for (int i = 0; i < 30; ++i) {
    inner_rules += "    .filler-class-" + std::to_string(i) +
                   " { padding: " + std::to_string(i) + "px; }\n";
  }

  std::string css =
      "@layer utilities {\n"
      "  @media (min-width: 64rem) {\n" +
      inner_rules +
      "  }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  // The matching rule should be present.
  EXPECT_NE(result.critical_css.find(".target"), std::string::npos)
      << ".target rule should survive per-rule filtering";
  EXPECT_NE(result.critical_css.find("color: green"), std::string::npos);
  // The @media wrapper should be present.
  EXPECT_NE(result.critical_css.find("@media (min-width: 64rem)"),
            std::string::npos)
      << "@media wrapper should be present around filtered rules";
  // Non-matching filler rules should be excluded.
  EXPECT_EQ(result.critical_css.find("filler-class-"), std::string::npos)
      << "Non-matching filler rules should be excluded by per-rule filtering";
}

TEST(CriticalCssExtractorTest, MediaBlockOutsideLayerAlwaysWholesale) {
  // A large @media block NOT inside @layer should always be included
  // wholesale, regardless of size.
  CriticalCssConfig config;
  config.max_wholesale_media_bytes = 256;  // Low threshold
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"target"}));

  // Build a large @media block (>256 bytes) at top level (not inside @layer).
  std::string inner_rules = "    .target { color: green; }\n";
  for (int i = 0; i < 30; ++i) {
    inner_rules += "    .filler-class-" + std::to_string(i) +
                   " { padding: " + std::to_string(i) + "px; }\n";
  }

  std::string css = "@media (min-width: 64rem) {\n" + inner_rules + "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  // The entire block should be included wholesale — even non-matching rules.
  EXPECT_NE(result.critical_css.find(".target"), std::string::npos)
      << ".target should be present";
  EXPECT_NE(result.critical_css.find("filler-class-0"), std::string::npos)
      << "Non-matching filler rules should be present (wholesale, not inside "
         "@layer)";
  EXPECT_NE(result.critical_css.find("filler-class-29"), std::string::npos)
      << "Last filler rule should be present (wholesale inclusion)";
}

TEST(CriticalCssExtractorTest, HexEscapeSpaceNotTreatedAsCombinator) {
  // The selector `div .lg\3a grid` has a real combinator space (between `div`
  // and `.lg\3a grid`) and a hex-escape space (after `\3a`).  Only the
  // combinator space should split the selector.
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  std::string css = R"(div .lg\3a grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(div .lg\3a grid {)"), std::string::npos)
      << "Descendant selector with hex escape should match element";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, SupportsInsideLayerPreservesInsideLayer) {
  // @supports nested inside @layer should still propagate inside_layer=true,
  // so large @media blocks within are filtered per-rule.
  CriticalCssConfig config;
  config.max_wholesale_media_bytes = 128;  // Low threshold
  CriticalCssExtractor extractor(config);
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"target"}));

  // Build a >128-byte @media block inside @supports inside @layer.
  std::string media_body;
  media_body += "    .target { color: green; }\n";
  for (int i = 0; i < 10; ++i) {
    media_body += "    .filler-" + std::to_string(i) +
                  " { padding: " + std::to_string(i) + "px; }\n";
  }
  std::string css =
      "@layer utilities {\n"
      "  @supports (display: grid) {\n"
      "    @media (min-width: 64rem) {\n" +
      media_body +
      "    }\n"
      "  }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".target"), std::string::npos)
      << ".target should be included via per-rule filtering";
  EXPECT_EQ(result.critical_css.find("filler-"), std::string::npos)
      << "Non-matching filler rules should be excluded (per-rule filtering "
         "inside @supports inside @layer)";
}

// --- Tailwind @custom-variant :where() tests ---

TEST(CriticalCssExtractorTest, TailwindDarkVariantWithWhere) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // The HTML element has class "dark:bg-stone-950" which Tailwind escapes
  // as dark\:bg-stone-950 in the CSS selector.
  elements.push_back(
      MakeElement("header", 0, 0, "", {"bg-white", "dark:bg-stone-950"}));

  std::string css = R"(
    .bg-white { background-color: white; }
    .dark\:bg-stone-950:where(.dark,.dark *) { background-color: #0c0a09; }
  )";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("bg-white"), std::string::npos)
      << "bg-white should be in critical CSS";
  EXPECT_NE(result.critical_css.find("dark\\:bg-stone-950"), std::string::npos)
      << "dark:bg-stone-950 variant with :where() should be in critical CSS";
  EXPECT_EQ(2, result.critical_rules);
}

TEST(CriticalCssExtractorTest, TailwindDarkVariantWithWhereAndSpace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"dark:text-stone-100"}));

  // Spaces inside :where() must not be treated as descendant combinators.
  std::string css =
      R"(.dark\:text-stone-100:where(.dark, .dark *) { color: #f5f5f4; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("dark\\:text-stone-100"),
            std::string::npos)
      << "Space inside :where() should not split the selector";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, NestedParenthesesInWhere) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));

  // Nested parentheses: :where(:not(.bar .baz)) has spaces inside nested parens.
  std::string css = R"(.foo:where(:not(.bar .baz)) { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".foo:where"), std::string::npos)
      << "Nested parentheses in :where(:not()) should not break selector "
         "parsing";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, DeeplyNestedParensInWhere) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"x"}));

  // Triple-nested: :where(:not(:is(.a .b))) — spaces at depth 3.
  std::string css = R"(.x:where(:not(:is(.a .b))) { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".x:where"), std::string::npos)
      << "Triple-nested parentheses should not break selector parsing";
  EXPECT_EQ(1, result.critical_rules);
}

// Regression for issue #258: a rule whose ONLY matchable token lives inside a
// `:where()` wrapper (no matchable outer simple selector) was dropped because
// the matcher broke at the leading ':' before parsing the inner alternatives.
// `:where()` is a zero-specificity scoping hint, not a gating predicate, so the
// inner selector list must be desugared and the rule retained when an inner
// alternative matches.
TEST(CriticalCssExtractorTest, WhereWrapsTheOnlyMatchableSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"bg-white"}));

  std::string css = R"(:where(.bg-white) { background-color: white; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(":where(.bg-white)"), std::string::npos)
      << ":where(.bg-white) should match a .bg-white element and be retained";
  EXPECT_EQ(1, result.critical_rules);
}

// Issue #258: the core dark-mode case, exactly as reported. Tailwind v4's
// `@custom-variant dark` compiles `dark:bg-stone-900` to a rule scoped by
// `:where(.dark, .dark *)`. The card element carries the literal utility class
// `dark:bg-stone-900`, but the `.dark` scope is set on <html> by a client-side
// script that has NOT run when the DOM is sampled. So `:where(.dark, .dark *)`
// matches nothing in the sample. Per the issue, `:where()` is a zero-specificity
// scoping hint, not a gating predicate: for `A:where(B)`, the rule must be
// retained when `A` matches a sampled element, regardless of `B`. The dark rule
// must therefore land in the critical CSS so a dark site does not flash light.
TEST(CriticalCssExtractorTest, WhereDarkScopeIsNonConstraining) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // The card element carries both the light and dark utility classes (as
  // Tailwind emits them), but NOT `.dark` — that scope is applied to <html>
  // post-sample by client JS, so `:where(.dark, .dark *)` does not match here.
  elements.push_back(
      MakeElement("div", 0, 0, "", {"bg-white", "dark:bg-stone-900"}));

  std::string css = R"(
    .bg-white { background-color: var(--color-white); }
    .dark\:bg-stone-900:where(.dark, .dark *) { background-color: #1c1917; }
  )";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".bg-white"), std::string::npos)
      << "light-mode rule should be in critical CSS";
  EXPECT_NE(result.critical_css.find("dark\\:bg-stone-900"), std::string::npos)
      << "dark `:where()`-scoped rule must be retained (where() is "
         "non-constraining) to avoid a light flash on dark sites";
  EXPECT_EQ(2, result.critical_rules);
}

// Issue #258: `:is(...)` shares `:where()`'s desugaring path. A leading
// `:is(.hero, .banner)` with no outer simple selector must be retained when an
// inner alternative matches a sampled element.
TEST(CriticalCssExtractorTest, IsWrapsTheOnlyMatchableSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("section", 0, 0, "", {"banner"}));

  std::string css = R"(:is(.hero, .banner) { padding: 2rem; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(":is(.hero, .banner)"), std::string::npos)
      << ":is() inner alternative match should retain the rule";
  EXPECT_EQ(1, result.critical_rules);
}

// Issue #258 guard: `:where()`/`:is()` must remain non-constraining and must
// NOT cause false retention. A rule whose only matchable token is inside a
// `:where()` that matches NOTHING in the sampled DOM should still be dropped.
TEST(CriticalCssExtractorTest, WhereWithNoInnerMatchIsDropped) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"bg-white"}));

  std::string css = R"(:where(.never-present) { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(".never-present"), std::string::npos)
      << ":where() wrapping an unmatched selector must not be retained";
  EXPECT_EQ(0, result.critical_rules);
}

// ---------------------------------------------------------------------------
// Async-CSS FOUC sufficiency gate (CriticalCssIsSufficient).
// ---------------------------------------------------------------------------

// All cases below pass external_css_unresolved=false (every declared external
// sheet was resolved from cache) unless they specifically exercise the
// cold-cache fail-safe.
constexpr bool kResolved = false;
constexpr bool kUnresolved = true;

TEST(AsyncCssSufficiencyTest, ThinCriticalOverLargeSheetIsInsufficient) {
  // The live FOUC regression: ~830 B of critical CSS deferring a ~110 KB sheet
  // (~0.0075 coverage) must NOT defer.
  AsyncCssSufficiencyConfig cfg;  // defaults: 0.10 floor, 15000 byte escape
  EXPECT_FALSE(CriticalCssIsSufficient(-1.0f, 830, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, OptimisticProfileCoverageCannotOverrideBytes) {
  // The 2026-07-04 production FOUC (#879): the browser profile claimed
  // coverage=0.39 while the EXTRACTED critical CSS was 830 B against a 115 KB
  // sheet (~0.007). The optimistic profile number must never authorize
  // deferral on its own — the byte ratio is a hard floor.
  AsyncCssSufficiencyConfig cfg;  // defaults: 0.10 floor
  EXPECT_FALSE(CriticalCssIsSufficient(0.39f, 830, 115000, kResolved, cfg));
  EXPECT_FALSE(CriticalCssIsSufficient(0.40f, 830, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, PessimisticProfileCoverageStillGates) {
  AsyncCssSufficiencyConfig cfg;
  // Known LOW coverage -> insufficient even when the byte ratio looks healthy
  // (coverage is trusted downward: a profile that measured most rules unused
  // can veto a byte-fat critical block).
  EXPECT_FALSE(CriticalCssIsSufficient(0.05f, 40000, 110000, kResolved, cfg));
  // Both healthy -> sufficient.
  EXPECT_TRUE(CriticalCssIsSufficient(0.40f, 40000, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, SmallDeferredSheetAlwaysSufficient) {
  // A genuinely small CACHED/RESOLVED sheet has a trivial FOUC window.
  AsyncCssSufficiencyConfig cfg;  // 15000 byte escape hatch
  EXPECT_TRUE(CriticalCssIsSufficient(-1.0f, 500, 4096, kResolved, cfg));
  EXPECT_TRUE(CriticalCssIsSufficient(0.01f, 500, 4096, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, ColdCacheUnresolvedIsInsufficient) {
  // The iispeed.com FOUC: a declared external <link> is NOT yet in cache, so
  // the deferred byte count collapsed to the ~179 B inline-only blob — which
  // would otherwise trip the small-sheet escape hatch and defer the unmeasured
  // 39 KB sheet. external_css_unresolved must dominate and refuse to defer,
  // regardless of how small/large the (wrong) byte counts or coverage look.
  AsyncCssSufficiencyConfig cfg;
  EXPECT_FALSE(CriticalCssIsSufficient(-1.0f, 179, 179, kUnresolved, cfg));
  // Even a (falsely) healthy-looking ratio or large bytes cannot override it.
  EXPECT_FALSE(CriticalCssIsSufficient(0.95f, 50000, 60000, kUnresolved, cfg));
  // Floor-disabled gate does NOT bypass the cold-cache fail-safe.
  cfg.min_coverage_ratio = 0.0f;
  EXPECT_FALSE(CriticalCssIsSufficient(-1.0f, 1, 1000000, kUnresolved, cfg));
}

TEST(AsyncCssSufficiencyTest, RichCriticalIsSufficient) {
  AsyncCssSufficiencyConfig cfg;
  EXPECT_TRUE(
      CriticalCssIsSufficient(-1.0f, 40000, 110000, kResolved, cfg));  // ~0.36
}

TEST(AsyncCssSufficiencyTest, FloorOfZeroDisablesGate) {
  AsyncCssSufficiencyConfig cfg;
  cfg.min_coverage_ratio = 0.0f;
  EXPECT_TRUE(CriticalCssIsSufficient(-1.0f, 1, 1000000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, HeuristicFallbackDerivesRatioFromBytes) {
  AsyncCssSufficiencyConfig cfg;  // 0.10 floor
  EXPECT_FALSE(
      CriticalCssIsSufficient(-1.0f, 800, 100000, kResolved, cfg));  // 0.008
  EXPECT_TRUE(
      CriticalCssIsSufficient(-1.0f, 10000, 100000, kResolved, cfg));  // 0.10
}

TEST(AsyncCssSufficiencyTest, NaNCoverageFallsBackToByteRatio) {
  AsyncCssSufficiencyConfig cfg;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(CriticalCssIsSufficient(nan, 800, 100000, kResolved, cfg));
  EXPECT_TRUE(CriticalCssIsSufficient(nan, 20000, 100000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, DoesNotFightHighEndDegradedGuard) {
  // The over-report (coverage near 1.0) is handled by LooksLikeDegradedProfile,
  // not here: a high ratio is "sufficient" from this gate's perspective.
  AsyncCssSufficiencyConfig cfg;
  EXPECT_TRUE(CriticalCssIsSufficient(0.99f, 109000, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, ZeroDeferredBytesIsSufficient) {
  AsyncCssSufficiencyConfig cfg;
  // 0-byte sheet with a substantial floor and unknown coverage: avoid div-by-0.
  cfg.min_deferred_css_bytes = 0;
  EXPECT_TRUE(CriticalCssIsSufficient(-1.0f, 0, 0, kResolved, cfg));
}

// ---------------------------------------------------------------------------
// Issue #1056: DOM-matched retention of used above-the-fold utility rules.
// ---------------------------------------------------------------------------

// A state-conditional dark variant scoped by `:where(.dark, .dark *)` is
// retained when the element literally carries the variant class, even though
// the `.dark` scope is never active in a static, JS-off, light-mode sample.
TEST(CriticalCssExtractorTest, DomMatchedRetainsDarkVariantOnAboveFoldElement) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"dark:bg-stone-900"}));

  std::string css =
      R"(.dark\:bg-stone-900:where(.dark,.dark *) { background-color: #1c1917; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("dark\\:bg-stone-900"), std::string::npos)
      << "dark variant on an above-the-fold element must be retained";
  EXPECT_EQ(1, result.critical_rules);
}

// A skip-link's `focus:not-sr-only` variant (and the base `sr-only`) is retained
// when the element carries both classes: the `:focus` pseudo is a non-gating
// constraint for retention, and the escaped-colon variant class DOM-matches.
TEST(CriticalCssExtractorTest, DomMatchedRetainsFocusVariantSkipLink) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(
      MakeElement("a", 0, 0, "", {"sr-only", "focus:not-sr-only"}));

  std::string css =
      ".sr-only { position: absolute; width: 1px; height: 1px; }\n"
      ".focus\\:not-sr-only:focus { position: static; width: auto; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".sr-only"), std::string::npos)
      << "sr-only base rule must be retained";
  EXPECT_NE(result.critical_css.find("focus\\:not-sr-only"), std::string::npos)
      << "focus:not-sr-only variant must be retained for the skip link";
  EXPECT_EQ(2, result.critical_rules);
}

// State-unconditional Tailwind v4 utilities (.flex/.h-16/.text-sm) inside
// `@layer utilities` are retained, the @layer wrapper preserved, output balanced.
TEST(CriticalCssExtractorTest, DomMatchedRetainsStateUnconditionalUtilities) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("nav", 0, 0, "", {"flex", "h-16", "text-sm"}));

  std::string css =
      "@layer utilities {\n"
      "  .flex { display: flex; }\n"
      "  .h-16 { height: 4rem; }\n"
      "  .text-sm { font-size: 0.875rem; }\n"
      "}";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer utilities"), std::string::npos)
      << "the @layer wrapper must be preserved";
  EXPECT_NE(result.critical_css.find(".flex"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".h-16"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".text-sm"), std::string::npos);
  EXPECT_EQ(3, result.critical_rules);
  // The serialized block must be brace-balanced.
  auto opens =
      std::count(result.critical_css.begin(), result.critical_css.end(), '{');
  auto closes =
      std::count(result.critical_css.begin(), result.critical_css.end(), '}');
  EXPECT_EQ(opens, closes) << "serialized critical CSS must be brace-balanced";
}

// An attribute selector cannot be captured from the DOM sample, but when its
// identity is supplied via force_include it is re-emitted inside its @layer.
TEST(CriticalCssExtractorTest, ForceIncludeAugmentsMatcherBlindSpot) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer components {\n"
      "  [data-x] { color: red; }\n"
      "}";

  // Without force_include the attribute rule cannot match the sample DOM.
  CriticalCssResult without = extractor.Extract(elements, css);
  EXPECT_EQ(without.critical_css.find("[data-x]"), std::string::npos)
      << "attribute selector is not DOM-matchable and should be dropped";

  // With its identity forced, it is re-emitted inside its @layer wrapper.
  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"components", "", NormalizeSelector("[data-x]")});
  CriticalCssResult with = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);
  EXPECT_NE(with.critical_css.find("[data-x]"), std::string::npos)
      << "force_include identity must re-emit the attribute rule";
  EXPECT_NE(with.critical_css.find("@layer components"), std::string::npos)
      << "forced rule must be serialized inside its layer wrapper";
}

// The Tailwind v4 layer-order statement `@layer a,b,c,d;` is preserved and
// emitted before the block rules that reference those layers.
TEST(CriticalCssExtractorTest, LayerOrderStatementEmittedFirst) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"flex"}));

  std::string css =
      "@layer theme,base,components,utilities;\n"
      "@layer utilities { .flex { display: flex; } }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  size_t order_pos =
      result.critical_css.find("@layer theme,base,components,utilities;");
  ASSERT_NE(order_pos, std::string::npos)
      << "layer-order statement must be preserved";
  size_t flex_pos = result.critical_css.find(".flex");
  ASSERT_NE(flex_pos, std::string::npos);
  EXPECT_LT(order_pos, flex_pos)
      << "the layer-order statement must precede the block rules";
}

// A custom-property registration is not a style rule: dropping it does not
// merely omit styling, it changes how the RETAINED rules compute. An
// unregistered `var(--x)` is invalid at computed-value time, so a rule that
// survived into the critical block renders wrong until the full sheet lands.
TEST(CriticalCssExtractorTest, PropertyRegistrationsAreAlwaysIncluded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@property --tw-border-style { syntax: \"*\"; inherits: false; "
      "initial-value: solid; }\n"
      ".box { border-style: var(--tw-border-style); }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@property --tw-border-style"),
            std::string::npos)
      << "@property registration must survive into the critical block";
  EXPECT_NE(result.critical_css.find(".box"), std::string::npos)
      << "the rule consuming the registered property must still be retained";
}

TEST(CriticalCssExtractorTest, CounterStyleAndFontPaletteValuesAlwaysIncluded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@counter-style thumbs { system: cyclic; symbols: \"X\"; suffix: \" \"; "
      "}\n"
      "@font-palette-values --Alt { font-family: \"Bixa\"; base-palette: 1; }\n"
      ".box { list-style: thumbs; font-palette: --Alt; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@counter-style thumbs"),
            std::string::npos)
      << "@counter-style must survive into the critical block";
  EXPECT_NE(result.critical_css.find("@font-palette-values --Alt"),
            std::string::npos)
      << "@font-palette-values must survive into the critical block";
}

// Tailwind v4 emits its registrations inside `@layer` (and sometimes behind an
// `@supports` probe), so the always-include rule has to hold through the
// container recursion, not just at top level.
TEST(CriticalCssExtractorTest,
     PropertyRegistrationSurvivesInsideLayerAndSupports) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer base {\n"
      "  @supports (color: red) {\n"
      "    @property --tw-x { syntax: \"*\"; inherits: false; }\n"
      "  }\n"
      "}";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@property --tw-x"), std::string::npos)
      << "@property nested in @layer/@supports must survive";
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "the @layer wrapper must be preserved";
  EXPECT_NE(result.critical_css.find("@supports"), std::string::npos)
      << "the @supports wrapper must be preserved";
  auto opens =
      std::count(result.critical_css.begin(), result.critical_css.end(), '{');
  auto closes =
      std::count(result.critical_css.begin(), result.critical_css.end(), '}');
  EXPECT_EQ(opens, closes) << "serialized critical CSS must be brace-balanced";
}

// Chrome CSS-coverage ranges are byte slices of the stylesheet: the enclosing
// `@layer utilities {` prelude is not part of any used range, so the identity
// derived from coverage arrives context-free. It must still reach the rule the
// producer sees inside the layer.
TEST(CriticalCssExtractorTest,
     ForceIncludeMatchesLayeredRuleWhenCoverageIdentityLacksLayerPath) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css = "@layer utilities { .flex { display: flex; } }";

  CriticalCssResult without = extractor.Extract(elements, css);
  ASSERT_EQ(without.critical_css.find(".flex"), std::string::npos)
      << ".flex does not match the sample DOM, so only force_include can "
         "retain it";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".flex")});
  CriticalCssResult with = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);
  EXPECT_NE(with.critical_css.find(".flex"), std::string::npos)
      << "a context-free coverage identity must reach the layered rule";
  EXPECT_NE(with.critical_css.find("@layer utilities"), std::string::npos)
      << "the forced rule must be serialized inside its layer wrapper";
}

// The responsive-variant case: the coverage slice drops the `@media` prelude
// as well as the `@layer` one, so both context fields must relax together.
// Responsive variants are the bulk of a utility sheet — relaxing only the
// layer path would leave the seam inert for exactly the class that matters.
TEST(
    CriticalCssExtractorTest,
    ForceIncludeMatchesMediaNestedRuleWhenCoverageIdentityLacksMediaCondition) {
  CriticalCssConfig config;
  // Force the per-rule @media recursion rather than wholesale inclusion.
  config.max_wholesale_media_bytes = 8;
  CriticalCssExtractor extractor(config);
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer utilities {\n"
      "  @media (min-width: 768px) { .md\\:flex { display: flex; } }\n"
      "}";

  CriticalCssResult without = extractor.Extract(elements, css);
  ASSERT_EQ(without.critical_css.find("md\\:flex"), std::string::npos)
      << "the responsive variant does not match the sample DOM";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".md\\:flex")});
  CriticalCssResult with = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);
  EXPECT_NE(with.critical_css.find("md\\:flex"), std::string::npos)
      << "a context-free coverage identity must reach the responsive variant";
  EXPECT_NE(with.critical_css.find("@layer utilities"), std::string::npos);
  EXPECT_NE(with.critical_css.find("@media (min-width: 768px)"),
            std::string::npos)
      << "the forced rule must keep its @media wrapper";
}

// The ambiguity control on relaxation: when the sheet places the same
// normalized selector in more than one cascade LAYER, a context-free identity
// matches NEITHER. Those are different rules with different precedence and
// nothing in a context-free identity says which one the browser used, so
// relaxation refuses rather than guessing.
TEST(CriticalCssExtractorTest, AmbiguousRelaxedForceIncludeKeyMatchesNothing) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer base { .btn { color: red; } }\n"
      "@layer utilities { .btn { color: blue; } }";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".btn")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(".btn"), std::string::npos)
      << "an ambiguous relaxed key must match neither context";
}

// Relaxation is a fallback for identities that LOST their context, not a
// wildcard: an identity that carries a layer path keeps exact-match semantics.
TEST(CriticalCssExtractorTest,
     RelaxedFallbackOnlyAppliesToContextFreeIdentities) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css = "@layer utilities { .grid { display: grid; } }";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"components", "", NormalizeSelector(".grid")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(".grid"), std::string::npos)
      << "an identity naming a different layer must not relax into this one";
}

// Several @media conditions within ONE layer are not ambiguity. A context-free
// coverage identity means the browser used the selector with no media
// qualification in play, so the unconditional occurrence is the one it names.
//
// The `.container` shape is why this matters: its breakpoint overrides ride
// along wholesale (they are small @media blocks inside the layer, emitted as
// whole rules), so treating the selector as ambiguous would drop ONLY the base
// rule and leave `.container` with a max-width and no width — a worse result
// than either forcing or dropping the whole family.
TEST(CriticalCssExtractorTest,
     RelaxedForceIncludePrefersUnconditionalOccurrence) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer utilities {\n"
      "  .container { width: 100%; }\n"
      "  @media (min-width: 640px) { .container { max-width: 640px; } }\n"
      "  @media (min-width: 768px) { .container { max-width: 768px; } }\n"
      "}";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".container")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("width: 100%"), std::string::npos)
      << "the unconditional .container rule must be force-included";
  EXPECT_NE(result.critical_css.find("max-width: 640px"), std::string::npos)
      << "the 640px override rides along with its @media block";
  EXPECT_NE(result.critical_css.find("max-width: 768px"), std::string::npos)
      << "the 768px override rides along with its @media block";
  auto opens =
      std::count(result.critical_css.begin(), result.critical_css.end(), '{');
  auto closes =
      std::count(result.critical_css.begin(), result.critical_css.end(), '}');
  EXPECT_EQ(opens, closes) << "serialized critical CSS must be brace-balanced";
}

// Characterization of accepted over-inclusion: @supports is transparent for
// identity everywhere in this file, so a guarded and an unguarded copy of the
// same selector collapse to ONE context and a context-free identity forces
// BOTH. Bounded — the two are cascade alternatives, and only one can ever win.
TEST(CriticalCssExtractorTest, SupportsIsTransparentForRelaxedForceInclude) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer utilities {\n"
      "  .gap-4 { gap: 1rem; }\n"
      "  @supports (display: grid) { .gap-4 { gap: 1.5rem; } }\n"
      "}";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".gap-4")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("gap: 1rem"), std::string::npos);
  EXPECT_NE(result.critical_css.find("gap: 1.5rem"), std::string::npos)
      << "@supports is transparent for identity, so both copies are forced";
}

// Prefix matching would make `@importantly` an `@import` and retain an
// arbitrary rule wholesale. At-rule names are matched with an ident boundary.
TEST(CriticalCssExtractorTest, AtRuleMatchingRespectsIdentBoundaries) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@propertyish --nope { color: red; }\n"
      "@importantly { color: blue; }\n"
      "@property --real { syntax: \"*\"; inherits: false; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("@propertyish"), std::string::npos)
      << "@propertyish is not @property";
  EXPECT_EQ(result.critical_css.find("@importantly"), std::string::npos)
      << "@importantly is not @import";
  EXPECT_NE(result.critical_css.find("@property --real"), std::string::npos)
      << "the real registration is still retained";
}

// ---------------------------------------------------------------------------
// Measured fold: the browser's per-viewport render replaces the
// "first 25 elements" estimate.
// ---------------------------------------------------------------------------

// The live repro, in one test.  modpagespeed.com's <head> spends the whole
// 25-element estimate before the first visible body element, so the utility
// classes that lay out the header (element index ~45) are judged below the fold
// and their rules are dropped from the critical block — the page paints
// unstyled until the deferred sheet lands.  A real browser render says those
// elements ARE above the fold; consulting it must admit them.
TEST(CriticalCssExtractorTest, MeasuredFoldSelectorsAdmitBodyLevelUtilities) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  // A head-heavy document: 40 elements consumed before any body content.
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "", {"flex", "h-16"}));
  elements.push_back(MakeElement("span", 4, 45, "", {"items-center"}));

  const std::string css =
      ".flex { display: flex; }\n"
      ".h-16 { height: 4rem; }\n"
      ".items-center { align-items: center; }\n";

  // Baseline: the index estimate alone drops all three.
  CriticalCssExtractor legacy;
  CriticalCssResult before = legacy.Extract(elements, css);
  ASSERT_TRUE(before.success);
  EXPECT_EQ(before.critical_css.find("display: flex"), std::string::npos)
      << "precondition: the 25-element estimate excludes index 44/45";
  EXPECT_EQ(before.critical_rules, 0);

  CriticalCssConfig config;
  config.measured_above_fold_selectors = {".flex", ".h-16", ".items-center"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult after = extractor.Extract(elements, css);

  ASSERT_TRUE(after.success);
  EXPECT_NE(after.critical_css.find("display: flex"), std::string::npos);
  EXPECT_NE(after.critical_css.find("height: 4rem"), std::string::npos);
  EXPECT_NE(after.critical_css.find("align-items: center"), std::string::npos);
  EXPECT_EQ(after.critical_rules, 3);
}

// The measured set is matched against the ELEMENT, not against the rule text:
// a token nothing in this document carries admits nothing.
TEST(CriticalCssExtractorTest, MeasuredFoldSelectorsMatchElementsNotRuleText) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "", {"mt-96"}));

  const std::string css = ".flex { display: flex; }\n.mt-96 { margin: 24rem; }";

  CriticalCssConfig config;
  // Measured on some other page's fold; this document has no `.flex`.
  config.measured_above_fold_selectors = {".flex"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("display: flex"), std::string::npos)
      << "a measured token no element carries must not force its rule in";
  EXPECT_EQ(result.critical_css.find("margin: 24rem"), std::string::npos);
}

// An id token admits its element too, and matching is against the raw
// attribute value — the tokens carry DOM attribute text, not escaped CSS
// identifiers.
TEST(CriticalCssExtractorTest, MeasuredFoldMatchesIdsAndRawClassValues) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "site-bar"));
  elements.push_back(MakeElement("a", 4, 45, "", {"dark:bg-stone-900"}));

  const std::string css =
      "#site-bar { position: sticky; }\n"
      ".dark\\:bg-stone-900 { background: #1c1917; }";

  CriticalCssConfig config;
  config.measured_above_fold_selectors = {"#site-bar", ".dark:bg-stone-900"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("position: sticky"), std::string::npos);
  EXPECT_NE(result.critical_css.find("#1c1917"), std::string::npos)
      << "the class token is the raw DOM value; the sheet escapes the colon";
}

// Nothing measured (no browser profile, a failed render, a profile written
// before the fold was measured) must behave exactly as before.
TEST(CriticalCssExtractorTest, EmptyMeasuredFoldFallsBackToLegacyHeuristic) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 3, "", {"intro"}));
  elements.push_back(MakeElement("div", 1, 44, "", {"flex"}));

  const std::string css = ".intro { color: red; }\n.flex { display: flex; }";

  CriticalCssConfig config;
  config.measured_above_fold_selectors = {};
  CriticalCssExtractor measured(config);
  CriticalCssResult with_empty = measured.Extract(elements, css);

  CriticalCssExtractor legacy;
  CriticalCssResult baseline = legacy.Extract(elements, css);

  ASSERT_TRUE(with_empty.success);
  ASSERT_TRUE(baseline.success);
  EXPECT_EQ(with_empty.critical_css, baseline.critical_css);
  EXPECT_NE(with_empty.critical_css.find("color: red"), std::string::npos);
  EXPECT_EQ(with_empty.critical_css.find("display: flex"), std::string::npos);
}

// The universal selector is not a fold descriptor.  A garbage or hostile
// profile carrying "*" must not turn every element in the document into
// above-the-fold content.
TEST(CriticalCssExtractorTest, MeasuredFoldIgnoresWildcardAndEmptyTokens) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "", {"mt-96"}));

  const std::string css = ".mt-96 { margin: 24rem; }";

  CriticalCssConfig config;
  config.measured_above_fold_selectors = {"*", "", "div"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("margin: 24rem"), std::string::npos)
      << "wildcard/empty/bare-tag tokens must admit nothing";
}

}  // namespace
}  // namespace pagespeed
