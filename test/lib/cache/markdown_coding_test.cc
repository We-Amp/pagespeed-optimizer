// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/markdown_coding.h"

#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

std::string Order(std::vector<std::string_view> lines) {
  const MarkdownCodingOrder o = SelectMarkdownCoding(lines);
  if (o.count == 0) return "none";
  std::string out;
  for (size_t i = 0; i < o.count; ++i) {
    if (i > 0) out += ",";
    out += MarkdownCodingName(o.order[i]);
  }
  return out;
}

std::string First(std::vector<std::string_view> lines) {
  const MarkdownCodingOrder o = SelectMarkdownCoding(lines);
  return o.count == 0 ? "none" : std::string(MarkdownCodingName(o.order[0]));
}

TEST(MarkdownCodingTest, ChoosesByWeightThenBrGzipIdentity) {
  struct Row {
    std::vector<std::string_view> lines;
    const char* first;
  };
  const Row rows[] = {
      {{"br, gzip"}, "br"},
      {{"gzip"}, "gzip"},
      {{"gzip;q=1, br;q=0.5"}, "gzip"},
      {{"br;q=0, gzip"}, "gzip"},
      {{"br;q=0"}, "identity"},
      {{"br;q=0.0"}, "identity"},
      {{"br;q=0.000"}, "identity"},
      {{"br;q=0.001, gzip;q=0.001"}, "br"},
      {{"gzip;q=1.000, br;q=0.999"}, "gzip"},
      {{"*"}, "br"},
      {{"*;q=0, identity"}, "identity"},
      {{"identity;q=0"}, "none"},
      {{"br, identity;q=0"}, "br"},
      {{"X-GZIP"}, "gzip"},
      {{}, "identity"},
      {{""}, "identity"},
      {{"gzip;q=abc"}, "identity"},
  };
  for (const Row& row : rows) {
    std::string joined;
    for (auto l : row.lines) joined += "[" + std::string(l) + "]";
    SCOPED_TRACE(joined);
    EXPECT_EQ(First(row.lines), row.first);
  }
}

TEST(MarkdownCodingTest, ReturnsTheWholePreferenceOrder) {
  EXPECT_EQ(Order({"br, gzip"}), "br,gzip,identity");
  EXPECT_EQ(Order({"gzip;q=1, br;q=0.5"}), "gzip,br,identity");
  EXPECT_EQ(Order({"*"}), "br,gzip,identity");
  EXPECT_EQ(Order({"*;q=0.5, gzip"}), "gzip,br,identity");
  EXPECT_EQ(Order({"identity, br;q=0.5"}), "identity,br");
  EXPECT_EQ(Order({"*;q=0"}), "none");
  EXPECT_EQ(Order({"*;q=0, identity;q=0.1"}), "identity");
  EXPECT_EQ(Order({"BR", "Gzip;Q=0.5"}), "br,gzip,identity")
      << "field lines combine, names and the weight name ignore case";
}

TEST(MarkdownCodingTest, OtherCodingsAndMalformedElementsAreIgnored) {
  EXPECT_EQ(Order({"deflate, zstd"}), "identity");
  EXPECT_EQ(Order({"gzip;q=1.5"}), "identity") << "q above 1";
  EXPECT_EQ(Order({"gzip;q=1.001"}), "identity");
  EXPECT_EQ(Order({"gzip;q=0.0001"}), "identity") << "four decimals";
  EXPECT_EQ(Order({"gzip;q="}), "identity");
  EXPECT_EQ(Order({"gzip;q=.5"}), "identity");
  EXPECT_EQ(Order({"gzip;"}), "identity");
  EXPECT_EQ(Order({"gz ip"}), "identity") << "not a token";
  EXPECT_EQ(Order({",, ,gzip,"}), "gzip,identity") << "empty members";
  EXPECT_EQ(Order({"gzip ; q=0.5"}), "gzip,identity") << "OWS around ;";
  EXPECT_EQ(Order({"gzip;level=1"}), "gzip,identity")
      << "an unknown parameter is ignored";
}

TEST(MarkdownCodingTest, ACodingNamedTwiceTakesItsLowestWeight) {
  EXPECT_EQ(Order({"br, br;q=0"}), "identity");
  EXPECT_EQ(Order({"gzip;q=0.2", "gzip;q=0.9, br;q=0.5"}), "br,gzip,identity");
}

TEST(MarkdownCodingTest, CopyIdsAndNames) {
  EXPECT_EQ(MarkdownCopyId(MarkdownCoding::kIdentity), 0x7C);
  EXPECT_EQ(MarkdownCopyId(MarkdownCoding::kBrotli), 0xDC);
  EXPECT_EQ(MarkdownCopyId(MarkdownCoding::kGzip), 0xEC);
  EXPECT_EQ(MarkdownCodingOfCodedId(0xDC), MarkdownCoding::kBrotli);
  EXPECT_EQ(MarkdownCodingOfCodedId(0xEC), MarkdownCoding::kGzip);
  EXPECT_FALSE(MarkdownCodingOfCodedId(0x7C).has_value());
  EXPECT_FALSE(MarkdownCodingOfCodedId(0xFC).has_value());
  EXPECT_EQ(MarkdownCodingName(MarkdownCoding::kBrotli), "br");
  EXPECT_EQ(MarkdownCodingName(MarkdownCoding::kGzip), "gzip");
  EXPECT_EQ(MarkdownCodingName(MarkdownCoding::kIdentity), "identity");
}

}  // namespace
}  // namespace pagespeed
