// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/planner.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "lib/html/html_keywords.h"
#include "lib/html/safe_entity_decode.h"
#include "lib/packs/matcher.h"
#include "lib/packs/url_norm.h"

namespace pagespeed::packs {

std::string_view SkipReasonName(SkipReason reason) {
  switch (reason) {
    case SkipReason::kNone:
      return "none";
    case SkipReason::kHostNotListed:
      return "host_not_listed";
    case SkipReason::kNoHead:
      return "no_head";
    case SkipReason::kNotRewritable:
      return "not_rewritable";
    case SkipReason::kSizeLimit:
      return "size_limit";
    case SkipReason::kError:
      return "error";
  }
  return "error";
}

namespace {

bool IsWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool HasScheme(std::string_view s) {
  if (s.empty() || !absl::ascii_isalpha(static_cast<unsigned char>(s[0]))) {
    return false;
  }
  for (size_t i = 1; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ':') return true;
    if (!(absl::ascii_isalnum(c) || c == '+' || c == '-' || c == '.')) {
      return false;
    }
  }
  return false;
}

// Removes "." and ".." segments from the path part of `s` (before any ? or
// #), leaving the rest untouched.
std::string RemoveDotSegments(std::string_view s) {
  const size_t tail_at = s.find_first_of("?#");
  std::string_view path = s.substr(0, tail_at);
  std::string_view tail = tail_at == std::string_view::npos ? std::string_view()
                                                            : s.substr(tail_at);
  std::vector<std::string_view> out;
  std::vector<std::string_view> parts = absl::StrSplit(path, '/');
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string_view p = parts[i];
    const bool last = i + 1 == parts.size();
    if (p == ".") {
      if (last) out.push_back("");
    } else if (p == "..") {
      if (out.size() > 1) out.pop_back();
      if (last) out.push_back("");
    } else {
      out.push_back(p);
    }
  }
  return absl::StrCat(absl::StrJoin(out, "/"), tail);
}

}  // namespace

std::string CollapseWhitespace(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  for (char c : s) {
    if (IsWs(c)) {
      pending_space = !out.empty();
    } else {
      if (pending_space) out.push_back(' ');
      pending_space = false;
      out.push_back(c);
    }
  }
  return out;
}

std::string ResolveUrl(std::string_view base, std::string_view ref) {
  while (!ref.empty() && IsWs(ref.front())) ref.remove_prefix(1);
  while (!ref.empty() && IsWs(ref.back())) ref.remove_suffix(1);
  if (HasScheme(ref)) return std::string(ref);
  const size_t scheme_end = base.find("://");
  if (scheme_end == std::string_view::npos) return std::string();
  const size_t frag = base.find('#');
  if (frag != std::string_view::npos) base = base.substr(0, frag);
  size_t auth_end = base.find_first_of("/?", scheme_end + 3);
  if (auth_end == std::string_view::npos) auth_end = base.size();
  const std::string_view origin = base.substr(0, auth_end);
  const std::string_view rest = base.substr(auth_end);
  const size_t q = rest.find('?');
  std::string_view path = rest.substr(0, q);
  std::string_view query =
      q == std::string_view::npos ? std::string_view() : rest.substr(q);
  if (path.empty()) path = "/";
  if (ref.empty()) return absl::StrCat(origin, path, query);
  if (ref.starts_with("//")) {
    return absl::StrCat(base.substr(0, scheme_end + 1), ref);
  }
  if (ref.front() == '#') return absl::StrCat(origin, path, query, ref);
  if (ref.front() == '?') return absl::StrCat(origin, path, ref);
  if (ref.front() == '/') return absl::StrCat(origin, RemoveDotSegments(ref));
  const size_t slash = path.rfind('/');
  return absl::StrCat(
      origin, RemoveDotSegments(absl::StrCat(path.substr(0, slash + 1), ref)));
}

