// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the shared UTF-8-safe entity decoder. Pins the
// contract that the agent_optimize body-text, inline-code, and <meta
// description> paths all rely on: decode HTML entities to human glyphs while
// NEVER erasing a non-ASCII byte and NEVER shipping invalid UTF-8.

#include "lib/html/safe_entity_decode.h"

#include <string>

#include "gtest/gtest.h"
#include "lib/html/html_keywords.h"

namespace net_instaweb {
namespace {

class SafeEntityDecodeTest : public testing::Test {
 protected:
  void SetUp() override { HtmlKeywords::Init(); }
};

TEST_F(SafeEntityDecodeTest, DecodesAsciiEntities) {
  EXPECT_EQ(SafeDecodeHtmlEntities("Tom &amp; Jerry &lt;1ms&gt;"),
            "Tom & Jerry <1ms>");
}

TEST_F(SafeEntityDecodeTest, NoAmpersandFastPathIsVerbatim) {
  EXPECT_EQ(SafeDecodeHtmlEntities("plain ascii text"), "plain ascii text");
  // Literal multibyte UTF-8 (em-dash) with no entity passes through verbatim.
  EXPECT_EQ(SafeDecodeHtmlEntities("Fast \xE2\x80\x94 really"),
            "Fast \xE2\x80\x94 really");
}

// The contract: the no-'&' fast path must never ship invalid UTF-8 even when an
// upstream byte-cap split a trailing multibyte sequence. The incomplete tail is
// dropped (the rest of the field is preserved).
TEST_F(SafeEntityDecodeTest, FastPathTrimsTruncatedTrailingMultibyte) {
  // "caf" + the first 2 bytes of a 3-byte em-dash (E2 80) with the 3rd byte
  // (94) cut off by a cap. No '&' -> fast path. The dangling E2 80 is dropped.
  EXPECT_EQ(SafeDecodeHtmlEntities("caf\xE2\x80"), "caf");
  // A lone lead byte at the end is also dropped.
  EXPECT_EQ(SafeDecodeHtmlEntities("ok\xE2"), "ok");
  // A complete sequence is preserved.
  EXPECT_EQ(SafeDecodeHtmlEntities("ok\xE2\x80\x94"), "ok\xE2\x80\x94");
}

// A named entity that decodes to a single Latin-1 byte > 127 (e.g. &middot; ->
// 0xB7) is a lone invalid byte; the decoder must fall back to the original
// entity text rather than ship the invalid byte.
TEST_F(SafeEntityDecodeTest, NonAsciiEntityFallsBackToOriginal) {
  const std::string out = SafeDecodeHtmlEntities("a &middot; b");
  // Never an invalid byte; the literal entity text survives.
  EXPECT_TRUE(IsWellFormedUtf8(out));
  EXPECT_NE(out.find("&middot;"), std::string::npos);
}

// &nbsp; decodes to Latin-1 0xA0 (a lone invalid UTF-8 byte) — it must be folded
// to an ASCII space, never shipped raw.
TEST_F(SafeEntityDecodeTest, FoldsNbspToSpace) {
  const std::string out = SafeDecodeHtmlEntities("a&nbsp;b");
  EXPECT_EQ(out, "a b");
  EXPECT_TRUE(IsWellFormedUtf8(out));
}

TEST_F(SafeEntityDecodeTest, IsWellFormedUtf8Basics) {
  EXPECT_TRUE(IsWellFormedUtf8("ascii"));
  EXPECT_TRUE(IsWellFormedUtf8("caf\xC3\xA9"));    // é
  EXPECT_TRUE(IsWellFormedUtf8("\xE2\x80\x94"));   // em-dash
  EXPECT_FALSE(IsWellFormedUtf8("\xE2\x80"));      // truncated
  EXPECT_FALSE(IsWellFormedUtf8("\xB7"));          // lone Latin-1 byte
  EXPECT_FALSE(IsWellFormedUtf8("\xED\xA0\x80"));  // surrogate
}

TEST_F(SafeEntityDecodeTest, FoldLatin1NbspInPlace) {
  std::string s = "a\xA0\x62";  // a, NBSP, b
  FoldLatin1Nbsp(&s);
  EXPECT_EQ(s, "a b");
}

// The UTF-8 no-break space is two bytes; both fold together, and a letter whose
// second byte is 0xA0 (U+00E0) is left alone.
TEST_F(SafeEntityDecodeTest, FoldNbspKeepsSequencesWhole) {
  std::string s = "16\xC2\xA0KB \xC3\xA0";
  FoldNbsp(&s);
  EXPECT_EQ(s, "16 KB \xC3\xA0");
  EXPECT_TRUE(IsWellFormedUtf8(s));
}

TEST_F(SafeEntityDecodeTest, LiteralNbspFoldedOnEveryPath) {
  EXPECT_EQ(SafeDecodeHtmlEntities("16\xC2\xA0KB"), "16 KB");
  EXPECT_EQ(SafeDecodeHtmlEntities("a\xC2\xA0\xC2\xA0\x62"), "a  b");
}

TEST_F(SafeEntityDecodeTest, ToWellFormedUtf8ReplacesInvalidBytes) {
  EXPECT_EQ(ToWellFormedUtf8("ok \xE2\x80\x94"), "ok \xE2\x80\x94");
  EXPECT_EQ(ToWellFormedUtf8("caf\xE9!"), "caf\xEF\xBF\xBD!");
  EXPECT_EQ(ToWellFormedUtf8("a\x80\x62"), "a\xEF\xBF\xBD\x62");
  EXPECT_EQ(ToWellFormedUtf8("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD");
  EXPECT_EQ(ToWellFormedUtf8("\xED\xA0\x80"),
            "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  // An incomplete sequence at the very end is dropped (a byte-cap cut).
  EXPECT_EQ(ToWellFormedUtf8("caf\xE2\x80"), "caf");
  // The same bytes in the middle are invalid, so they are replaced.
  EXPECT_EQ(ToWellFormedUtf8("\xE2\x80x"), "\xEF\xBF\xBD\xEF\xBF\xBDx");
  for (const char* in : {"\xFF", "a\xC2", "\xF4\x90\x80\x80", "\xA0\xA0"}) {
    EXPECT_TRUE(IsWellFormedUtf8(ToWellFormedUtf8(in))) << in;
  }
}

TEST_F(SafeEntityDecodeTest, DecodeNeverReturnsInvalidUtf8) {
  EXPECT_EQ(SafeDecodeHtmlEntities("caf\xE9 au lait"),
            "caf\xEF\xBF\xBD au lait");
  EXPECT_TRUE(IsWellFormedUtf8(SafeDecodeHtmlEntities("caf\xE9 &lt;")));
}

}  // namespace
}  // namespace net_instaweb
