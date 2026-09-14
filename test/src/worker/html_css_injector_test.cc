// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the HTML Critical CSS Injector.

#include "src/worker/html_css_injector.h"

#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Test: Basic injection before </head>.
TEST(HtmlCssInjectorTest, InjectBeforeHead) {
  std::string html =
      "<html><head><title>Test</title></head><body>Hello</body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.injected);
  // Style tag should appear before </head>.
  EXPECT_NE(result.html.find(
                "<style data-pagespeed-critical>body { margin: 0; }</style>"
                "</head>"),
            std::string::npos);
  // Original content should still be present.
  EXPECT_NE(result.html.find("<title>Test</title>"), std::string::npos);
  EXPECT_NE(result.html.find("Hello"), std::string::npos);
}

// Test: Uppercase </HEAD>.
TEST(HtmlCssInjectorTest, InjectBeforeUppercaseHead) {
  std::string html =
      "<HTML><HEAD><TITLE>Test</TITLE></HEAD><BODY>Hello</BODY></HTML>";
  std::string css = "h1 { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find(
                "<style data-pagespeed-critical>h1 { color: red; }</style>"
                "</HEAD>"),
            std::string::npos);
}

// Test: Mixed case </hEaD>.
TEST(HtmlCssInjectorTest, InjectBeforeMixedCaseHead) {
  std::string html = "<html><head></hEaD><body></body></html>";
  std::string css = "p { font-size: 14px; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find("</style></hEaD>"), std::string::npos);
}

// Test: Missing </head> falls back to </body>.
TEST(HtmlCssInjectorTest, FallbackToBody) {
  std::string html = "<html><body><p>Hello</p></body></html>";
  std::string css = "p { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find("</style></body>"), std::string::npos);
}

// Test: Missing both </head> and </body> falls back to after <head>.
TEST(HtmlCssInjectorTest, FallbackToAfterHead) {
  std::string html = "<html><head><title>T</title><p>Hello</p>";
  std::string css = "p { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Style tag should be injected after <head>.
  size_t head_end = result.html.find("<head>") + 6;
  EXPECT_NE(result.html.find("<style data-pagespeed-critical>", head_end),
            std::string::npos);
}

// Test: No head or body at all — insert at document start.
TEST(HtmlCssInjectorTest, FallbackToDocumentStart) {
  std::string html = "<div>Hello World</div>";
  std::string css = "div { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u);
}

// Test: </head> inside HTML comment is skipped.
TEST(HtmlCssInjectorTest, HeadInsideCommentSkipped) {
  std::string html =
      "<html><head>"
      "<!-- </head> this is a comment -->"
      "</head><body>Hello</body></html>";
  std::string css = "body { background: white; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The injected style tag should be before the real </head>,
  // not the one inside the comment.
  size_t comment_end = result.html.find("-->");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, comment_end);
}

// Test: Empty CSS returns original HTML unchanged.
TEST(HtmlCssInjectorTest, EmptyCssNoInjection) {
  std::string html = "<html><head></head><body>Hello</body></html>";

  auto result = InjectCriticalCss(html, "");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
  EXPECT_EQ(result.html, html);
}

// Test: Empty HTML returns failure.
TEST(HtmlCssInjectorTest, EmptyHtmlFailure) {
  auto result = InjectCriticalCss("", "body { margin: 0; }");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test: Multiple </head> inside comments, real one at the end.
TEST(HtmlCssInjectorTest, MultipleCommentsBeforeRealHead) {
  std::string html =
      "<html><head>"
      "<!-- </head> -->"
      "<!-- another </head> -->"
      "</head><body></body></html>";
  std::string css = "a { color: green; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Style should be before the real </head>, after all comments.
  size_t second_comment_end = result.html.rfind("-->");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, second_comment_end);
}

