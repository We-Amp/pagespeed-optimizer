// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_PACKS_PLANNER_H_
#define PAGESPEED_LIB_PACKS_PLANNER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lib/packs/decision.h"
#include "lib/packs/pack.h"
#include "lib/packs/page_facts.h"
#include "lib/packs/template.h"

namespace pagespeed::packs {

// What is known about the page being rewritten, besides its HTML.
struct PageContext {
  PageUrl url;
  Mode global_mode = Mode::kReport;
};

enum class SkipReason {
  kNone,
  kHostNotListed,
  kNoHead,
  kMalformedHead,
  kNotRewritable,
  kSizeLimit,
  kError,
};
std::string_view SkipReasonName(SkipReason reason);

enum class OpType {
  kInsert,                // append a new element to <head>
  kRemove,                // delete the element
  kRemoveCanonicalToken,  // keep the element, drop "canonical" from rel
  kSetValue,              // set href / content / title text of the element
};

struct PlanOp {
  OpType type = OpType::kInsert;
  Kind kind = Kind::kCanonical;
  ElementFact target;  // the existing element (not for kInsert)
  // kInsert, kSetValue: the attribute value (canonical, description, the href
  // of an hreflang link), the already HTML-escaped text (title) or the script
  // text (jsonld).
  std::string value;
  // hreflang, kInsert: the (normalized) language code of the new link.
  std::string code;
};

struct RulePlan {
  const Rule* rule = nullptr;
  PackDecision decision;
  std::vector<PlanOp> ops;  // empty when the decision is a no-op
};

struct Plan {
  SkipReason skip = SkipReason::kNone;
  std::vector<RulePlan> rules;  // in kKindOrder; one per applicable rule
  size_t added_bytes = 0;
};

// Pure: decides, from the pack, the page URL and the facts collected from the
// HTML, what each rule does on this page. Nothing here touches HTML. Plans
// the canonical, title and description kinds; other kinds are ignored.
//
// The page is skipped whole (no rule plans) when its host is not listed, when
// no explicit <head> exists, or when the bytes to add exceed
// `max_added_bytes`.
//
// `extra_bytes_per_change` is added to the byte count of every insert or
// value change (room for the debug comment the filter may write).
Plan BuildPlan(const Pack& pack, const PageContext& ctx, const PageFacts& facts,
               size_t max_added_bytes = kMaxAddedBytesPerPage,
               size_t extra_bytes_per_change = 0);

// Escapes `s` for use as an attribute value (inside double quotes): & < > "
// and ' only, so UTF-8 passes through untouched.
std::string EscapeAttributeValue(std::string_view s);

// Whitespace-collapses (ASCII whitespace runs become one space, ends
// trimmed). Exposed for the filter and tests.
std::string CollapseWhitespace(std::string_view s);

// Resolves `ref` against the absolute URL `base` (RFC 3986, without
// normalization beyond dot segments). Returns `ref` unchanged when it is
// already absolute, and an empty string when `base` is not absolute.
std::string ResolveUrl(std::string_view base, std::string_view ref);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_LIB_PACKS_PLANNER_H_
