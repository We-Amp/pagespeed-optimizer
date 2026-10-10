// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_PACKS_PAGE_FACTS_H_
#define PAGESPEED_LIB_PACKS_PAGE_FACTS_H_

#include <string>
#include <vector>

#include "lib/packs/pack.h"

namespace net_instaweb {
class HtmlCharactersNode;
class HtmlElement;
}  // namespace net_instaweb

namespace pagespeed::packs {

// One element of a rule's kind found in the main document (never inside
// svg, math, template, noscript, noembed or noframes). The planner treats the
// element pointers as opaque handles; only the applier dereferences them.
struct ElementFact {
  net_instaweb::HtmlElement* element = nullptr;
  bool in_head = false;
  // canonical: the decoded href; title: the decoded text; description: the
  // decoded content. Not normalized.
  std::string value;
  // canonical: href present; description: content present.
  bool has_value_attr = false;
  // canonical only: the rel tokens other than "canonical", as written.
  std::vector<std::string> other_rel_tokens;
  // title only: the text nodes that make up its content.
  std::vector<net_instaweb::HtmlCharactersNode*> text_nodes;
};

// What the traversal collected. Filled in StartElement/EndElement/Characters,
// read once at the end of the document.
struct PageFacts {
  net_instaweb::HtmlElement* head = nullptr;  // first explicit <head>
  std::string base_href;                      // first <base href>, decoded
  std::vector<ElementFact> canonicals;
  std::vector<ElementFact> titles;
  std::vector<ElementFact> descriptions;

  const std::vector<ElementFact>& For(Kind kind) const {
    switch (kind) {
      case Kind::kCanonical:
        return canonicals;
      case Kind::kTitle:
        return titles;
      default:
        return descriptions;
    }
  }
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_LIB_PACKS_PAGE_FACTS_H_
