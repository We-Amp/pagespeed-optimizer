// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/cookie_cache_gate.h"

#include "lib/cache/html_cache_policy.h"

namespace pagespeed {

bool CookieCacheGateAllows(bool request_has_cookie, bool html_class,
                           uint16_t origin_cc_flags) {
  if (!html_class) {
    return true;
  }
  return HtmlCookieRequestMayShare(
      request_has_cookie, HtmlExplicitlySharedFromFlags(origin_cc_flags));
}

}  // namespace pagespeed
