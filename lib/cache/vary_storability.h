// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_VARY_STORABILITY_H_
#define PAGESPEED_LIB_CACHE_VARY_STORABILITY_H_

#include <string_view>

namespace pagespeed {

// Store-side `Vary` refusal predicate: may a response carrying this `Vary`
// header be stored in the shared cache at all?
//
// A response is refused when its `Vary` names `*` (RFC 9110 §12.5.5 — varies
// on everything) or ANY field-name outside the set the 32-bit capability mask
// actually keys on.  Storing such a response would put a single representation
// behind a key that does not distinguish the axis it genuinely varies on, and
// the next client on a different axis value gets the wrong bytes.
//
// The allowed set is four request header fields, each of which the capability
// mask genuinely keys on:
//
//   Accept-Encoding  — transfer-encoding bits
//   User-Agent       — viewport class + pixel-density bits
//   Accept           — image-format bits
//   Save-Data        — save-data bit
//
// It is a SUBSET of what the mask consumes, not a mirror of it.
// `CapabilityMask::FromHeaders` also takes `Sec-CH-DPR` (it feeds the same
// density bit as `User-Agent`), and a response declaring `Vary: Sec-CH-DPR` is
// refused here. That is deliberate and is the conservative direction: a refusal
// costs one cache entry, whereas admitting a field this gate has never admitted
// is a behaviour change with its own blast radius. Widening the list is a
// decision, not a mechanical consequence of the mask gaining an axis.
//
// The half that IS mechanical is the dangerous half, and it is tested: every
// field allowed here must be one the mask actually distinguishes on, or this
// predicate admits responses the cache key cannot tell apart. See
// `VaryStorabilityAxes` in the unit test, which drives
// `CapabilityMask::FromHeaders` rather than comparing against a second copy of
// the list.
//
// `combined_vary` is the response's `Vary` value.  HTTP permits a response to
// carry several `Vary` header lines, which combine as one comma-separated list
// (RFC 9110 §5.3) — a caller holding multiple lines MUST join them with `,`
// before calling, because a per-line call would accept a list this predicate
// as a whole would refuse.  An empty value means the response declares no
// variance and is storable.
//
// Field-name matching is ASCII case-insensitive and OWS (SP / HTAB) around each
// list member is trimmed per RFC 9110 §5.6.1.  Empty list members are skipped,
// which is what makes `Vary: Accept,,Accept-Encoding` storable rather than a
// refusal on an empty field-name.
//
// This is the one implementation of the predicate: it is shared by the front
// stages that decide what to store and exported on the C ABI as
// `ps_vary_uncacheable`, so an embedder applies the same gate rather than
// re-deriving it.
bool VaryUncacheable(std::string_view combined_vary);

// Everything a response's `Vary` decides on the store side, answered in ONE
// call so the answers cannot disagree.
//
// The refusal above is not the whole store-side question.  A response can be
// perfectly storable and still be one the optimizer must keep its hands off:
// an origin that declares `Vary: Accept` has said that IT picks the
// representation from the request's `Accept`.  We store what it sent — but we
// must not then derive a second representation family from the one copy we
// happened to capture, because the copy is whichever representation the FIRST
// requester's `Accept` elicited.  Every later client would be served variants
// transcoded from one arbitrary client's negotiation result.
//
// Splitting that across two independently-derived predicates is what made the
// two halves drift apart: the store side admitted the response and the notify
// side had no `Vary` term at all, so `Vary: Accept` responses were stored AND
// optimized. One parse, one verdict, both consumers reading the same fields.
struct VaryStoreVerdict {
  // May the response enter the shared cache at all?  Exactly
  // `!VaryUncacheable(combined_vary)`.
  bool storable = true;

  // Does the origin negotiate on `Accept` itself?  When true the stored
  // original must be stamped with `AlternateMetadata::kFlagOriginVariesAccept`
  // and must NEVER be handed to the optimizer.  The persisted flag is what
  // lets a later serve tell "deliberately left alone" from "not optimized
  // yet", and it is what puts `Accept` back into the `Vary` of a response
  // built from the entry (on a HIT the origin's own headers are gone).
  //
  // False whenever `storable` is false: the class is "store it, but never
  // optimize it", and there is nothing to store or to mark when the response
  // is refused outright.  So `varies_accept` implies `storable`.
  bool varies_accept = false;
};

// Classify one response's combined `Vary` for the store path.
//
// `combined_vary` has exactly the shape `VaryUncacheable` documents: the
// response's several `Vary` lines joined with `,`, empty when it carries none.
VaryStoreVerdict ClassifyVaryForStore(std::string_view combined_vary);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_VARY_STORABILITY_H_
