// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_PACKS_DECISION_H_
#define PAGESPEED_LIB_PACKS_DECISION_H_

#include <string>
#include <string_view>

#include "lib/packs/pack.h"

namespace pagespeed::packs {

// What a rule did (enforce) or would do (report) on one page.
enum class Action { kNone, kInsert, kFill, kReplace, kRemove, kDedupe };

// Why. The same names the decision log uses.
enum class Reason {
  kAbsent,
  kPresent,
  kDuplicate,
  kUnusable,
  kOutOfHead,
  kEmpty,
  kConflict,
  kEqual,
  kNoValue,
  kInvalidValue,
  kClusterWithoutSelf,
  kShadowed,
};

std::string_view ActionName(Action action);
std::string_view ReasonName(Reason reason);

// One rule's decision on one page. Values are recorded as hashes only, never
// as text, so a decision log cannot leak page or rule content.
struct PackDecision {
  std::string rule_id;
  Kind kind = Kind::kCanonical;
  // The effective mode of the rule on this page: `report` decisions are what
  // the rule WOULD do and changed nothing.
  Mode mode = Mode::kReport;
  Action action = Action::kNone;
  Reason reason = Reason::kPresent;
  // The defect id (as the SEO-defects lens names it) this decision clears or
  // observes; empty when the situation is not a lens defect.
  std::string defect;
  // Hash of the value before and after; empty when there was none.
  std::string old_hash;
  std::string new_hash;

  bool operator==(const PackDecision&) const = default;
};

// Stable 64-bit FNV-1a of `value`, as "fnv1a64:" plus 16 lowercase hex
// digits. Deterministic across runs and platforms (no per-process seed).
std::string HashValue(std::string_view value);

// One JSON object, no trailing newline, keys in a fixed order:
// host, path, rule, kind, mode, action, reason, defect, old, new. The path
// is cut to 300 bytes. Contains no timestamp: the caller adds `ts`.
std::string DecisionToJsonLine(std::string_view host, std::string_view path,
                               const PackDecision& decision);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_LIB_PACKS_DECISION_H_