// Test: Preserves existing style tags.
TEST(HtmlCssInjectorTest, PreservesExistingStyles) {
  std::string html =
      "<html><head>"
      "<style>.existing { color: red; }</style>"
      "</head><body></body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find(".existing { color: red; }"), std::string::npos);
  EXPECT_NE(result.html.find("data-pagespeed-critical"), std::string::npos);
}

// Test 0a: CSS containing </style> XSS payload aborts injection.
TEST(HtmlCssInjectorTest, StyleCloseXssAborted) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style><script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
  // The HTML should be empty (not returned unchanged) since injection
  // was aborted due to dangerous CSS, not because CSS was empty.
  EXPECT_TRUE(result.html.empty());
}

// Test 0a: Case-insensitive </STYLE> detection.
TEST(HtmlCssInjectorTest, StyleCloseXssCaseInsensitive) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</STYLE><script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: </style followed by space (no closing ">") is still detected.
TEST(HtmlCssInjectorTest, StyleCloseXssWithSpace) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style <script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: </style followed by tab is detected.
TEST(HtmlCssInjectorTest, StyleCloseXssWithTab) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style\t<script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: </style followed by newline is detected.
TEST(HtmlCssInjectorTest, StyleCloseXssWithNewline) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style\n<script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: Mixed case </sTyLe with tab is detected.
TEST(HtmlCssInjectorTest, StyleCloseXssMixedCaseWithTab) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</sTyLe\t<script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: Null bytes in CSS are stripped.
TEST(HtmlCssInjectorTest, NullBytesStripped) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body { margin: 0; }";
  css.push_back('\0');  // Append a null byte.
  css.append("extra");

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Null byte should have been stripped, leaving "body { margin: 0; }extra".
  EXPECT_NE(result.html.find("body { margin: 0; }extra"), std::string::npos);
}

// Test 0b: </head> inside <script> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideScriptSkipped) {
  std::string html =
      "<html><head>"
      "<script>var s = \"</head>\";</script>"
      "</head><body></body></html>";
  std::string css = "body { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Should inject before the real </head>, not the one inside <script>.
  size_t script_end = result.html.find("</script>");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, script_end);
}

// Test 0b: </head> inside <textarea> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideTextareaSkipped) {
  std::string html =
      "<html><head>"
      "<textarea></head></textarea>"
      "</head><body></body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t textarea_end = result.html.find("</textarea>");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, textarea_end);
}

// Test 0b: </head> inside <style> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideStyleSkipped) {
  std::string html =
      "<html><head>"
      "<style>/* </head> */</style>"
      "</head><body></body></html>";
  std::string css = "body { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t style_tag_end = result.html.find("</style>");
  size_t injected_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(injected_pos, style_tag_end);
}

// Test 0b: </head> inside <title> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideTitleSkipped) {
  std::string html =
      "<html><head>"
      "<title></head></title>"
      "</head><body></body></html>";
  std::string css = "body { color: green; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t title_end = result.html.find("</title>");
  size_t injected_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(injected_pos, title_end);
}

// Test 0b: </head> inside <xmp> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideXmpSkipped) {
  std::string html =
      "<html><head>"
      "<xmp></head></xmp>"
      "</head><body></body></html>";
  std::string css = "body { color: purple; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t xmp_end = result.html.find("</xmp>");
  size_t injected_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(injected_pos, xmp_end);
}

// Test 0c: <header> is not matched when looking for <head>.
TEST(HtmlCssInjectorTest, HeaderNotMatchedAsHead) {
  // No </head>, no </body>, only <header> — should NOT match as <head>.
  // Falls through to strategy 4 (document start).
  std::string html = "<html><header>Hello</header></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Injection should be at document start (strategy 4), not after <header>.
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u);
}

// Test 0c: <head class="foo"> still matches (word boundary after name).
TEST(HtmlCssInjectorTest, HeadWithAttributesMatches) {
  std::string html = "<html><head class=\"foo\"><title>T</title><p>Hello</p>";
  std::string css = "p { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Should inject after <head class="foo">, not at document start.
  size_t head_end = result.html.find("<head class=\"foo\">") + 18;
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_EQ(style_pos, head_end);
}

