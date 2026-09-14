// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// from the template hash, using SentinelId::kBrowserProfile (0x5C).

#ifndef PAGESPEED_SRC_BROWSER_OPTIMIZATION_PROFILE_H_
#define PAGESPEED_SRC_BROWSER_OPTIMIZATION_PROFILE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace pagespeed {

// Rendered image dimensions detected by browser analysis.
struct ImageDimension {
  std::string selector;  // CSS selector for the image element
  uint32_t rendered_width = 0;
  uint32_t rendered_height = 0;
  uint32_t natural_width = 0;
  uint32_t natural_height = 0;
};

// Resource preload hint from browser analysis.
struct PreloadHint {
  std::string url;
  std::string as;    // "image", "style", "script", "font"
  std::string type;  // MIME type (optional)
};

// Per-viewport optimization data from browser analysis.
struct ViewportProfile {
  std::string critical_css;
  std::string lcp_selector;
  std::string lcp_url;
  // Selector tokens for the elements a real browser measured above the fold in
  // THIS viewport, deduped, in document order. Two token shapes are meaningful
  // to the critical-CSS extractor: "#id" and ".class", carrying the RAW DOM
  // attribute value (not an escaped CSS identifier — `.dark:bg-stone-900`, not
  // `.dark\:bg-stone-900`). Anything else here — notably the tag-prefixed image
  // selectors this list has always carried, e.g. "img#hero" — is inert to the
  // extractor by construction.
  std::vector<std::string> above_fold_selectors;
  std::vector<std::string> below_fold_selectors;
  std::vector<ImageDimension> image_dimensions;
  float css_coverage_ratio = 0.0f;  // fraction of CSS bytes used
  size_t total_css_bytes = 0;
  size_t unused_css_bytes = 0;

  // Empirical sufficiency record for stylesheet deferral (async-CSS).
  //
  // The byte-ratio floor is a proxy: it can pass while the inlined block does
  // not in fact cover the fold, which is a flash of unstyled content. Deferral
  // therefore requires a POSITIVE record here, and the absence of one means
  // "keep the stylesheet render-blocking" — never "assume it is fine".
  bool critical_css_validated = false;
  // Differing-pixel ratio the validation measured; <0 means "never measured".
  float validation_diff_ratio = -1.0f;
  // Hex SHA-256 of the critical block the validation rendered. FORENSIC ONLY —
  // written here, never read by any gate. It answers "which bytes was this
  // record actually about?" when a page misbehaves; the block is re-derived per
  // page, so comparing against it at serve time would invalidate nearly
  // everything. The gate reads validated_combined_css_hash below.
  std::string validated_critical_css_hash;
  // Hex SHA-256 of the COMBINED stylesheet the validation was performed
  // against. The validated bit may only be honoured while the page still
  // serves that exact stylesheet: the template hash covers the tag tree and is
  // blind to CSS, so a CSS-only redeploy (new utility-framework build,
  // unchanged markup) would otherwise keep a stale validation alive for the
  // rest of the profile's TTL. Hashing the combined sheet rather than the
  // critical block is deliberate — per-page class variation changes the
  // critical block on nearly every page and would suppress everything.
  std::string validated_combined_css_hash;
};

// Content hash used to bind a validation record to the stylesheet it was made
// against. Hex-encoded SHA-256, the primitive the cache already
// content-addresses with (SentinelId::kContentHash).
//
// Empty input hashes to the empty string, which no stored hash can equal:
// "validated against nothing" must never satisfy the gate.
//
// ---------------------------------------------------------------------------
// INPUT CONTRACT — read before hashing anything
// ---------------------------------------------------------------------------
// The argument MUST be the bytes produced by `Worker::BuildCombinedCss`
// (src/worker/worker.h), and a producer of a validation record MUST CALL that
// function rather than reconstruct the bytes. A record is only meaningful
// against the exact sequence the serve path will hash, and the assembly has
// enough steps that an independent reimplementation will drift:
//
//   1. Seed: `scan_result.inline_css` verbatim — the concatenated <style>
//      bodies, exactly as HtmlScanner produced them.
//   2. @import flattening of that seed is ALL-OR-NOTHING: the flattened result
//      replaces the seed only when `imports_resolved > 0`; any unresolved
//      import leaves the ORIGINAL bytes. Disabled entirely under
//      `disable_css_import_flattening`.
//   3. Then each declared external <link rel=stylesheet>, in the order
//      HtmlScanner recorded them (document order), resolved from cache at the
//      normalized URL. Hrefless sheets are skipped; a sheet that resolves with
//      an EMPTY body contributes nothing and does not set the missing flag.
//   4. Join: a single "\n" before each appended sheet, and only when the
//      accumulator is already non-empty. No trailing newline, no separator
//      before the first contribution.
//   5. Per-sheet @import flattening is all-or-nothing on the same rule.
//   6. Cap: 10 MiB. Exceeding it BREAKS out of the loop — later sheets are
//      silently absent from the bytes and `external_css_missing` is set.
//   7. No normalization of any kind: no minification, no whitespace collapsing,
//      no ordering fix-up. Bytes in, bytes hashed.
//
// KNOWN INSTABILITIES — accepted, and design inputs for the validator that
// will produce these records:
//   * A sheet whose cache entry flips between original and minified variants
//     changes these bytes without the stylesheet having been republished. The
//     record invalidates and the page reverts to render-blocking until it is
//     re-validated. Correct direction (fail toward render-blocking), but it
//     means "hash changed" does NOT imply "the site published new CSS".
//   * A transient cache miss on one sheet is indistinguishable here from a
//     redeploy: both simply produce different bytes. The validator must not
//     treat a single mismatch as evidence the site changed.
std::string CombinedCssValidationHash(std::string_view css);

