// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Critical CSS Extractor
//
// Extracts critical CSS rules that apply to above-the-fold content
// based on collected HTML elements and heuristics.

#ifndef PAGESPEED_SRC_WORKER_CRITICAL_CSS_EXTRACTOR_H_
#define PAGESPEED_SRC_WORKER_CRITICAL_CSS_EXTRACTOR_H_

#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "lib/classify/capability_mask.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

// Stable identity of a single style rule, independent of surrounding byte
// layout. Two rules share an identity iff they carry the same normalized
// selector under the same cascade-layer path AND @media condition. This is
// what lets the DOM-matched producer (which knows the enclosing @layer/@media
// during recursion) agree with a set of "must-retain" identities derived
// elsewhere (browser coverage, force-include augmentation) — a raw-selector
// match would collapse `.flex` in `@layer utilities` with `.flex` in a
// `@media` block and mis-key state-conditional Tailwind v4 variants.
struct RuleIdentity {
  // ">"-joined enclosing @layer names, outermost first (empty at top level;
  // anonymous @layer contributes an empty segment).
  std::string layer_path;
  // Normalized enclosing @media condition text (empty when unconditional;
  // nested @media conditions are joined with " and ").
  std::string media_condition;
  // Selector text after NormalizeSelector (trim + whitespace collapse).
  std::string normalized_selector;

  bool operator==(const RuleIdentity& other) const {
    return layer_path == other.layer_path &&
           media_condition == other.media_condition &&
           normalized_selector == other.normalized_selector;
  }

  template <typename H>
  friend H AbslHashValue(H h, const RuleIdentity& id) {
    return H::combine(std::move(h), id.layer_path, id.media_condition,
                      id.normalized_selector);
  }
};

// Where a stylesheet places one normalized selector: the distinct cascade-layer
// paths, and the distinct @media conditions, it occurs under. Internal to the
// relaxed force-include fallback (see CriticalCssExtractor::ForceIncludeIndex);
// the two axes are kept apart because they are adjudicated differently — a
// selector spanning two LAYERS is ambiguous, whereas a selector spanning two
// MEDIA conditions within one layer has a well-defined base occurrence.
struct SelectorContexts {
  absl::flat_hash_set<std::string> layer_paths;
  absl::flat_hash_set<std::string> media_conditions;
};

// Canonicalize a selector for identity comparison: trim leading/trailing
// whitespace and collapse every internal run of ASCII whitespace to a single
// space. Escapes are left intact (they are load-bearing in selector text, e.g.
// `dark\:bg-stone-900`), so this is deliberately NOT a wholesale
// UnescapeCssIdent — only the parts that would otherwise be unescaped per
// compound (id/class/tag) are unescaped by the matcher, and both the producer
// and any coverage parser run selectors through THIS function so their
// identities collapse identically.
std::string NormalizeSelector(std::string_view selector);

// Configuration for Critical CSS extraction heuristics
struct CriticalCssConfig {
  // Maximum number of elements to consider "above the fold"
  int max_elements = 25;

  // Maximum depth to consider critical (elements deeper are below fold)
  int max_depth = 10;

  // Always include these selectors regardless of element matching
  // (universal selectors that affect page layout)
  std::vector<std::string> always_include_selectors = {"*", "html", "body",
                                                       ":root"};

  // Tag name patterns to always include (header/nav elements)
  std::vector<std::string> include_tag_patterns = {"header", "nav", "main"};

  // Class patterns to always include (hero sections, etc.)
  std::vector<std::string> include_class_patterns = {"hero", "banner",
                                                     "masthead", "above-fold"};

  // ID patterns to always include
  std::vector<std::string> include_id_patterns = {"hero", "header", "nav",
                                                  "masthead"};

  // Class patterns to always exclude (below-fold content)
  std::vector<std::string> exclude_class_patterns = {"footer", "lazy", "defer",
                                                     "below-fold", "lazyload"};

  // ID patterns to always exclude
  std::vector<std::string> exclude_id_patterns = {"footer", "below-fold"};

  // Tag names to always exclude
  std::vector<std::string> exclude_tag_patterns = {"footer"};

