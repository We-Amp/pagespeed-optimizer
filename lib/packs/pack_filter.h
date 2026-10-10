// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_PACKS_PACK_FILTER_H_
#define PAGESPEED_LIB_PACKS_PACK_FILTER_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"
#include "lib/packs/decision.h"
#include "lib/packs/pack.h"
#include "lib/packs/page_facts.h"
#include "lib/packs/planner.h"
#include "lib/packs/template.h"

namespace pagespeed::packs {

// Splits an absolute http(s) page URL into scheme, host (lowercased, port
// stripped), path (starts with '/') and query (without '?'). The fragment is
// dropped. Returns nullopt for anything else.
std::optional<PageUrl> ParsePageUrl(std::string_view url);

struct PackFilterOptions {
  // Put an HTML comment before every element the pack inserted or changed
  // (enforce only).
  bool debug_comments = false;
  size_t max_added_bytes = kMaxAddedBytesPerPage;
};

// Applies the rules of a transform pack (canonical, title, description,
// hreflang and jsonld) to one HTML document.
//
// The filter collects facts while the document streams by (StartElement,
// EndElement, Characters), then decides and mutates only at EndDocument,
// after the plan for the whole page is complete. It relies on the whole
// document being one flush window, as the worker's HTML pass runs it; a node
// that is no longer rewritable at that point makes the filter drop the whole
// plan and leave the page untouched.
//
// The head ends at </head>, at <body>, or at the first element that cannot be
// head content, whichever comes first; inserts go there. If an inert element
// (svg, noscript, ...) is still open at that point the page is skipped
// (kMalformedHead).
//
// A rule changes the HTML only when its effective mode is `enforce`
// (the lowest of the global mode, the matching site's mode and the rule's
// own opt-in). Otherwise the filter still records what the rule would do in
// decisions() and leaves the document byte-identical.
//
// The filter holds the pack alive for the duration of the pass. Its output
// depends only on the page URL and the HTML, never on the device class.
class PackFilter : public net_instaweb::EmptyHtmlFilter {
 public:
  PackFilter(net_instaweb::HtmlParse* parser, std::shared_ptr<const Pack> pack,
             PageUrl url, Mode global_mode, PackFilterOptions options = {});

  const char* Name() const override { return "PackFilter"; }
  bool CanModifyUrls() override { return can_modify_urls_; }
  ScriptUsage GetScriptUsage() const override {
    return may_inject_scripts_ ? kMayInjectScripts : kNeverInjectsScripts;
  }

  void StartDocument() override;
  void StartElement(net_instaweb::HtmlElement* element) override;
  void EndElement(net_instaweb::HtmlElement* element) override;
  void Characters(net_instaweb::HtmlCharactersNode* characters) override;
  void EndDocument() override;

  // True when the filter changed the document. When false the caller serves
  // the ORIGINAL bytes: the kernel re-serializes some tag whitespace, so the
  // filter's own output can differ from the input without any rule acting.
  bool modified() const { return modified_; }
  // True when at least one rule has something to do on this page, whatever
  // its mode (an enforcing rule that did it, or a report-only rule that
  // would).
  bool would_modify() const { return would_modify_; }
  // Why the page was left alone as a whole (kNone when it was evaluated).
  SkipReason skip_reason() const { return skip_reason_; }
  // One decision per applicable rule, in application order.
  const std::vector<PackDecision>& decisions() const { return decisions_; }
  const PageUrl& url() const { return url_; }

 private:
  bool Rewritable(const Plan& plan) const;
  void Apply(const Plan& plan);
  void ApplyOp(const Rule& rule, const PackDecision& decision,
               const PlanOp& op);
  void AddDebugComment(const Rule& rule, const PackDecision& decision,
                       net_instaweb::HtmlElement* before);
  void EndLogicalHead(net_instaweb::HtmlElement* ender);
  void InsertAtHeadEnd(net_instaweb::HtmlNode* node);
  void SetEscaped(net_instaweb::HtmlElement* el,
                  net_instaweb::HtmlName::Keyword kw,
                  const std::string& escaped);

  net_instaweb::HtmlParse* parser_;
  std::shared_ptr<const Pack> pack_;
  PageUrl url_;
  Mode global_mode_;
  PackFilterOptions options_;
  bool can_modify_urls_ = false;
  bool may_inject_scripts_ = false;

  PageFacts facts_;
  std::vector<net_instaweb::HtmlElement*> inert_;
  net_instaweb::HtmlElement* open_head_ = nullptr;
  int open_title_ = -1;  // index into facts_.titles
  std::string open_title_raw_;
  int open_jsonld_ = -1;  // index into facts_.jsonlds
  const Rule* commented_rule_ = nullptr;

  bool modified_ = false;
  bool would_modify_ = false;
  SkipReason skip_reason_ = SkipReason::kNone;
  std::vector<PackDecision> decisions_;
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_LIB_PACKS_PACK_FILTER_H_
