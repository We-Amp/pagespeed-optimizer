// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The HTML cache decision line formatter.
//
// The properties these tests exist to hold:
//
//   1. the line is one JSON object whose keys come out in the canonical order
//      whatever order the caller gave them in;
//   2. the key set is closed, and has no key through which a body, a cookie,
//      a header value, a client address or a user agent could be logged;
//   3. every value is escaped, and a value that could split the line (CR, LF,
//      NUL) is refused rather than written;
//   4. the bounds (path length, host present) hold.

#include "lib/cache/html_cache_decision.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

using Fields = std::vector<HtmlCacheDecisionField>;

std::optional<std::string> Format(const Fields& fields) {
  return FormatHtmlCacheDecisionLine(fields);
}

TEST(HtmlCacheDecisionTest, KeysAreTheClosedCanonicalSet) {
  const std::vector<std::string_view> expected = {
      "ts_ms",  "host",    "path",    "mode",       "action",
      "reason", "status",  "size",    "lifetime_s", "validators",
      "cc",     "vary",    "sidecar", "request",    "personalisation",
      "before", "variant", "binding", "age_s"};
  const auto keys = HtmlCacheDecisionKeys();
  ASSERT_EQ(keys.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(keys[i], expected[i]);
}

TEST(HtmlCacheDecisionTest, NoKeyCanCarryAPrivateValue) {
  for (std::string_view key : HtmlCacheDecisionKeys()) {
    for (std::string_view banned :
         {"body", "cookie", "user_agent", "remote_addr", "value", "header",
          "headers", "ip", "client", "agent", "referer", "authorization",
          "query", "content"}) {
      EXPECT_EQ(key.find(banned), std::string_view::npos)
          << key << " contains " << banned;
    }
  }
  // And the formatter enforces it: those names are refused.
  for (std::string_view banned :
       {"body", "cookie", "user_agent", "remote_addr", "value"}) {
    const Fields f = {{"host", "a.example"}, {banned, "x"}};
    EXPECT_FALSE(Format(f).has_value()) << banned;
  }
}

TEST(HtmlCacheDecisionTest, CanonicalOrderWhateverTheInputOrder) {
  const Fields shuffled = {{"age_s", "7"},        {"reason", "none"},
                           {"host", "a.example"}, {"ts_ms", "1700000000000"},
                           {"path", "/x"},        {"mode", "report-only"},
                           {"action", "store"}};
  EXPECT_EQ(Format(shuffled).value(),
            "{\"ts_ms\":1700000000000,\"host\":\"a.example\",\"path\":\"/x\","
            "\"mode\":\"report-only\",\"action\":\"store\",\"reason\":\"none\","
            "\"age_s\":7}");
}

TEST(HtmlCacheDecisionTest, OnlyTheGivenKeysAreWritten) {
  const Fields one = {{"host", "a.example"}};
  EXPECT_EQ(Format(one).value(), "{\"host\":\"a.example\"}");
  const Fields none;
  EXPECT_EQ(Format(none).value(), "{}");
}

TEST(HtmlCacheDecisionTest, EveryKeyRoundTrips) {
  Fields all;
  for (std::string_view key : HtmlCacheDecisionKeys())
    all.push_back({key, "1"});
  all[1].value = "h.example";
  const std::string line = Format(all).value();
  EXPECT_EQ(line.front(), '{');
  EXPECT_EQ(line.back(), '}');
  EXPECT_EQ(line.find('\n'), std::string::npos);
  for (std::string_view key : HtmlCacheDecisionKeys()) {
    EXPECT_NE(line.find("\"" + std::string(key) + "\":"), std::string::npos)
        << key;
  }
}

TEST(HtmlCacheDecisionTest, NumbersAreNumbersOnlyWhenCanonicalDigits) {
  const Fields digits = {{"host", "h"},         {"ts_ms", "1"},
                         {"status", "200"},     {"size", "0"},
                         {"lifetime_s", "300"}, {"age_s", "12"}};
  EXPECT_EQ(Format(digits).value(),
            "{\"ts_ms\":1,\"host\":\"h\",\"status\":200,\"size\":0,"
            "\"lifetime_s\":300,\"age_s\":12}");

  const Fields strings = {{"host", "h"},         {"ts_ms", "12ab"},
                          {"status", ""},        {"size", "-5"},
                          {"lifetime_s", "007"}, {"age_s", "1.5"}};
  EXPECT_EQ(
      Format(strings).value(),
      "{\"ts_ms\":\"12ab\",\"host\":\"h\",\"status\":\"\",\"size\":\"-5\","
      "\"lifetime_s\":\"007\",\"age_s\":\"1.5\"}");

  // Too many digits to be a safe number.
  const Fields wide = {{"host", "h"}, {"size", "123456789012345678901"}};
  EXPECT_EQ(Format(wide).value(),
            "{\"host\":\"h\",\"size\":\"123456789012345678901\"}");

  // A digits-only value for a non-numeric key stays a string.
  const Fields text = {{"host", "h"}, {"before", "1234"}, {"cc", "42"}};
  EXPECT_EQ(Format(text).value(),
            "{\"host\":\"h\",\"cc\":\"42\",\"before\":\"1234\"}");
}

TEST(HtmlCacheDecisionTest, ValuesAreEscaped) {
  const Fields f = {{"host", "a\"b\\c"},
                    {"path", "/\x01\t\x1f"},
                    {"reason", "headers:x\"y"},
                    {"vary", "a</script>b"}};
  EXPECT_EQ(Format(f).value(),
            "{\"host\":\"a\\\"b\\\\c\",\"path\":\"/\\u0001\\t\\u001f\","
            "\"reason\":\"headers:x\\\"y\",\"vary\":\"a</script>b\"}");
}

TEST(HtmlCacheDecisionTest, UnknownAndDuplicateKeysAreRefused) {
  EXPECT_FALSE(Format({{"host", "h"}, {"bogus", "x"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"HOST", "x"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"", "x"}}).has_value());
  EXPECT_FALSE(
      Format({{"host", "h"}, {"path", "/a"}, {"path", "/b"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"host", "h"}}).has_value());
}

TEST(HtmlCacheDecisionTest, LineBreakingBytesAreRefused) {
  EXPECT_FALSE(Format({{"host", "h"}, {"reason", "a\nb"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"reason", "a\rb"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"reason", std::string_view("a\0b", 3)}})
                   .has_value());
  EXPECT_FALSE(Format({{"host", "a\nb"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"path", "/a\r\n"}}).has_value());
  // Other control characters are escaped, not refused.
  EXPECT_TRUE(Format({{"host", "h"}, {"reason", "a\tb"}}).has_value());
}

TEST(HtmlCacheDecisionTest, HostMustNotBeEmpty) {
  EXPECT_FALSE(Format({{"host", ""}}).has_value());
  EXPECT_FALSE(Format({{"host", ""}, {"path", "/"}}).has_value());
  EXPECT_TRUE(Format({{"host", "x"}}).has_value());
}

TEST(HtmlCacheDecisionTest, PathIsBoundedAt300Bytes) {
  const std::string ok(300, 'a');
  const std::string too_long(301, 'a');
  EXPECT_TRUE(Format({{"host", "h"}, {"path", ok}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"path", too_long}}).has_value());
  // The bound is on the raw value, not on the escaped one.
  const std::string control(300, '\x01');
  EXPECT_TRUE(Format({{"host", "h"}, {"path", control}}).has_value());
  // Other keys are not bounded by it.
  EXPECT_TRUE(Format({{"host", "h"}, {"reason", too_long}}).has_value());
}