  // ---------------------------------------------------------------------
  // Measured fold — a real browser render replaces the estimate above.
  // ---------------------------------------------------------------------
  //
  // `max_elements` is an ESTIMATE of where the fold is, and on a page with a
  // substantial <head> it is exhausted before the first visible body element:
  // the utility classes that lay out the header sit at element index ~45 and
  // are judged below the fold, so their rules never enter the critical block.
  //
  // Selector tokens for elements a browser measured above the fold. Two shapes
  // are honoured — "#id" and ".class" — each carrying the RAW DOM attribute
  // value, compared against CollectedElement::id / ::classes, which are likewise
  // raw HTML attribute values. (Raw, deliberately: a DOM class of
  // `dark:bg-stone-900` is written `.dark\:bg-stone-900` in the stylesheet, and
  // routing the token through a selector parser would only re-introduce that
  // escaping asymmetry.)
  //
  // Every other token shape is INERT, and that is load-bearing rather than
  // laziness: a bare tag token such as `div` would match essentially every
  // element in the document and promote the whole page to above-the-fold,
  // re-inlining most of a utility stylesheet. `*` and the empty string are
  // inert for the same reason.
  //
  // This is the ONLY measured channel, by design. An earlier revision also
  // carried the browser's COUNT of above-the-fold elements and used it to raise
  // `max_elements`. That is unsound and was removed: the count is consumed as an
  // index bound, but it counts elements with no layout box — the whole <head>,
  // and every `display:none` subtree — because getBoundingClientRect() returns
  // all-zeros for them and `top < viewportHeight` is therefore true. The
  // analyzed document is also not the served one (the analysis render has the
  // stylesheet inlined and a <base> injected, and scripts can add nodes), so the
  // two element orderings do not line up. Between them a count could promote an
  // entire document to critical. An element that only a tag rule describes
  // therefore still falls back to the `max_elements` estimate — i.e. exactly
  // today's behaviour, which is no regression.
  //
  // Empty = nothing measured: the heuristics above are then the only thing in
  // play, exactly as before.
  std::vector<std::string> measured_above_fold_selectors;

  // Maximum size (bytes) for wholesale inclusion of @media blocks inside
  // @layer. Blocks larger than this are recursed into and filtered per-rule.
  // Default 4096 bytes (covers typical Tailwind responsive utility blocks).
  int max_wholesale_media_bytes = 4096;
};

// Result of CSS extraction
struct CriticalCssResult {
  bool success = false;
  std::string error_message;
  std::string critical_css;

  // Statistics
  int total_rules = 0;
  int critical_rules = 0;
};

// Thresholds for the async-CSS FOUC sufficiency gate (see WorkerConfig).
struct AsyncCssSufficiencyConfig {
  float min_coverage_ratio = 0.10f;
  size_t min_deferred_css_bytes = 15000;
};

// Returns true when the inlined critical CSS is sufficient to bridge first
// paint, so the full stylesheet may safely be DEFERRED (switched to
// rel="preload" as="style" by async-CSS). Returns false when the critical CSS
// is too thin relative to the sheet it would defer — deferring then produces a
// flash of unstyled content, so the caller must keep the stylesheet
// render-blocking.
//
// `coverage_ratio` is the browser profile's rule-level css_coverage_ratio when
// known; pass a negative value when unknown (the heuristic path). It is only
// ever trusted DOWNWARD: the gate always computes the extracted-bytes ratio
// (critical_css_bytes / deferred_css_bytes) and gates on the pessimistic of
// the two. A profile's coverage is measured pre-extraction and can wildly
// contradict the bytes actually inlined (a claimed 0.39 coverage next to
// 830 B critical / 115 KB deferred caused a live FOUC); an optimistic
// coverage number must never authorize deferral on its own. Semantics:
//   - external_css_unresolved (a declared external <link> stylesheet was NOT
//     resolved from cache) -> ALWAYS insufficient. This is the dominant
//     fail-safe: when an external sheet is missing, `deferred_css_bytes` does
//     not include it (the cold sheet was never gathered) AND the critical CSS
//     was derived WITHOUT it, so both the byte ratio and the small-sheet escape
//     hatch below are measuring the wrong, shrunken denominator. Never defer a
//     sheet we could not measure — keep it render-blocking (no FOUC). The caller
//     marks the variant for revalidation, so this self-heals once the sheet
//     caches and a real sufficiency decision can be made.
//   - min_coverage_ratio <= 0  -> always sufficient (gate disabled / legacy).
//   - deferred_css_bytes < min_deferred_css_bytes -> always sufficient (a small
//     sheet has a trivial FOUC window; avoids false-positives on light pages).
//   - otherwise sufficient iff min(byte ratio, known coverage_ratio) >=
//     min_coverage_ratio (the byte ratio alone when coverage is unknown/NaN).
bool CriticalCssIsSufficient(float coverage_ratio, size_t critical_css_bytes,
                             size_t deferred_css_bytes,
                             bool external_css_unresolved,
                             const AsyncCssSufficiencyConfig& cfg);