// The serve path's accept test for a validation record (issue #1056).
//
// The byte/coverage floor is a proxy for "the inlined block covers the fold",
// and a proxy can pass while the fold is left unstyled. Deferral therefore
// additionally requires a POSITIVE record on the viewport profile this page's
// critical block was derived from, still bound to the exact stylesheet being
// served:
//
//   - No profile (the heuristic path) -> no record and none obtainable, so
//     never defer. On a small sheet the byte gate's escape hatch would
//     otherwise say "sufficient" without measuring anything.
//   - Stylesheet-only redeploy -> the template hash covers the tag tree and is
//     blind to CSS, so the stale record survives the change that invalidates
//     it. The hash comparison is what notices.
//   - Nothing gathered (empty combined sheet) -> hashes to nothing matchable,
//     so "validated against no stylesheet" cannot satisfy the gate either.
//
// It lives here, beside the hash it consults, so the code that PRODUCES a
// record can be tested against the exact predicate that CONSUMES it rather
// than against a second copy that can drift into agreeing with itself.
bool AsyncCssValidatedForServedSheet(const ViewportProfile* vp,
                                     std::string_view combined_css);

// Which record, if any, the accept test above may be applied to (issue #1216).
//
// A record is only ever about ONE block: the output of
// `DeriveDomMatchedCriticalCss` for this profile, which is what the validation
// rendered and stamped. When that derivation comes back EMPTY for the page
// being served, whatever is inlined in its place — the heuristic extractor's
// substitute, or no block at all — is not what the record describes, while the
// record itself still reads as perfectly healthy: the validated bit is set and
// the stylesheet hash still matches, because neither of them says anything
// about WHICH block is being served. Handing it to the accept test then
// authorizes deferral on a confirmation made about different bytes, and the
// byte floor cannot catch that either — below `async_css_min_deferred_bytes`
// the small-sheet escape hatch clears whatever the coverage is.
//
// So: `vp` when the derivation produced the block, nullptr otherwise. Feeding
// the result straight into `AsyncCssValidatedForServedSheet` keeps the
// stylesheet render-blocking; the substitute block is still inlined, which is a
// win on its own and needs no confirmation.
const ViewportProfile* AsyncCssRecordForDerivedBlock(
    const ViewportProfile* vp, std::string_view derived_critical_css);

// Complete optimization profile for a template.
// Stored in cache at synthetic key:
//   URL:      __pagespeed_profile__/{hex(template_hash)}
//   Hostname: __internal__
//   Sentinel: kBrowserProfile (0x5C)
struct OptimizationProfile {
  std::string template_hash_hex;
  std::string analyzed_url;

  // Per-viewport profiles (standard breakpoints).
  ViewportProfile mobile;   // 375x667
  ViewportProfile tablet;   // 768x1024
  ViewportProfile desktop;  // 1440x900

  // Cross-viewport optimization data.
  std::vector<PreloadHint> preload_hints;
  std::vector<std::string> defer_safe_scripts;

  // Metadata.
  int64_t created_at = 0;  // Unix timestamp
  int64_t expires_at = 0;  // TTL for re-analysis

  // Serialize to JSON string.
  std::string ToJson() const;

  // Deserialize from JSON string.
  static absl::StatusOr<OptimizationProfile> FromJson(std::string_view json);

  // Build the synthetic cache URL for a template hash.
  static std::string CacheUrl(uint64_t template_hash);

  // Fixed hostname used for profile cache entries.
  static constexpr std::string_view kProfileHostname = "__internal__";
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_OPTIMIZATION_PROFILE_H_
