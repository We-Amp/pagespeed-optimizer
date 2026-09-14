// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/authz_cache_gate.h"

#include "lib/classify/alternate_metadata.h"

namespace pagespeed {

bool AuthzCacheGateAllows(bool request_has_authorization,
                          uint16_t origin_cc_flags) {
  if (!request_has_authorization) {
    return true;
  }
  // RFC 9111 §3.5: only these response directives explicitly permit a
  // shared cache to store/serve despite the request's Authorization.  The
  // s-maxage permit is its Present flag (which also covers s-maxage=0); a
  // stored s-maxage value without the flag cannot occur (see header), so
  // origin_cc_flags == 0 — no Cache-Control at store time, or corrupt
  // metadata — carries no permission signal and fails closed.
  constexpr uint16_t kPermits = AlternateMetadata::kCCOriginPublic |
                                AlternateMetadata::kCCOriginMustRevalidate |
                                AlternateMetadata::kCCOriginSMaxagePresent;
  return (origin_cc_flags & kPermits) != 0;
}

bool AuthzCacheGateAllowsStale(bool request_has_authorization,
                               uint16_t origin_cc_flags) {
  if (!request_has_authorization) {
    return true;
  }
  // Once stale, must-revalidate/s-maxage demand revalidation before reuse
  // (RFC 9111 §4.2.4, §5.2.2.10), so of §3.5's permits only `public`
  // still allows a stale serve (coalesced-SWR and stale-if-error alike).
  return (origin_cc_flags & AlternateMetadata::kCCOriginPublic) != 0;
}

}  // namespace pagespeed