// Extracts critical CSS rules based on collected HTML elements.
//
// This extractor uses heuristics to determine which CSS rules are needed
// for above-the-fold content rendering:
//
// Always included:
// - Universal selectors (*, html, body, :root)
// - First N elements (configurable, default 25)
// - Elements matching header/nav/hero patterns in tag/class/id
// - Non-selector at-rules that the retained rules DEPEND on, wherever they
//   sit in the layer/supports nesting: @charset, @import, @namespace,
//   @font-face, @keyframes, and the registrations (@property,
//   @counter-style, @font-palette-values, @font-feature-values,
//   @position-try). These are not styling and are not optional — a retained
//   rule referencing a registration that was dropped is invalid at
//   computed-value time, so omitting one changes what the SURVIVING rules
//   compute rather than merely removing a rule.
//
// Always excluded:
// - Elements with depth > max_depth (default 10)
// - Elements matching footer/lazy/defer patterns
// - @media print rules
//
// Usage:
//   CriticalCssExtractor extractor;
//   auto result = extractor.Extract(scan_result.elements, css);
//   if (result.success) {
//     // Use result.critical_css
//   }
class CriticalCssExtractor {
 public:
  CriticalCssExtractor();
  explicit CriticalCssExtractor(CriticalCssConfig config);
  ~CriticalCssExtractor();

  // Extract critical CSS rules from the given CSS that apply to the
  // collected elements.
  //
  // elements: Elements collected from HTML by HtmlScanner
  // css: Full CSS content to filter
  // viewport: Target viewport class for @media query filtering
  //           (default: kDesktop for backward compatibility)
  //
  // force_include: optional set of rule identities to emit even when they do
  //   NOT match any collected element. This augments the DOM matcher's blind
  //   spots (attribute selectors, state-conditional variants that cannot be
  //   captured from a static, JS-off, light-mode sample) without falling back
  //   to raw text: a forced rule is still emitted THROUGH the layered
  //   serializer, inside its own @layer/@media wrapper. nullptr disables it.
  //
  // Returns the result with critical CSS rules
  CriticalCssResult Extract(
      const std::vector<CollectedElement>& elements, std::string_view css,
      CapabilityMask::Viewport viewport = CapabilityMask::Viewport::kDesktop,
      const absl::flat_hash_set<RuleIdentity>* force_include = nullptr);

  // Get the current configuration
  const CriticalCssConfig& config() const { return config_; }

 private:
  // Force-include lookup state, built once per Extract().
  //
  // Chrome reports CSS coverage as byte ranges over the stylesheet, and the
  // critical blob is the plain concatenation of those ranges: a used range
  // covers the rule, never the enclosing `@layer ... {` / `@media ... {`
  // prelude. So an identity derived from coverage arrives as {"", "", selector}
  // and can never key-match a rule the producer sees inside a wrapper. On a
  // primary-key miss the lookup therefore retries with both context fields
  // relaxed — but only for force-include entries that carry no context of
  // their own, and only against ONE resolved occurrence, chosen as follows:
  //
  //   - Two distinct cascade LAYERS is genuine ambiguity: the same selector in
  //     `@layer base` and `@layer utilities` are different rules with different
  //     precedence and nothing in a context-free identity says which the
  //     browser used. Matches NOTHING.
  //   - Within a single layer, several @media conditions are NOT ambiguity.
  //     A context-free coverage identity means the browser used the selector
  //     with no media qualification in play, which is the UNCONDITIONAL
  //     occurrence — so that one is chosen and the responsive overrides are
  //     left to whatever the normal @media handling does with them. Choosing
  //     "ambiguous" here would drop the base rule while its own breakpoint
  //     overrides ride along, leaving e.g. `.container` with a `max-width` and
  //     no `width` — worse than either alternative.
  //   - A single layer with exactly one occurrence resolves to that occurrence,
  //     media-qualified or not; that is the responsive-variant case where the
  //     browser used a rule that only exists inside a breakpoint.
  //   - A single layer, several media conditions, none of them unconditional:
  //     no base occurrence exists to prefer, so this matches NOTHING.
  //
  // Relaxation therefore never crosses a layer boundary, and within a layer it
  // resolves to at most one occurrence. Over-inclusion stays bounded and the
  // choice is deterministic.
  struct ForceIncludeIndex {
    explicit ForceIncludeIndex(
        const absl::flat_hash_set<RuleIdentity>& exact_ids)
        : exact(exact_ids) {}

