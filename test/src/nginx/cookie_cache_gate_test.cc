// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the shared-cache gate on cookie-bearing HTML requests
// (src/nginx/cookie_cache_gate.h).  The module wiring (record path, HIT path,
// the Markdown copy's lookup of the recorded page) is covered end to end by
// tools/http-compliance/test_cookie_request.py.

#include "src/nginx/cookie_cache_gate.h"

#include "gtest/gtest.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using Meta = AlternateMetadata;

constexpr uint16_t kPlain = Meta::kCCOriginHeaderPresent;

TEST(CookieCacheGateTest, RequestWithoutCookieIsUnaffected) {
  EXPECT_TRUE(CookieCacheGateAllows(false, true, 0));
  EXPECT_TRUE(CookieCacheGateAllows(false, true, kPlain));
}

TEST(CookieCacheGateTest, CookieRequestWithoutPublicIsNotShared) {
  EXPECT_FALSE(CookieCacheGateAllows(true, true, 0));
  EXPECT_FALSE(CookieCacheGateAllows(true, true, kPlain));
  EXPECT_FALSE(
      CookieCacheGateAllows(true, true, kPlain | Meta::kCCOriginNoCache));
}

TEST(CookieCacheGateTest, CookieRequestWithPublicOrSMaxageIsShared) {
  EXPECT_TRUE(
      CookieCacheGateAllows(true, true, kPlain | Meta::kCCOriginPublic));
  EXPECT_TRUE(CookieCacheGateAllows(true, true,
                                    kPlain | Meta::kCCOriginSMaxagePresent));
}

TEST(CookieCacheGateTest, MustRevalidateDoesNotMarkAPageShareable) {
  // must-revalidate is one of the Authorization permits, not a sharing mark.
  EXPECT_FALSE(CookieCacheGateAllows(true, true,
                                     kPlain | Meta::kCCOriginMustRevalidate));
  EXPECT_FALSE(CookieCacheGateAllows(true, true,
                                     kPlain | Meta::kCCOriginProxyRevalidate));
}

TEST(CookieCacheGateTest, NonHtmlIsOutsideTheRule) {
  EXPECT_TRUE(CookieCacheGateAllows(true, false, 0));
  EXPECT_TRUE(CookieCacheGateAllows(true, false, kPlain));
}

}  // namespace
}  // namespace pagespeed
