// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_WORKER_HTML_TRANSFORM_FILTER_H_
#define PAGESPEED_SRC_WORKER_HTML_TRANSFORM_FILTER_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

class PageSpeedCache;

// Configuration for HtmlTransformFilter.
struct HtmlTransformConfig {
  bool enable_critical_css = true;
  bool enable_lazy_load = true;
  bool enable_image_dimensions = true;
  bool enable_lcp_preload = true;
  bool enable_preconnect_injection = true;
  bool enable_font_preload = false;       // opt-in
  bool enable_speculation_rules = false;  // opt-in
  bool enable_async_css = false;          // opt-in, requires critical CSS
  bool enable_script_deferral = false;    // opt-in, requires browser analysis
};

// A unified HtmlFilter that applies HTML transformations in a single
// parse pass. Extends EmptyHtmlFilter so only the needed callbacks
// are overridden.
//
// Usage:
//   NullMessageHandler message_handler;
//   HtmlParse parser(&message_handler);
//   HtmlTransformFilter transform(&parser, config, critical_css,
//                                 cache, hostname);
//   parser.AddFilter(&transform);
//   HtmlWriterFilter writer(&parser);
//   writer.set_writer(&string_writer);
//   parser.AddFilter(&writer);
//   parser.StartParse(url);
//   parser.ParseText(html);
//   parser.FinishParse();
//   // string_writer now contains the transformed HTML
class HtmlTransformFilter : public net_instaweb::EmptyHtmlFilter {
 public:
  // Constructs the filter.
  // parser: The HtmlParse instance (needed for MakeName, NewElement, etc.)
  // config: Toggles for each transformation.
  // critical_css: CSS to inject as <style data-pagespeed-critical>.
  //               Empty string means no critical CSS injection.
  // cache: Cache for reading image dimensions. May be nullptr.
  // hostname: Current hostname for cache lookups.
  // scheme: URL scheme for cache lookups (e.g., "https").
  HtmlTransformFilter(
      net_instaweb::HtmlParse* parser, const HtmlTransformConfig& config,
      std::string_view critical_css, PageSpeedCache* cache,
      std::string_view hostname, std::string_view scheme,
      LcpCandidate lcp_candidate = {},
      const std::vector<PreconnectOrigin>& preconnect_origins = {},
      const std::vector<std::string>& speculation_urls = {},
      const std::vector<std::string>& defer_scripts = {},
      const std::vector<std::string>& font_urls = {});

  const char* Name() const override { return "HtmlTransform"; }

  ScriptUsage GetScriptUsage() const override {
    return (config_.enable_speculation_rules || config_.enable_async_css)
               ? kMayInjectScripts
               : kNeverInjectsScripts;
  }

  void StartDocument() override;
  void StartElement(net_instaweb::HtmlElement* element) override;
  void EndElement(net_instaweb::HtmlElement* element) override;
  void EndDocument() override;

  // The page's cascade-layer order (Worker::BuildCombinedCss).
  // Decides, with DecideCriticalCssLayerPlacement, whether a critical block
  // that names a layer can still go before the sheets, with an `@layer`
  // statement in front. Unset (the default) is "not proven": a layered block
  // keeps the old placement.
  void set_cascade_layer_order(CascadeLayerOrder order) {
    layer_order_ = std::move(order);
  }

  // Returns true if any transformation was applied.
  bool modified() const { return modified_; }

  // Returns true if critical CSS was successfully injected.
  bool critical_css_injected() const { return critical_css_injected_; }

  // Check if an attribute value is a valid pixel dimension (not
  // percentage, not zero, not empty).
  static bool IsValidPixelDimension(const char* value);

  // Check if a URL scheme is safe for preload injection.
  // Returns true only for https://, http://, or path-absolute (/) URLs.
  // Rejects javascript:, data:, protocol-relative (//), etc.
  static bool IsAllowedPreloadUrl(std::string_view url);