TEST(HtmlCacheDecisionTest, Fnv1a64Vectors) {
  EXPECT_EQ(HtmlCacheFnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(HtmlCacheFnv1a64("a"), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(HtmlCacheFnv1a64("foobar"), 0x85944171f73967e8ULL);
  EXPECT_NE(HtmlCacheFnv1a64("a"), HtmlCacheFnv1a64("b"));
  // Bytes at and above 0x80 hash as unsigned bytes.
  EXPECT_EQ(HtmlCacheFnv1a64(std::string_view("\xff", 1)),
            0xaf64724c8602eb6eULL);
  // NUL bytes are hashed, not terminators.
  EXPECT_NE(HtmlCacheFnv1a64(std::string_view("a\0", 2)),
            HtmlCacheFnv1a64("a"));
}

TEST(HtmlCacheDecisionTest, DedupKey) {
  const Fields f = {{"ts_ms", "5"},
                    {"age_s", "9"},
                    {"host", "h.example"},
                    {"path", "/p"},
                    {"mode", "off"},
                    {"action", "refuse"},
                    {"reason", "vary:accept"},
                    {"sidecar", "swap"},
                    {"before", "abcd"},
                    {"status", "200"}};
  EXPECT_EQ(HtmlCacheDecisionDedupKey(f),
            std::string("h.example\x1f/p\x1foff\x1frefuse\x1fvary:accept\x1f"
                        "swap\x1f"
                        "abcd"));
  // Order of the input does not matter; the ignored fields do not either.
  const Fields g = {{"before", "abcd"},        {"sidecar", "swap"},
                    {"reason", "vary:accept"}, {"action", "refuse"},
                    {"mode", "off"},           {"path", "/p"},
                    {"host", "h.example"}};
  EXPECT_EQ(HtmlCacheDecisionDedupKey(f), HtmlCacheDecisionDedupKey(g));
  // A missing component is empty, so the separators still count.
  const Fields sparse = {{"host", "h"}};
  EXPECT_EQ(HtmlCacheDecisionDedupKey(sparse),
            std::string("h\x1f\x1f\x1f\x1f\x1f\x1f"));
  const Fields changed = {{"host", "h.example"},     {"path", "/p"},
                          {"mode", "off"},           {"action", "refuse"},
                          {"reason", "vary:accept"}, {"sidecar", "swap"},
                          {"before", "ffff"}};
  EXPECT_NE(HtmlCacheDecisionDedupKey(f), HtmlCacheDecisionDedupKey(changed));
}

}  // namespace
}  // namespace pagespeed
