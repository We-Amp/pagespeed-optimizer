// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The request side of the HTML cache policy. The shared policy cases
// (daemon_html_policy_cases.json) state the verdicts through the C API; these
// cover what that file does not reach: spans, ordering and combinations.

#include "lib/cache/html_request_policy.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {
namespace {

std::string Text(std::span<const std::string_view> lines,
                 const HtmlCookieSpan& s) {
  return std::string(lines[s.line].substr(s.offset, s.length));
}

TEST(HtmlRequestPolicyTest, GuardChecksThePathBeforeAnyHeader) {
  const HeaderField h[] = {{"X-Forwarded-Host", "a.example"}};
  const HtmlGuardVerdict v = HtmlRequestGuard("/a;b", h, "https");
  EXPECT_EQ(v.reason, HtmlGuardReason::kPath);
  EXPECT_EQ(v.header_index, -1);
  EXPECT_FALSE(v.xfp);
}

TEST(HtmlRequestPolicyTest, GuardReportsTheFirstRefusedLine) {
  const HeaderField h[] = {{"Accept", "text/html"},
                           {"X-Forwarded-Proto", " http "},
                           {"X-Host", "a.example"}};
  const HtmlGuardVerdict v = HtmlRequestGuard("/page", h, "https");
  EXPECT_EQ(v.reason, HtmlGuardReason::kRequestHeader);
  EXPECT_EQ(v.header_index, 1);
  EXPECT_TRUE(v.xfp);
  EXPECT_EQ(HtmlRequestGuard("/page", h, "http").header_index, 2);
}

TEST(HtmlRequestPolicyTest, GuardDecodesTheLastSegmentForItsExtension) {
  EXPECT_EQ(HtmlRequestGuard("/a/style%2Ecss", {}, "https").reason,
            HtmlGuardReason::kPath);
  EXPECT_EQ(HtmlRequestGuard("/a/style%2", {}, "https").reason,
            HtmlGuardReason::kNone);
  EXPECT_EQ(HtmlRequestGuard("/", {}, "https").reason, HtmlGuardReason::kNone);
  EXPECT_EQ(HtmlRequestGuard("/a%2", {}, "https").reason,
            HtmlGuardReason::kNone);
}

TEST(HtmlRequestPolicyTest, CookieSpansPointIntoTheCallersLines) {
  const std::string_view lines[] = {" _ga = \"GA1.2\" ; _gid=x",
                                    "_fbp=fb.1; sid=abc"};
  const HtmlCookieVerdict v = ClassifyHtmlCookies(lines, true, {});
  EXPECT_EQ(v.cookie_class, HtmlCookieClass::kBlocking);
  EXPECT_FALSE(v.malformed);
  EXPECT_EQ(Text(lines, v.first_blocking), "sid");
  EXPECT_EQ(v.first_blocking.line, 1u);
  EXPECT_TRUE(v.ignorable_values.empty());

  const std::string_view ok[] = {" _ga = \"GA1.2\" ; _gid=x"};
  const HtmlCookieVerdict w = ClassifyHtmlCookies(ok, true, {});
  ASSERT_EQ(w.cookie_class, HtmlCookieClass::kIgnorableOnly);
  ASSERT_EQ(w.ignorable_values.size(), 2u);
  EXPECT_EQ(Text(ok, w.ignorable_values[0]), "GA1.2");
  EXPECT_EQ(Text(ok, w.ignorable_values[1]), "x");
}

TEST(HtmlRequestPolicyTest, MalformedPairsBlockWithoutAName) {
  for (std::string_view line :
       {"a", "=v", "a=\x01", "a=\"unclosed", "a=b\"c", "a b=1", "", ";"}) {
    const std::string_view lines[] = {line};
    const HtmlCookieVerdict v = ClassifyHtmlCookies(lines, true, {});
    EXPECT_EQ(v.cookie_class, HtmlCookieClass::kBlocking) << line;
    EXPECT_TRUE(v.malformed) << line;
    EXPECT_EQ(v.first_blocking.length, 0u) << line;
  }
}

TEST(HtmlRequestPolicyTest, ARefusedAdditionIsIgnored) {
  const std::string_view lines[] = {"sess_x=1"};
  const std::string_view additions[] = {"sess*", "*"};
  EXPECT_EQ(ClassifyHtmlCookies(lines, true, additions).cookie_class,
            HtmlCookieClass::kBlocking);
  // Session names block in any case, whatever an accepted prefix covers.
  const std::string_view upper[] = {"abtest_SESSION=1"};
  const std::string_view prefix[] = {"abtest_*"};
  EXPECT_EQ(ClassifyHtmlCookies(upper, true, prefix).cookie_class,
            HtmlCookieClass::kBlocking);
}

TEST(HtmlRequestPolicyTest, StampIsStableAndNeverZero) {
  const std::string_view a[] = {"utm_src", "abtest_*"};
  const std::string_view b[] = {"abtest_*", "utm_src", "utm_src"};
  const uint64_t x = HtmlPolicyStamp(HtmlCredentialedPolicy::kAnonymousOnly,
                                     true, kHtmlCookieListVersion, a);
  EXPECT_EQ(x, HtmlPolicyStamp(HtmlCredentialedPolicy::kAnonymousOnly, true,
                               kHtmlCookieListVersion, b));
  EXPECT_NE(x, 0u);
  // An addition cannot be confused with two.
  const std::string_view joined[] = {"utm_src\n7:abtest_*"};
  EXPECT_NE(x, HtmlPolicyStamp(HtmlCredentialedPolicy::kAnonymousOnly, true,
                               kHtmlCookieListVersion, joined));
}

TEST(HtmlRequestPolicyTest, UnsafeMethods) {
  EXPECT_TRUE(HtmlUnsafeMethodInvalidates("POST", 204));
  EXPECT_TRUE(HtmlUnsafeMethodInvalidates("", 301));
  EXPECT_TRUE(HtmlUnsafeMethodInvalidates("get", 200));
  EXPECT_FALSE(HtmlUnsafeMethodInvalidates("POST", 400));
  EXPECT_FALSE(HtmlUnsafeMethodInvalidates("HEAD", 200));
}

}  // namespace
}  // namespace pagespeed