 private:
  // Inject the critical <style>. Called at </head> (or </body> as fallback);
  // the block is placed BEFORE the first stylesheet source seen so far
  // (critical_css_anchor_; a <style>, a stylesheet <link>, or an author's
  // loadCSS preload) when there is one, else before the current
  // end tag.
  //
  // WHY BEFORE THE SHEET. The block duplicates rules of the
  // page's own stylesheets, filtered to the fold and to the variant's viewport
  // class. Two rules of equal specificity resolve by source order, so a block
  // that FOLLOWS the sheet wins every tie against it once the sheet applies —
  // permanently, in the fully-styled page, not only during the flash window.
  // Concretely: the extractor drops a `@media` override that does not apply to
  // the variant's viewport class while keeping the element's base rule, and a
  // visitor whose actual window falls under that override (a desktop-UA browser
  // at 375 px, a phone in landscape) then sees the block's base rule beat the
  // sheet's override. Placed before every stylesheet source it derives from,
  // the block is a first-paint bridge only: whatever it carries, the author's
  // cascade wins as soon as the real sheets apply.
  //
  // CASCADE LAYERS. A layer's position is fixed by its first mention, and a
  // block placed first would mention its layers in the combined sheet's order,
  // which is not the document's when an inline <style> follows a <link> or a
  // layered sheet is missing from the combined sheet. So a block that names a
  // layer goes first only with an `@layer a, b, …;` statement in front that
  // lists every layer of the page in the document's order, and only when that
  // order is proven (layer_order_; DecideCriticalCssLayerPlacement,
  // cascade_layer_order.h). Otherwise it keeps the end-of-head placement before
  // the end tag, and with it the end-of-head behaviour for overrides the block
  // dropped.
  void InjectCriticalCss(net_instaweb::HtmlElement* head_element);

  // Maintain critical_css_anchor_: the first <link rel="stylesheet"> or <style>
  // seen since the last head-prelude element (<meta charset>, <meta
  // http-equiv="Content-Type" | "Content-Security-Policy">, <base>), outside
  // <noscript>/<noembed>/<noframes>/<template>/<svg>/<math> (a block inserted
  // there would not apply, or not as CSS). The block must precede the
  // stylesheet sources it duplicates (see InjectCriticalCss) but must never
  // move ahead of the charset declaration (which has to stay in the first 1024
  // bytes), a meta CSP (a policy governs only what follows it) or <base>; a
  // stylesheet source that precedes one of those is not a valid anchor, and
  // the next source after it is. The validator's StripStylesheetSources
  // (critical_css_validator.cc) mirrors this rule on the string level; change
  // both together.
  void TrackCriticalCssAnchor(net_instaweb::HtmlElement* element);

  // Apply loading="lazy" or fetchpriority="high" to img/iframe.
  void ApplyLazyLoad(net_instaweb::HtmlElement* element);

  // Inject width/height on img elements missing dimensions.
  void ApplyImageDimensions(net_instaweb::HtmlElement* element);

  // Inject <link rel="preload" as="image"> for LCP candidate.
  void InjectLcpPreload(net_instaweb::HtmlElement* head_element);

  // Inject <link rel="preconnect"> for third-party origins.
  void InjectPreconnectLinks(net_instaweb::HtmlElement* head_element);

  // Inject <link rel="preload" as="font"> for font URLs.
  void InjectFontPreload(net_instaweb::HtmlElement* head_element);

  // Inject <script type="speculationrules"> before </body>.
  void InjectSpeculationRules(net_instaweb::HtmlElement* body_element);

  // Convert render-blocking <link rel="stylesheet"> to async pattern.
  void ApplyAsyncCss(net_instaweb::HtmlElement* element);

  // Inject the CSP-safe external loader <script> once per document (only when
  // at least one stylesheet was deferred by ApplyAsyncCss).
  void InjectAsyncCssLoader(net_instaweb::HtmlElement* element);

  // Remove previously-applied async CSS markers for revalidation cleanup.
  void RemoveAsyncCssMarkers(net_instaweb::HtmlElement* element);

  // Put a deferred <link> back the way the author wrote it: rel="preload" ->
  // rel="stylesheet", drop the `as` ApplyAsyncCss added, restore the recorded
  // media, and drop the swap bookkeeping. The attribute half of both the
  // revalidation cleanup and the same-pass revert below — a revert that failed
  // to restore rel would ship a page whose stylesheet is never consumed by
  // anything, so this is the load-bearing half of RevertAsyncCss.
  void RestoreDeferredLink(net_instaweb::HtmlElement* element);