namespace {

// ---------------------------------------------------------------------------
// Target values
// ---------------------------------------------------------------------------

enum class TargetStatus { kOk, kNoValue, kInvalid };

struct Target {
  TargetStatus status = TargetStatus::kNoValue;
  // What gets written: the href / content, or for a title the HTML-escaped
  // text.
  std::string value;
  // What it compares as: NormUrl() for a canonical, the whitespace-collapsed
  // decoded text otherwise.
  std::string norm;
};

std::string LogicalText(Kind kind, std::string_view value) {
  if (kind == Kind::kTitle) {
    return CollapseWhitespace(net_instaweb::SafeDecodeHtmlEntities(value));
  }
  return CollapseWhitespace(value);
}

Target ComputeTarget(const Pack& pack, const Rule& rule, const RuleHit& hit,
                     const PageUrl& url) {
  Target t;
  ExpandContext ex;
  ex.url = url;
  ex.captures = hit.captures;
  const Kind kind = rule.kind;
  const EscapeContext esc =
      kind == Kind::kTitle ? EscapeContext::kHtmlText : EscapeContext::kNone;
  const size_t max_bytes = kind == Kind::kCanonical ? kMaxUrlBytes
                           : kind == Kind::kTitle   ? kMaxTitleChars * 8
                                                    : kMaxDescriptionChars * 8;

  std::string raw;
  switch (rule.value.source) {
    case ValueSource::kTemplate: {
      auto s = ExpandTemplate(rule.value.tpl, ex, esc, max_bytes);
      if (!s.ok()) {
        t.status = TargetStatus::kInvalid;
        return t;
      }
      raw = *std::move(s);
      break;
    }
    case ValueSource::kTable: {
      const std::string* hitv = nullptr;
      auto it = pack.tables.find(rule.value.table);
      if (it != pack.tables.end()) {
        hitv = LookupTableValue(it->second, url.path, url.query);
      }
      if (hitv != nullptr) {
        raw = kind == Kind::kTitle ? EscapeForHtmlText(*hitv) : *hitv;
      } else if (rule.value.fallback_tpl.has_value()) {
        auto s = ExpandTemplate(*rule.value.fallback_tpl, ex, esc, max_bytes);
        if (!s.ok()) {
          t.status = TargetStatus::kInvalid;
          return t;
        }
        raw = *std::move(s);
      } else {
        t.status = TargetStatus::kNoValue;
        return t;
      }
      break;
    }
    case ValueSource::kPattern:
      t.status = TargetStatus::kNoValue;
      return t;
  }

  if (kind == Kind::kCanonical) {
    if (!CheckUrlValue(raw).ok()) {
      t.status = TargetStatus::kInvalid;
      return t;
    }
    std::optional<std::string> n = NormUrl(raw);
    if (!n.has_value()) {
      t.status = TargetStatus::kInvalid;
      return t;
    }
    t.norm = *std::move(n);
  } else {
    t.norm = LogicalText(kind, raw);
    const size_t limit =
        kind == Kind::kTitle ? kMaxTitleChars : kMaxDescriptionChars;
    if (t.norm.empty() || HasControlChars(t.norm) ||
        Utf8Length(t.norm) > limit) {
      t.status = TargetStatus::kInvalid;
      return t;
    }
  }
  t.value = std::move(raw);
  t.status = TargetStatus::kOk;
  return t;
}

// ---------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------

struct Cand {
  const ElementFact* fact = nullptr;
  std::string norm;     // empty when blank or unparsable
  bool blank = false;   // no value at all
  bool usable = false;  // in <head> and carries a valid value
};

std::vector<Cand> MakeCands(Kind kind, const std::vector<ElementFact>& facts,
                            const std::string& base) {
  std::vector<Cand> out;
  out.reserve(facts.size());
  for (const ElementFact& f : facts) {
    Cand c;
    c.fact = &f;
    if (kind == Kind::kCanonical) {
      const std::string trimmed = CollapseWhitespace(f.value);
      c.blank = trimmed.empty();
      if (!c.blank) {
        std::optional<std::string> n = NormUrl(ResolveUrl(base, trimmed));
        if (n.has_value()) c.norm = *std::move(n);
      }
    } else {
      c.norm = CollapseWhitespace(f.value);
      c.blank = c.norm.empty();
    }
    c.usable = f.in_head && !c.norm.empty();
    out.push_back(std::move(c));
  }
  return out;
}

std::string KindPrefix(Kind kind) { return std::string(KindName(kind)); }
std::string MissingDefect(Kind kind) {
  return kind == Kind::kCanonical ? "canonical-missing"
                                  : absl::StrCat(KindPrefix(kind), "-missing");
}
std::string MultipleDefect(Kind kind) {
  return absl::StrCat(KindPrefix(kind), "-multiple");
}
// Only the canonical has a lens defect for an unusable element; for a title
// or description the same situation is "missing".
std::string UnusableDefect(Kind kind) {
  return kind == Kind::kCanonical ? "canonical-unusable" : MissingDefect(kind);
}

Reason UnusableReason(const Cand& c) {
  if (c.blank) return Reason::kEmpty;
  return c.fact->in_head ? Reason::kUnusable : Reason::kOutOfHead;
}

PlanOp RemoveOp(Kind kind, const Cand& c) {
  PlanOp op;
  op.kind = kind;
  op.target = *c.fact;
  op.type = (kind == Kind::kCanonical && !c.fact->other_rel_tokens.empty())
                ? OpType::kRemoveCanonicalToken
                : OpType::kRemove;
  return op;
}

PlanOp SetOp(Kind kind, const Cand& c, const Target& t) {
  PlanOp op;
  op.type = OpType::kSetValue;
  op.kind = kind;
  op.target = *c.fact;
  op.value = t.value;
  return op;
}

PlanOp InsertOp(Kind kind, const Target& t) {
  PlanOp op;
  op.type = OpType::kInsert;
  op.kind = kind;
  op.value = t.value;
  return op;
}

size_t DistinctNorms(const std::vector<Cand>& cands) {
  std::map<std::string, int> seen;
  for (const Cand& c : cands) {
    if (!c.norm.empty()) seen[c.norm]++;
  }
  return seen.size();
}

void Finish(RulePlan* rp, Action action, Reason reason, std::string defect) {
  rp->decision.action = action;
  rp->decision.reason = reason;
  rp->decision.defect = std::move(defect);
}

// Records the hash of the first existing value as the "old" value.
void SetOldHash(RulePlan* rp, const std::vector<Cand>& cands) {
  for (const Cand& c : cands) {
    if (!c.norm.empty()) {
      rp->decision.old_hash = HashValue(c.norm);
      return;
    }
  }
}

// keep: act only when the kind is absent from the whole document; otherwise
// remove exact duplicates and nothing else.
void PlanKeep(Kind kind, const std::vector<Cand>& cands, const Target& t,
              RulePlan* rp) {
  if (cands.empty()) {
    rp->ops.push_back(InsertOp(kind, t));
    Finish(rp, Action::kInsert, Reason::kAbsent, MissingDefect(kind));
    return;
  }
  // Within a group of equal values keep one (the first in <head>, else the
  // first), remove the rest.
  std::map<std::string, const Cand*> keeper;
  for (const Cand& c : cands) {
    if (c.norm.empty()) continue;
    auto [it, inserted] = keeper.emplace(c.norm, &c);
    if (!inserted && !it->second->fact->in_head && c.fact->in_head) {
      it->second = &c;
    }
  }
  size_t removed = 0;
  std::string old_norm;
  for (const Cand& c : cands) {
    if (c.norm.empty() || keeper[c.norm] == &c) continue;
    rp->ops.push_back(RemoveOp(kind, c));
    if (old_norm.empty()) old_norm = c.norm;
    ++removed;
  }
  const size_t distinct = keeper.size();
  if (removed > 0) {
    rp->decision.old_hash = HashValue(old_norm);
    Finish(rp, Action::kDedupe, Reason::kDuplicate,
           distinct > 1 ? MultipleDefect(kind) : std::string());
  } else if (distinct > 1) {
    Finish(rp, Action::kNone, Reason::kConflict, MultipleDefect(kind));
  } else if (distinct == 0) {
    Finish(rp, Action::kNone, Reason::kEmpty, UnusableDefect(kind));
  } else if (keeper.begin()->first == t.norm) {
    Finish(rp, Action::kNone, Reason::kEqual, std::string());
  } else {
    Finish(rp, Action::kNone, Reason::kPresent, std::string());
  }
}

// repair: make sure one usable element exists; leave a usable one alone.
void PlanRepair(Kind kind, const std::vector<Cand>& cands, const Target& t,
                RulePlan* rp) {
  if (cands.empty()) {
    rp->ops.push_back(InsertOp(kind, t));
    Finish(rp, Action::kInsert, Reason::kAbsent, MissingDefect(kind));
    return;
  }
  const Cand* first_usable = nullptr;
  for (const Cand& c : cands) {
    if (c.usable) {
      first_usable = &c;
      break;
    }
  }
  if (first_usable == nullptr) {
    // Nothing usable. Rewrite the first element that sits in <head> in
    // place; otherwise add one. Everything else goes.
    const Cand* reuse = nullptr;
    for (const Cand& c : cands) {
      if (c.fact->in_head) {
        reuse = &c;
        break;
      }
    }
    for (const Cand& c : cands) {
      if (&c != reuse) rp->ops.push_back(RemoveOp(kind, c));
    }
    if (reuse != nullptr) {
      rp->ops.insert(rp->ops.begin(), SetOp(kind, *reuse, t));
      if (!reuse->norm.empty()) {
        rp->decision.old_hash = HashValue(reuse->norm);
      }
      Finish(rp, reuse->blank ? Action::kFill : Action::kReplace,
             UnusableReason(*reuse), UnusableDefect(kind));
    } else {
      rp->ops.push_back(InsertOp(kind, t));
      SetOldHash(rp, cands);
      Finish(rp, Action::kReplace, Reason::kOutOfHead, UnusableDefect(kind));
    }
    return;
  }
  // A usable element exists: it stays. Duplicates, conflicts and unusable
  // leftovers are removed.
  bool conflict = false;
  bool unusable = false;
  Reason unusable_reason = Reason::kUnusable;
  std::string old_norm;
  for (const Cand& c : cands) {
    if (&c == first_usable) continue;
    if (c.usable) {
      if (c.norm != first_usable->norm) conflict = true;
    } else if (!unusable) {
      unusable = true;
      unusable_reason = UnusableReason(c);
    }
    if (old_norm.empty()) old_norm = c.norm;
    rp->ops.push_back(RemoveOp(kind, c));
  }
  if (rp->ops.empty()) {
    Finish(rp, Action::kNone,
           first_usable->norm == t.norm ? Reason::kEqual : Reason::kPresent,
           std::string());
    return;
  }
  rp->decision.old_hash = HashValue(old_norm);
  if (conflict) {
    Finish(rp, Action::kRemove, Reason::kConflict, MultipleDefect(kind));
  } else if (unusable) {
    Finish(rp, Action::kRemove, unusable_reason,
           kind == Kind::kCanonical ? UnusableDefect(kind) : std::string());
  } else {
    Finish(rp, Action::kDedupe, Reason::kDuplicate, std::string());
  }
}

// replace: the document ends with exactly the rule's value.
void PlanReplace(Kind kind, const std::vector<Cand>& cands, const Target& t,
                 RulePlan* rp) {
  if (cands.empty()) {
    rp->ops.push_back(InsertOp(kind, t));
    Finish(rp, Action::kInsert, Reason::kAbsent, MissingDefect(kind));
    return;
  }
  const Cand* keeper = nullptr;  // an element already equal to the target
  const Cand* reuse = nullptr;   // else the first element in <head>
  for (const Cand& c : cands) {
    if (keeper == nullptr && c.fact->in_head && c.norm == t.norm) keeper = &c;
    if (reuse == nullptr && c.fact->in_head) reuse = &c;
  }
  const Cand* kept = keeper != nullptr ? keeper : reuse;
  bool distinct_removed = false;
  std::string old_norm;
  for (const Cand& c : cands) {
    if (&c == kept) continue;
    if (!c.norm.empty() && c.norm != t.norm) distinct_removed = true;
    if (old_norm.empty()) old_norm = c.norm;
    rp->ops.push_back(RemoveOp(kind, c));
  }
  const bool multiple = DistinctNorms(cands) > 1;
  if (keeper != nullptr) {
    if (rp->ops.empty()) {
      Finish(rp, Action::kNone, Reason::kEqual, std::string());
    } else {
      rp->decision.old_hash = HashValue(old_norm);
      Finish(rp, distinct_removed ? Action::kRemove : Action::kDedupe,
             distinct_removed ? Reason::kConflict : Reason::kDuplicate,
             distinct_removed ? MultipleDefect(kind) : std::string());
    }
    return;
  }
  if (reuse != nullptr) {
    rp->ops.insert(rp->ops.begin(), SetOp(kind, *reuse, t));
    if (!reuse->norm.empty()) rp->decision.old_hash = HashValue(reuse->norm);
    std::string defect = multiple ? MultipleDefect(kind) : std::string();
    if (defect.empty() && !reuse->usable) defect = UnusableDefect(kind);
    Finish(rp, reuse->blank ? Action::kFill : Action::kReplace,
           reuse->blank ? Reason::kEmpty : Reason::kPresent, std::move(defect));
    return;
  }
  rp->ops.push_back(InsertOp(kind, t));
  SetOldHash(rp, cands);
  Finish(rp, Action::kReplace, Reason::kOutOfHead, UnusableDefect(kind));
}

size_t AddedBytes(const RulePlan& rp) {
  size_t n = 0;
  for (const PlanOp& op : rp.ops) {
    if (op.type == OpType::kInsert) n += op.value.size() + 48;
    if (op.type == OpType::kSetValue) n += op.value.size();
  }
  return n;
}

}  // namespace

