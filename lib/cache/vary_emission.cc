// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/vary_emission.h"

#include <string_view>

#include "lib/classify/content_type.h"

namespace pagespeed {

std::string_view VaryForServedResponse(ContentType ct,
                                       bool entry_varies_accept) {
  switch (ct) {
    case ContentType::kImage:
      // Sec-CH-DPR drives the density bit (image resize width) independently
      // of User-Agent, so downstream caches must key on it too.  Already
      // names Accept: the origin's own Accept negotiation adds no axis this
      // row was missing, and appending a second `Accept` would be a
      // duplicate list member.
      return "Accept, Save-Data, User-Agent, Sec-CH-DPR";
    case ContentType::kHtml:
      return entry_varies_accept ? "Accept-Encoding, User-Agent, Accept"
                                 : "Accept-Encoding, User-Agent";
    case ContentType::kCss:
    case ContentType::kJs:
      return entry_varies_accept ? "Accept-Encoding, Accept"
                                 : "Accept-Encoding";
    case ContentType::kOther:
      break;
  }
  // kOther: we negotiate nothing for it, so the table had no row.  The
  // origin's own Accept negotiation still has to be declared — it is the one
  // axis this response genuinely varies on.
  return entry_varies_accept ? "Accept" : std::string_view();
}

}  // namespace pagespeed