  // Undo every async-CSS conversion made in this pass. Deferring a stylesheet
  // is only safe because the inlined critical CSS paints the fold meanwhile;
  // when that inline block turns out not to ship, the deferral has no bridge
  // and must not ship either. The decision cannot be made at conversion time:
  // the <link> is converted at StartElement, and a CSP <meta> LATER in the head
  // still governs the inline <style> whose injection is decided at </head>, so
  // a source order of <link> before <meta> would otherwise defer past the very
  // policy that suppresses the bridge. Reverting at EndDocument is when the
  // answer is known, and the whole document is one flush window on every entry
  // point the filter is driven from, so the nodes are still rewritable.
  void RevertAsyncCss();

  // Add defer to scripts identified as safe to defer.
  void ApplyScriptDeferral(net_instaweb::HtmlElement* element);

  // Detect a <meta http-equiv="Content-Security-Policy" content="..."> tag and
  // accumulate its policy into meta_csps_. Enforcing metas only:
  // Content-Security-Policy-Report-Only is ignored (it never blocks).
  void CollectMetaCsp(net_instaweb::HtmlElement* element);

  // Whether every collected meta CSP would honor an inline <style> we inject.
  // True when no enforcing meta CSP was seen. Combines restrictively: inline is
  // permitted only if permitted under EVERY collected policy.
  bool MetaCspAllowsInlineStyle() const;

  // As above, for an inline <script> (speculation rules).
  bool MetaCspAllowsInlineScript() const;

  net_instaweb::HtmlParse* parser_;
  HtmlTransformConfig config_;
  std::string critical_css_;
  CascadeLayerOrder layer_order_;
  PageSpeedCache* cache_;
  std::string hostname_;
  std::string scheme_;

  // Without an LCP candidate, the first plausible body image inside this
  // window gets the fetchpriority="high" fallback; with a candidate, the
  // same window is the above-fold lazy-load guard.
  static constexpr int kAboveFoldImgWindow = 3;
  // Body iframes inside this window are exempt from lazy-loading: a
  // top-of-page embed (video player etc.) is almost always the first
  // iframe, and lazy-loading it regresses LCP.  Later iframes (comment
  // widgets, ads) are safely below the fold.  Only visible iframes
  // without an existing loading attribute count toward the window
  // (invisible ones get no transform and must not consume the slot).
  static constexpr int kAboveFoldIframeWindow = 1;

  bool modified_ = false;
  bool critical_css_injected_ = false;
  // See TrackCriticalCssAnchor. Null until a stylesheet source is seen; reset
  // by a head-prelude element. Only dereferenced through the parser after an
  // IsRewritable check, so a node flushed out of the window is skipped, not
  // touched.
  net_instaweb::HtmlElement* critical_css_anchor_ = nullptr;
  bool in_head_ = false;
  bool in_body_ = false;
  bool fallback_priority_applied_ = false;
  // Whether the current <picture> has a <source srcset> (see StartElement).
  bool picture_has_source_srcset_ = false;
  int body_img_count_ = 0;
  int body_iframe_count_ = 0;
  LcpCandidate lcp_candidate_;
  bool lcp_preload_injected_ = false;
  std::vector<PreconnectOrigin> preconnect_origins_;
  bool preconnect_injected_ = false;
  std::vector<std::string> speculation_urls_;
  bool speculation_rules_injected_ = false;
  std::vector<std::string> defer_scripts_;
  std::vector<std::string> font_urls_;
  bool font_preload_injected_ = false;
  bool async_css_loader_injected_ = false;

  // The <link>s converted by ApplyAsyncCss in this pass, and the nodes it
  // injected alongside them (<noscript> twins + the loader <script>), so
  // RevertAsyncCss can put the document back exactly as it found it.
  std::vector<net_instaweb::HtmlElement*> deferred_css_links_;
  std::vector<net_instaweb::HtmlNode*> async_css_injected_nodes_;

  // Enforcing CSPs seen via <meta http-equiv="Content-Security-Policy">, in
  // document order. Multiple metas combine restrictively at gate time. Meta
  // always precedes </head>/</body>, so it is collected before the inject
  // sites are reached. Header-delivered CSP is NOT visible here — see the
  // TODO(2.S9-phase2) note in lib/html/csp_inline_policy.h.
  std::vector<std::string> meta_csps_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_HTML_TRANSFORM_FILTER_H_