Plan BuildPlan(const Pack& pack, const PageContext& ctx, const PageFacts& facts,
               size_t max_added_bytes) {
  Plan plan;
  // Entity decoding needs the keyword tables; initialization is idempotent.
  net_instaweb::HtmlKeywords::Init();
  const std::string host = ctx.url.host;
  Selection sel = SelectRules(pack, host, ctx.url.path);
  if (sel.site == nullptr) {
    plan.skip = SkipReason::kHostNotListed;
    return plan;
  }

  bool any = false;
  for (Kind kind : {Kind::kCanonical, Kind::kTitle, Kind::kDescription}) {
    if (sel.by_kind[static_cast<int>(kind)].has_value()) any = true;
  }
  if (!any) return plan;
  if (facts.head == nullptr) {
    plan.skip = SkipReason::kNoHead;
    return plan;
  }

  const std::string page_url =
      absl::StrCat(ctx.url.scheme, "://", ctx.url.host, ctx.url.path,
                   ctx.url.query.empty() ? "" : "?", ctx.url.query);
  std::string base = page_url;
  if (!facts.base_href.empty()) {
    std::string b = ResolveUrl(page_url, facts.base_href);
    if (!b.empty() && NormUrl(b).has_value()) base = std::move(b);
  }

  for (Kind kind : {Kind::kCanonical, Kind::kTitle, Kind::kDescription}) {
    const auto& hit = sel.by_kind[static_cast<int>(kind)];
    if (!hit.has_value()) continue;
    const Rule& rule = *hit->rule;

    RulePlan rp;
    rp.rule = &rule;
    rp.decision.rule_id = rule.id;
    rp.decision.kind = kind;
    rp.decision.mode =
        EffectiveMode(ctx.global_mode, sel.site->mode, rule.enforce);

    Target target = ComputeTarget(pack, rule, *hit, ctx.url);
    if (target.status != TargetStatus::kOk) {
      Finish(&rp, Action::kNone,
             target.status == TargetStatus::kNoValue ? Reason::kNoValue
                                                     : Reason::kInvalidValue,
             std::string());
      plan.rules.push_back(std::move(rp));
      continue;
    }

    const std::vector<Cand> cands = MakeCands(kind, facts.For(kind), base);
    switch (rule.on_present) {
      case OnPresent::kKeep:
        PlanKeep(kind, cands, target, &rp);
        break;
      case OnPresent::kRepair:
        PlanRepair(kind, cands, target, &rp);
        break;
      case OnPresent::kReplace:
        PlanReplace(kind, cands, target, &rp);
        break;
    }
    for (const PlanOp& op : rp.ops) {
      if (op.type == OpType::kInsert || op.type == OpType::kSetValue) {
        rp.decision.new_hash = HashValue(target.norm);
      }
    }
    plan.added_bytes += AddedBytes(rp);
    plan.rules.push_back(std::move(rp));
  }

  if (plan.added_bytes > max_added_bytes) {
    plan.skip = SkipReason::kSizeLimit;
    plan.rules.clear();
  }
  return plan;
}

}  // namespace pagespeed::packs