    // Full identities, matched exactly.
    const absl::flat_hash_set<RuleIdentity>& exact;
    // Normalized selectors of the context-free force-include entries.
    absl::flat_hash_set<std::string> relaxable;
    // Normalized selector -> where the sheet places it.
    absl::flat_hash_map<std::string, SelectorContexts> sheet_contexts;

    // True when the rule currently being processed — identified by its own
    // enclosing layer path and media condition — is the occurrence the relaxed
    // key resolves to.
    bool MatchesRelaxed(const std::string& normalized_selector,
                        const std::string& layer_path,
                        const std::string& media_condition) const;
  };

  // Builds the index for one Extract(). `relaxable` and `sheet_contexts` stay
  // empty (and the census walk is skipped) when no force-include entry is
  // context-free, so the exact-match path costs nothing extra.
  static ForceIncludeIndex BuildForceIncludeIndex(
      const std::vector<struct CssRule>& rules,
      CapabilityMask::Viewport viewport,
      const absl::flat_hash_set<RuleIdentity>& force_include);

  // Recursively process CSS rules, handling @layer/@supports container
  // blocks by recursing into their inner rules. Returns the critical CSS
  // for the given rules. max_depth bounds recursion (default 5).
  // inside_layer: when true, large @media blocks are recursed into and
  // filtered per-rule rather than included wholesale.
  // layer_path / media_condition: the enclosing cascade-layer path and @media
  //   condition accumulated during recursion; used to build each rule's
  //   RuleIdentity for the force_include lookup.
  // force_index: identities to emit even without a DOM match (nullptr = off).
  std::string ProcessRules(const std::vector<struct CssRule>& rules,
                           const std::vector<CollectedElement>& elements,
                           CapabilityMask::Viewport viewport,
                           int& critical_count, int max_depth,
                           bool inside_layer, const std::string& layer_path,
                           const std::string& media_condition,
                           const ForceIncludeIndex* force_index);

  // Check if a selector should be included based on heuristics
  bool ShouldIncludeSelector(std::string_view selector,
                             const std::vector<CollectedElement>& elements,
                             CapabilityMask::Viewport viewport);

  // Check if selector matches any of the collected elements
  bool SelectorMatchesAnyElement(std::string_view selector,
                                 const std::vector<CollectedElement>& elements);

  // Parse a simple selector and check if it matches an element
  bool SimpleSelectorMatchesElement(std::string_view selector,
                                    const CollectedElement& element);

  // Check if a string contains any of the patterns (case-insensitive)
  bool ContainsPattern(std::string_view str,
                       const std::vector<std::string>& patterns);

  // Check if element should be excluded based on patterns
  bool IsElementExcluded(const CollectedElement& element);

  // Check if element should be included based on patterns
  bool IsElementIncluded(const CollectedElement& element);

  // Index config_.measured_above_fold_selectors for lookup. Called from every
  // constructor; config_ is fixed after construction, so this runs once per
  // extractor and never inside the rule walk.
  void BuildMeasuredFoldIndex();

  // True when a real browser render measured THIS element above the fold.
  //
  // Deliberately a raw set lookup rather than a trip through
  // SimpleSelectorMatchesElement: the tokens hold DOM attribute values and so
  // do CollectedElement::id / ::classes, whereas the selector parser expects
  // CSS identifier syntax and would need `dark\:bg-stone-900` where the DOM
  // says `dark:bg-stone-900`. Comparing the raw values is both correct and
  // O(1) — and this runs per element, per selector, over the whole sheet.
  bool MeasuredFoldContains(const CollectedElement& element) const;

  CriticalCssConfig config_;

  // Raw DOM id / class values from config_.measured_above_fold_selectors.
  // Tokens of any other shape are dropped here — see the config field.
  absl::flat_hash_set<std::string> measured_fold_ids_;
  absl::flat_hash_set<std::string> measured_fold_classes_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CRITICAL_CSS_EXTRACTOR_H_
