// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_VARY_EMISSION_H_
#define PAGESPEED_LIB_CACHE_VARY_EMISSION_H_

#include <string_view>

#include "lib/classify/content_type.h"

namespace pagespeed {

// The `Vary` field value a response we build OURSELVES must carry, so that a
// shared cache downstream of us keys it on every axis it actually varies on.
//
// TWO INPUTS, and they answer two different questions.
//
//   `ct` — what WE negotiate on.  A per-content-type table: images are
//   selected by format (`Accept`), save-data and density (`Save-Data`,
//   `User-Agent`, `Sec-CH-DPR`); everything else is at most re-encoded
//   (`Accept-Encoding`), with HTML additionally UA-dependent.  This is a
//   property of the optimizer, identical for every entry of that type.
//
//   `entry_varies_accept` — what the ORIGIN negotiates on, read from the
//   entry's persisted `AlternateMetadata::kFlagOriginVariesAccept`.  It is
//   per-ENTRY, and it cannot be derived from the content type: an
//   Accept-varying CSS response and an ordinary one are both `kCss`.
//
// The second input is the whole reason this is a function and not a table
// lookup.  A response built from a cache entry no longer has the origin's
// headers, so an origin `Vary: Accept` on (say) a stylesheet was simply lost:
// the table's CSS row is `Accept-Encoding`, and a downstream shared cache then
// keyed that response WITHOUT `Accept` and could hand one client's
// representation to a differently-negotiating one.
//
// Adding `Accept` to the CSS/JS row unconditionally would fix that case by
// over-keying every stylesheet and script at every downstream cache — paying a
// hit-rate regression on the whole population to cover the rare member of it.
// Keying on the stored per-entry marker pays it only where the origin actually
// asked for it.
//
// The unflagged answers are byte-identical to the table this replaced; the
// flagged answers are that same value with `Accept` appended (and bare
// `Accept` where the table had nothing to say). `kImage` already names
// `Accept`, so its two answers are the same string — a flagged image response
// must not end up declaring `Accept` twice.
//
// Returns an EMPTY view when there is nothing to emit; the caller must then
// add no header at all rather than an empty one.
//
// CALL SITES.  Only where the module composes the response headers itself:
// the cache HIT, the 304 write-back that is rewritten as a 200, and the
// stale-if-error serve.  NOT the MISS path — there the origin's own response
// (and its own `Vary`) passes through, so re-adding `Accept` from the marker
// would emit a duplicate list member.
std::string_view VaryForServedResponse(ContentType ct,
                                       bool entry_varies_accept);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_VARY_EMISSION_H_