// ========== Coverage: Empty HTML and missing tag ==========

TEST(HtmlCssInjectorTest, EmptyHtmlReturnsFailure) {
  auto result = InjectCriticalCss("", "body { color: red; }");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.injected);
  EXPECT_TRUE(result.html.empty());
}

TEST(HtmlCssInjectorTest, TagNotFoundFallsToDocStart) {
  // HTML without <head>, </head>, or </body> — falls through to
  // strategy 4: insert at document start.
  std::string html = "<p>Just a paragraph</p>";
  std::string css = "p { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The style tag should be at position 0 (document start).
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u)
      << "Without head/body tags, injection should be at document start";
  // Original content should still be present after the injected style.
  EXPECT_NE(result.html.find("<p>Just a paragraph</p>"), std::string::npos);
}

// ========== Coverage: TrySkipRawTextElement edge cases ==========

// Raw text tag followed by a non-boundary character (e.g., <scripting>)
// should NOT be treated as a raw text element.  This exercises the
// word-boundary check in TrySkipRawTextElement (lines 47-50).
TEST(HtmlCssInjectorTest, ScriptingTagNotTreatedAsScript) {
  // <scripting> should not be treated as <script>.
  // The </head> at the end should be found normally.
  std::string html =
      "<html><head>"
      "<scripting></head></scripting>"
      "</head><body></body></html>";
  std::string css = "body { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The first </head> (inside <scripting>) is NOT inside a raw text element,
  // so it should be matched as the injection point.
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  size_t first_head_close = result.html.find("</head>");
  EXPECT_LT(style_pos, first_head_close);
}

// Unclosed raw text element: <script> without </script>.
// TrySkipRawTextElement returns npos for the unclosed element (line 66),
// but the caller treats this the same as "not a raw text tag" and
// continues scanning.  The </head> is still found because the scanner
// advances past the unrecognized <script> character by character.
TEST(HtmlCssInjectorTest, UnclosedScriptTagStillFindsHead) {
  std::string html =
      "<html><head>"
      "<script>var x = 1; /* no closing script tag */"
      "</head><body></body></html>";
  std::string css = "body { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The unclosed <script> causes TrySkipRawTextElement to return npos,
  // but </head> is still found by the character-by-character scan.
  EXPECT_NE(result.html.find("</style></head>"), std::string::npos);
}

// Unclosed HTML comment means no </head> or </body> can be found.
TEST(HtmlCssInjectorTest, UnclosedCommentBlocksAllMatches) {
  std::string html =
      "<html><head>"
      "<!-- this comment is never closed"
      "</head><body></body></html>";
  std::string css = "body { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Unclosed comment swallows everything, so injection falls through to
  // strategy 3 (after <head>) or strategy 4 (document start).
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  // <head> tag does exist, so strategy 3 should work.
  size_t head_open_end = result.html.find("<head>") + 6;
  EXPECT_EQ(style_pos, head_open_end);
}

// Raw text element with closing tag missing '>' — the search for '>' after
// </script fails, causing TrySkipRawTextElement to return npos.
TEST(HtmlCssInjectorTest, ScriptClosingTagMissingAngleBracket) {
  std::string html =
      "<html><head>"
      "<script>code</script"
      "</head><body></body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The </script without '>' means the closing tag is malformed.
  // TrySkipRawTextElement returns npos, so </head> cannot be found
  // through that path.  Injection still works via strategy 3 or 4.
}

// <head without closing '>' — FindOpenTagEnd returns npos for that tag.
// Falls through to strategy 4.
TEST(HtmlCssInjectorTest, HeadTagWithoutClosingAngleBracket) {
  std::string html = "<html><head no-close";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // No valid <head>, </head>, or </body> → document start.
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u);
}

}  // namespace
}  // namespace pagespeed
