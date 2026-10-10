// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/planner.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
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
#include "nlohmann/json.hpp"

namespace pagespeed::packs {

std::string_view SkipReasonName(SkipReason reason) {
  switch (reason) {
    case SkipReason::kNone:
      return "none";
    case SkipReason::kHostNotListed:
      return "host_not_listed";
    case SkipReason::kNoHead:
      return "no_head";
    case SkipReason::kMalformedHead:
      return "malformed_head";
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

std::string EscapeAttributeValue(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

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
  // The value as text, for the decision log.
  std::string logical;
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
  t.logical = kind == Kind::kCanonical ? raw : t.norm;
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
  // In <head> and safe to rewrite in place: an element with other rel tokens
  // (an alternate link, say) is never reused, because its other meaning would
  // change with its href.
  bool reusable = false;
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
    c.reusable = f.in_head && f.other_rel_tokens.empty();
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

// Why a new element is inserted although one exists.
Reason InsertReason(const Cand& c) {
  if (c.blank) return Reason::kEmpty;
  return c.fact->in_head ? Reason::kPresent : Reason::kOutOfHead;
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
      rp->decision.before_hash = HashValue(c.norm);
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
    rp->decision.before_hash = HashValue(old_norm);
    Finish(rp, Action::kDedupe, Reason::kDuplicate,
           distinct > 1 ? MultipleDefect(kind) : std::string());
  } else if (distinct > 1) {
    Finish(rp, Action::kNone, Reason::kConflict, MultipleDefect(kind));
  } else if (distinct == 0) {
    // Only blank elements: keep stands down, as for any present element.
    Finish(rp, Action::kNone, Reason::kPresent, UnusableDefect(kind));
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
      if (c.reusable) {
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
        rp->decision.before_hash = HashValue(reuse->norm);
      }
      Finish(rp, reuse->blank ? Action::kFill : Action::kReplace,
             UnusableReason(*reuse), UnusableDefect(kind));
    } else {
      rp->ops.push_back(InsertOp(kind, t));
      SetOldHash(rp, cands);
      Finish(rp, Action::kReplace, InsertReason(cands.front()),
             UnusableDefect(kind));
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
  rp->decision.before_hash = HashValue(old_norm);
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
    if (reuse == nullptr && c.reusable) reuse = &c;
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
      rp->decision.before_hash = HashValue(old_norm);
      Finish(rp, distinct_removed ? Action::kRemove : Action::kDedupe,
             distinct_removed ? Reason::kConflict : Reason::kDuplicate,
             distinct_removed ? MultipleDefect(kind) : std::string());
    }
    return;
  }
  if (reuse != nullptr) {
    rp->ops.insert(rp->ops.begin(), SetOp(kind, *reuse, t));
    if (!reuse->norm.empty()) rp->decision.before_hash = HashValue(reuse->norm);
    std::string defect = multiple ? MultipleDefect(kind) : std::string();
    if (defect.empty() && !reuse->usable) defect = UnusableDefect(kind);
    Finish(rp, reuse->blank ? Action::kFill : Action::kReplace,
           reuse->blank ? Reason::kEmpty : Reason::kPresent, std::move(defect));
    return;
  }
  rp->ops.push_back(InsertOp(kind, t));
  SetOldHash(rp, cands);
  // (Only elements that cannot be reused get here: they sit outside <head>
  // or carry other rel tokens; a usable value is not a defect.)
  Finish(rp, Action::kReplace, InsertReason(cands.front()),
         cands.front().usable ? std::string() : UnusableDefect(kind));
}

// ---------------------------------------------------------------------------
// Shared: summarizing a multi-element decision
// ---------------------------------------------------------------------------

// What a multi-element rule (hreflang, jsonld) did, boiled down to the one
// decision it logs.
struct Flags {
  bool has_unusable = false;
  Reason unusable_reason = Reason::kUnusable;
  std::string unusable_defect;
  bool conflict = false;
  bool self_missing = false;
  std::string self_defect;
  bool set = false;
  bool inserted = false;
  bool duplicate = false;
  bool removed = false;
  // The defect to report when the rule acts because the kind is absent.
  std::string absent_defect;
  bool had_any = false;
  // Reported when nothing happens.
  Reason idle_reason = Reason::kPresent;
  std::string idle_defect;
};

void Summarize(RulePlan* rp, const Flags& f) {
  const bool acts = f.removed || f.set || f.inserted;
  if (!acts) {
    Finish(rp, Action::kNone, f.idle_reason, f.idle_defect);
    return;
  }
  Action action;
  if (f.set || ((f.set || f.inserted) && f.removed)) {
    action = Action::kReplace;
  } else if (f.inserted) {
    action = Action::kInsert;
  } else {
    action = f.duplicate && !f.conflict && !f.has_unusable ? Action::kDedupe
                                                           : Action::kRemove;
  }
  if (f.has_unusable) {
    Finish(rp, action, f.unusable_reason, f.unusable_defect);
  } else if (f.conflict) {
    Finish(rp, action, Reason::kConflict, std::string());
  } else if (f.self_missing && (f.inserted || f.set)) {
    Finish(rp, action, Reason::kAbsent, f.self_defect);
  } else if (f.set) {
    Finish(rp, action, Reason::kPresent, std::string());
  } else if (f.inserted) {
    Finish(rp, action, Reason::kAbsent,
           f.had_any ? std::string() : f.absent_defect);
  } else {
    Finish(rp, action, Reason::kDuplicate, std::string());
  }
}

// Appends "item (reason)" to the decision's list of removed things.
void NoteRemoved(RulePlan* rp, std::string_view item, std::string_view reason) {
  if (!rp->decision.removed.empty()) rp->decision.removed += "; ";
  absl::StrAppend(&rp->decision.removed, item, " (", reason, ")");
}

// ---------------------------------------------------------------------------
// hreflang
// ---------------------------------------------------------------------------

enum class HreflangStatus { kOk, kNoValue, kInvalid, kNoSelf };

struct HreflangTarget {
  HreflangStatus status = HreflangStatus::kNoValue;
  std::vector<std::pair<std::string, std::string>> entries;  // by code
  std::string page_norm;
};

HreflangTarget ComputeHreflangTarget(const Pack& pack, const Rule& rule,
                                     const RuleHit& hit, const PageUrl& url,
                                     const std::string& page_url) {
  HreflangTarget t;
  t.page_norm = NormUrl(page_url).value_or(std::string());
  if (rule.value.source == ValueSource::kTable) {
    const HreflangCluster* cluster = FindCluster(pack, page_url);
    if (cluster == nullptr) return t;  // kNoValue
    t.entries = cluster->entries;
  } else if (rule.value.source == ValueSource::kPattern) {
    ExpandContext ex;
    ex.url = url;
    ex.captures = hit.captures;
    for (const auto& [code, tpl] : rule.value.pattern) {
      auto s = ExpandTemplate(tpl, ex, EscapeContext::kNone, kMaxUrlBytes);
      if (!s.ok() || !CheckUrlValue(*s).ok() || !NormUrl(*s).has_value()) {
        t.status = HreflangStatus::kInvalid;
        t.entries.clear();
        return t;
      }
      t.entries.emplace_back(code, *std::move(s));
    }
  } else {
    return t;  // kNoValue
  }
  bool self = false;
  for (const auto& e : t.entries) {
    if (NormUrl(e.second) == t.page_norm) self = true;
  }
  t.status = self ? HreflangStatus::kOk : HreflangStatus::kNoSelf;
  return t;
}

struct HCand {
  const ElementFact* fact = nullptr;
  std::string code;       // lowercase and valid as written; empty otherwise
  std::string href_norm;  // NormUrl() of the absolute href; empty otherwise
  bool blank = false;     // no href at all
  bool usable = false;    // in <head>, valid code, absolute href
  std::string key() const { return absl::StrCat(code, "=", href_norm); }
};

std::vector<HCand> MakeHreflangCands(const std::vector<ElementFact>& facts) {
  std::vector<HCand> out;
  out.reserve(facts.size());
  for (const ElementFact& f : facts) {
    HCand c;
    c.fact = &f;
    const std::string lower = absl::AsciiStrToLower(CollapseWhitespace(f.code));
    // The code must be valid as the page wrote it: "en_US" is not.
    std::optional<std::string> code = NormalizeHreflangCode(lower);
    if (code.has_value() && *code == lower) c.code = lower;
    const std::string href = CollapseWhitespace(f.value);
    c.blank = href.empty();
    if (!c.blank) c.href_norm = NormUrl(href).value_or(std::string());
    c.usable = f.in_head && !c.code.empty() && !c.href_norm.empty();
    out.push_back(std::move(c));
  }
  return out;
}

Reason HreflangUnusableReason(const HCand& c) {
  if (c.blank) return Reason::kEmpty;
  if (c.code.empty() || c.href_norm.empty()) return Reason::kUnusable;
  return Reason::kOutOfHead;
}

// What the page had, for the "before" hash (never the text itself).
std::string HreflangBeforeKey(const HCand& c) {
  if (c.usable) return c.key();
  return absl::StrCat(CollapseWhitespace(c.fact->code), "=",
                      CollapseWhitespace(c.fact->value));
}

PlanOp HreflangRemove(const HCand& c) {
  PlanOp op;
  op.type = OpType::kRemove;
  op.kind = Kind::kHreflang;
  op.target = *c.fact;
  return op;
}

PlanOp HreflangInsert(const std::string& code, const std::string& href) {
  PlanOp op;
  op.type = OpType::kInsert;
  op.kind = Kind::kHreflang;
  op.code = code;
  op.value = href;
  return op;
}

PlanOp HreflangSet(const HCand& c, const std::string& href) {
  PlanOp op;
  op.type = OpType::kSetValue;
  op.kind = Kind::kHreflang;
  op.target = *c.fact;
  op.code = c.code;
  op.value = href;
  return op;
}

std::string HreflangAfter(const RulePlan& rp) {
  std::string out;
  for (const PlanOp& op : rp.ops) {
    if (op.type == OpType::kRemove) continue;
    if (!out.empty()) out += ", ";
    absl::StrAppend(&out, op.code, "=", op.value);
  }
  return TruncateUtf8(out, kMaxAfterBytes);
}

// True when the usable elements are exactly the target entries.
bool HreflangEqualsTarget(const HreflangTarget& t,
                          const std::map<std::string, const HCand*>& first) {
  if (first.size() != t.entries.size()) return false;
  for (const auto& e : t.entries) {
    auto it = first.find(e.first);
    if (it == first.end() ||
        it->second->href_norm != NormUrl(e.second).value_or(std::string())) {
      return false;
    }
  }
  return true;
}

void PlanHreflang(const HreflangTarget& t, OnPresent on_present,
                  const std::vector<HCand>& cands, RulePlan* rp) {
  Flags f;
  f.had_any = !cands.empty();
  f.self_defect = "hreflang-no-self";
  f.unusable_defect = "hreflang-invalid";

  // The element that survives per code: the first usable one in document
  // order, except that one pointing at the page itself wins, so that a
  // conflict never costs the page its own entry.
  std::map<std::string, const HCand*> first;
  for (const HCand& c : cands) {
    if (!c.usable) continue;
    auto [it, inserted] = first.emplace(c.code, &c);
    if (!inserted && it->second->href_norm != t.page_norm &&
        c.href_norm == t.page_norm) {
      it->second = &c;
    }
  }
  bool existing_self = false;
  for (const auto& [code, c] : first) {
    if (c->href_norm == t.page_norm) existing_self = true;
  }
  const bool self_missing = !cands.empty() && !existing_self;
  auto note_before = [&](const HCand& c) {
    if (rp->decision.before_hash.empty()) {
      rp->decision.before_hash = HashValue(HreflangBeforeKey(c));
    }
  };

  if (on_present == OnPresent::kKeep) {
    if (cands.empty()) {
      for (const auto& e : t.entries) {
        rp->ops.push_back(HreflangInsert(e.first, e.second));
      }
      f.inserted = true;
      Summarize(rp, f);
      return;
    }
    // Exact duplicates (same code and same href) are the only thing keep
    // removes; everything else is observed.
    std::set<std::string> seen;
    bool invalid = false;
    bool conflict = false;
    for (const HCand& c : cands) {
      if (!c.usable) {
        invalid = true;
        continue;
      }
      if (!seen.insert(c.key()).second) {
        rp->ops.push_back(HreflangRemove(c));
        NoteRemoved(rp, HreflangBeforeKey(c), ReasonName(Reason::kDuplicate));
        note_before(c);
        f.removed = f.duplicate = true;
      } else if (first[c.code] != &c) {
        conflict = true;
      }
    }
    const std::string observed =
        invalid ? f.unusable_defect
                : (self_missing ? f.self_defect : std::string());
    if (f.removed) {
      Summarize(rp, f);
    } else if (conflict) {
      Finish(rp, Action::kNone, Reason::kConflict, observed);
    } else {
      f.idle_defect = observed;
      const bool equal =
          !invalid && !self_missing && HreflangEqualsTarget(t, first);
      f.idle_reason = equal ? Reason::kEqual : Reason::kPresent;
      Summarize(rp, f);
    }
    return;
  }

  if (on_present == OnPresent::kRepair) {
    for (const HCand& c : cands) {
      Reason why;
      if (!c.usable) {
        why = HreflangUnusableReason(c);
        if (!f.has_unusable) {
          f.has_unusable = true;
          f.unusable_reason = why;
        }
      } else if (first[c.code] != &c) {
        if (first[c.code]->href_norm != c.href_norm) {
          f.conflict = true;
          why = Reason::kConflict;
        } else {
          f.duplicate = true;
          why = Reason::kDuplicate;
        }
      } else {
        continue;
      }
      rp->ops.push_back(HreflangRemove(c));
      NoteRemoved(rp, HreflangBeforeKey(c), ReasonName(why));
      note_before(c);
      f.removed = true;
    }
    bool self_inserted = false;
    for (const auto& e : t.entries) {
      if (first.count(e.first) != 0) continue;
      rp->ops.push_back(HreflangInsert(e.first, e.second));
      f.inserted = true;
      if (NormUrl(e.second) == t.page_norm) self_inserted = true;
    }
    if (self_missing && !self_inserted) {
      // Every self code exists but points elsewhere: point the first at the
      // page.
      for (const auto& e : t.entries) {
        if (NormUrl(e.second) != t.page_norm) continue;
        auto it = first.find(e.first);
        if (it == first.end()) continue;
        rp->ops.push_back(HreflangSet(*it->second, e.second));
        note_before(*it->second);
        f.set = true;
        break;
      }
    }
    f.self_missing = self_missing;
    f.idle_reason =
        HreflangEqualsTarget(t, first) ? Reason::kEqual : Reason::kPresent;
    Summarize(rp, f);
    return;
  }

  // replace: the document ends with exactly the target entries.
  std::set<const HCand*> kept;
  for (const auto& e : t.entries) {
    const HCand* match = nullptr;
    const HCand* reuse = nullptr;
    const std::string want = NormUrl(e.second).value_or(std::string());
    for (const HCand& c : cands) {
      if (!c.usable || c.code != e.first || kept.count(&c) != 0) continue;
      if (c.href_norm == want) {
        match = &c;
        break;
      }
      if (reuse == nullptr) reuse = &c;
    }
    if (match != nullptr) {
      kept.insert(match);
    } else if (reuse != nullptr) {
      kept.insert(reuse);
      rp->ops.push_back(HreflangSet(*reuse, e.second));
      note_before(*reuse);
      f.set = true;
    } else {
      rp->ops.push_back(HreflangInsert(e.first, e.second));
      f.inserted = true;
    }
  }
  for (const HCand& c : cands) {
    if (kept.count(&c) != 0) continue;
    rp->ops.push_back(HreflangRemove(c));
    note_before(c);
    f.removed = true;
    Reason why;
    if (!c.usable) {
      why = HreflangUnusableReason(c);
      if (!f.has_unusable) {
        f.has_unusable = true;
        f.unusable_reason = why;
      }
    } else if (first[c.code] == &c) {
      f.conflict = true;  // a distinct code the target does not carry
      why = Reason::kConflict;
    } else {
      f.duplicate = true;
      why = Reason::kDuplicate;
    }
    NoteRemoved(rp, HreflangBeforeKey(c), ReasonName(why));
  }
  f.self_missing = self_missing;
  f.idle_reason = Reason::kEqual;
  Summarize(rp, f);
}

// ---------------------------------------------------------------------------
// jsonld
// ---------------------------------------------------------------------------

using Json = nlohmann::json;

void CollectNodeTypes(const Json& node, bool allow_graph,
                      std::set<std::string>* out) {
  if (!node.is_object()) return;
  auto t = node.find("@type");
  if (t != node.end()) {
    if (t->is_string()) {
      out->insert(t->get<std::string>());
    } else if (t->is_array()) {
      for (const Json& v : *t) {
        if (v.is_string()) out->insert(v.get<std::string>());
      }
    }
  }
  if (allow_graph) {
    auto g = node.find("@graph");
    if (g != node.end() && g->is_array()) {
      for (const Json& m : *g) CollectNodeTypes(m, false, out);
    }
  }
}

// The set of top-level @type values, including the members of a top-level
// @graph.
std::set<std::string> TypesOf(const Json& doc) {
  std::set<std::string> out;
  if (doc.is_array()) {
    for (const Json& n : doc) CollectNodeTypes(n, true, &out);
  } else {
    CollectNodeTypes(doc, true, &out);
  }
  return out;
}

struct JCand {
  const ElementFact* fact = nullptr;
  bool valid = false;
  // Too deeply nested to parse safely: neither valid nor invalid, and never
  // touched.
  bool unreadable = false;
  std::set<std::string> types;
  std::string dump;  // canonical form of a valid block
  std::string hash_key;
};

bool Intersects(const std::set<std::string>& a,
                const std::set<std::string>& b) {
  for (const std::string& x : a) {
    if (b.count(x) != 0) return true;
  }
  return false;
}

JCand MakeJsonCand(const ElementFact& f) {
  JCand c;
  c.fact = &f;
  if (f.unreadable || JsonNestingExceeds(f.value, kMaxJsonLdDepth)) {
    c.unreadable = true;
    return c;
  }
  Json doc = Json::parse(f.value, nullptr, false);
  if (!doc.is_discarded() && (doc.is_object() || doc.is_array())) {
    c.valid = true;
    c.types = TypesOf(doc);
    c.dump = doc.dump();
    c.hash_key = c.dump;
  } else {
    c.hash_key = CollapseWhitespace(f.value);
  }
  return c;
}

struct JsonTarget {
  TargetStatus status = TargetStatus::kNoValue;
  std::string text;  // what gets written
  std::set<std::string> types;
  std::string dump;
};

JsonTarget ComputeJsonTarget(const Rule& rule, const RuleHit& hit,
                             const PageUrl& url, const std::string& title,
                             const std::string& description,
                             const std::string& canonical) {
  JsonTarget t;
  ExpandContext ex;
  ex.url = url;
  ex.captures = hit.captures;
  ex.title = title;
  ex.description = description;
  ex.canonical = canonical;
  auto s = ExpandTemplate(rule.value.tpl, ex, EscapeContext::kJsonString,
                          kMaxJsonLdBytes);
  if (!s.ok() || JsonNestingExceeds(*s, kMaxJsonLdDepth)) {
    t.status = TargetStatus::kInvalid;
    return t;
  }
  Json doc = Json::parse(*s, nullptr, false);
  if (doc.is_discarded() || !(doc.is_object() || doc.is_array())) {
    t.status = TargetStatus::kInvalid;
    return t;
  }
  t.types = TypesOf(doc);
  if (t.types.empty()) {
    t.status = TargetStatus::kInvalid;
    return t;
  }
  t.dump = doc.dump();
  t.text = *std::move(s);
  t.status = TargetStatus::kOk;
  return t;
}

PlanOp JsonRemove(const JCand& c) {
  PlanOp op;
  op.type = OpType::kRemove;
  op.kind = Kind::kJsonLd;
  op.target = *c.fact;
  return op;
}

PlanOp JsonSet(const JCand& c, const std::string& text) {
  PlanOp op;
  op.type = OpType::kSetValue;
  op.kind = Kind::kJsonLd;
  op.target = *c.fact;
  op.value = text;
  return op;
}

PlanOp JsonInsert(const std::string& text) {
  PlanOp op;
  op.type = OpType::kInsert;
  op.kind = Kind::kJsonLd;
  op.value = text;
  return op;
}

Reason JsonUnusableReason(const JCand& c) {
  return CollapseWhitespace(c.fact->value).empty() ? Reason::kEmpty
                                                   : Reason::kUnusable;
}

std::string TypesText(const JCand& c) {
  if (!c.valid) return "unparsable block";
  std::string out;
  for (const std::string& ty : c.types) {
    if (!out.empty()) out += ",";
    out += ty;
  }
  return out.empty() ? "untyped block" : out;
}

void PlanJsonLd(const JsonTarget& t, OnPresent on_present,
                const std::vector<JCand>& cands, RulePlan* rp) {
  Flags f;
  f.had_any = !cands.empty();
  f.unusable_defect = "jsonld-invalid";
  auto matching = [&](const JCand& c) {
    return c.valid && Intersects(c.types, t.types);
  };
  auto note_before = [&](const JCand& c) {
    if (rp->decision.before_hash.empty()) {
      rp->decision.before_hash = HashValue(c.hash_key);
    }
  };

  bool any_matching = false;
  bool any_invalid = false;
  const JCand* first_matching = nullptr;
  const JCand* equal_block = nullptr;
  const JCand* first_invalid = nullptr;
  for (const JCand& c : cands) {
    if (!c.valid) {
      any_invalid = true;
      if (first_invalid == nullptr) first_invalid = &c;
    } else if (matching(c)) {
      any_matching = true;
      if (first_matching == nullptr) first_matching = &c;
      if (equal_block == nullptr && c.dump == t.dump) equal_block = &c;
    }
  }

  // Exact duplicates among the blocks that carry the rule's types.
  std::set<std::string> seen;
  std::set<const JCand*> duplicates;
  for (const JCand& c : cands) {
    if (matching(c) && !seen.insert(c.dump).second) duplicates.insert(&c);
  }

  if (on_present == OnPresent::kKeep) {
    if (!any_matching && !any_invalid) {
      rp->ops.push_back(JsonInsert(t.text));
      f.inserted = true;
      Summarize(rp, f);
      return;
    }
    for (const JCand* d : duplicates) {
      rp->ops.push_back(JsonRemove(*d));
      NoteRemoved(rp, TypesText(*d), ReasonName(Reason::kDuplicate));
      note_before(*d);
      f.removed = f.duplicate = true;
    }
    f.idle_defect = any_invalid ? f.unusable_defect : std::string();
    f.idle_reason = equal_block != nullptr ? Reason::kEqual : Reason::kPresent;
    Summarize(rp, f);
    return;
  }

  if (on_present == OnPresent::kRepair) {
    for (const JCand& c : cands) {
      if (!c.valid) {
        rp->ops.push_back(JsonRemove(c));
        NoteRemoved(rp, TypesText(c), ReasonName(JsonUnusableReason(c)));
        note_before(c);
        f.removed = true;
        if (!f.has_unusable) {
          f.has_unusable = true;
          f.unusable_reason = JsonUnusableReason(c);
        }
      } else if (duplicates.count(&c) != 0) {
        rp->ops.push_back(JsonRemove(c));
        NoteRemoved(rp, TypesText(c), ReasonName(Reason::kDuplicate));
        note_before(c);
        f.removed = f.duplicate = true;
      }
    }
    if (!any_matching) {
      rp->ops.push_back(JsonInsert(t.text));
      f.inserted = true;
    }
    f.idle_reason = equal_block != nullptr ? Reason::kEqual : Reason::kPresent;
    Summarize(rp, f);
    return;
  }

  // replace: exactly one block with the rule's content. Only blocks whose
  // types are all among the rule's are the rule's to rewrite or remove. A
  // block that shares a type but carries others (a @graph with a WebPage and
  // an Organization, a ["Organization", "LocalBusiness"] block) would lose
  // data, so the rule stands down for the page.
  auto subset = [&](const JCand& c) {
    if (!c.valid || c.types.empty()) return false;
    for (const std::string& ty : c.types) {
      if (t.types.count(ty) == 0) return false;
    }
    return true;
  };
  for (const JCand& c : cands) {
    if (matching(c) && !subset(c)) {
      Finish(rp, Action::kNone, Reason::kConflict, std::string());
      return;
    }
  }
  const JCand* first_owned = nullptr;
  const JCand* equal_owned = nullptr;
  std::set<std::string> seen_owned;
  std::set<const JCand*> dup_owned;
  for (const JCand& c : cands) {
    if (!subset(c)) continue;
    if (first_owned == nullptr) first_owned = &c;
    if (equal_owned == nullptr && c.dump == t.dump) equal_owned = &c;
    if (!seen_owned.insert(c.dump).second) dup_owned.insert(&c);
  }
  const JCand* kept = equal_owned;
  const JCand* reuse = nullptr;
  if (kept == nullptr) {
    reuse = first_owned != nullptr ? first_owned : first_invalid;
  }
  if (reuse != nullptr) {
    rp->ops.push_back(JsonSet(*reuse, t.text));
    note_before(*reuse);
    f.set = true;
  }
  for (const JCand& c : cands) {
    if (&c == kept || &c == reuse) continue;
    if (c.valid && !subset(c)) continue;
    rp->ops.push_back(JsonRemove(c));
    note_before(c);
    f.removed = true;
    Reason why;
    if (!c.valid) {
      why = JsonUnusableReason(c);
      if (!f.has_unusable) {
        f.has_unusable = true;
        f.unusable_reason = why;
      }
    } else if (c.dump == t.dump || dup_owned.count(&c) != 0) {
      f.duplicate = true;
      why = Reason::kDuplicate;
    } else {
      f.conflict = true;
      why = Reason::kConflict;
    }
    NoteRemoved(rp, TypesText(c), ReasonName(why));
  }
  if (kept == nullptr && reuse == nullptr) {
    rp->ops.push_back(JsonInsert(t.text));
    f.inserted = true;
  }
  f.idle_reason = Reason::kEqual;
  Summarize(rp, f);
}

std::string JsonAfter(const JsonTarget& t) {
  std::string out;
  for (const std::string& ty : t.types) {
    if (!out.empty()) out += ", ";
    out += ty;
  }
  return TruncateUtf8(out, kMaxAfterBytes);
}

// ---------------------------------------------------------------------------
// The document's final head values, for jsonld placeholders
// ---------------------------------------------------------------------------

// The value `kind` ends with: the one the rule's plan writes when `rp` is
// given, otherwise the first usable element that survives the plan's removals.
std::string FinalValue(Kind kind, const std::vector<ElementFact>& facts,
                       const RulePlan* rp, const std::string& base) {
  std::set<const net_instaweb::HtmlElement*> gone;
  if (rp != nullptr) {
    for (const PlanOp& op : rp->ops) {
      if (op.type == OpType::kInsert || op.type == OpType::kSetValue) {
        return CollapseWhitespace(LogicalText(kind, op.value));
      }
      gone.insert(op.target.element);
    }
  }
  for (const ElementFact& f : facts) {
    if (!f.in_head || gone.count(f.element) != 0) continue;
    std::string v = CollapseWhitespace(f.value);
    if (v.empty()) continue;
    if (kind == Kind::kCanonical) {
      std::string abs = ResolveUrl(base, v);
      if (!NormUrl(abs).has_value()) continue;
      return abs;
    }
    return v;
  }
  return std::string();
}

// Bytes the plan adds to the page, counted after escaping (a title's value is
// already escaped).
size_t AddedBytes(const RulePlan& rp, size_t extra) {
  size_t n = 0;
  for (const PlanOp& op : rp.ops) {
    if (op.type != OpType::kInsert && op.type != OpType::kSetValue) continue;
    size_t value;
    switch (op.kind) {
      case Kind::kTitle:
      case Kind::kJsonLd:
        value = op.value.size();
        break;
      case Kind::kHreflang:
        value = EscapeAttributeValue(op.value).size() +
                EscapeAttributeValue(op.code).size() + 32;
        break;
      default:
        value = EscapeAttributeValue(op.value).size();
    }
    n += value + extra + (op.type == OpType::kInsert ? 64 : 0);
  }
  return n;
}

}  // namespace

Plan BuildPlan(const Pack& pack, const PageContext& ctx, const PageFacts& facts,
               size_t max_added_bytes, size_t extra_bytes_per_change) {
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
  for (Kind kind : kKindOrder) {
    if (sel.by_kind[static_cast<int>(kind)].has_value()) any = true;
  }
  if (!any) return plan;
  if (facts.malformed_head) {
    plan.skip = SkipReason::kMalformedHead;
    return plan;
  }
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

  // The head values the page ends with: `planned` as if every earlier rule
  // had been applied, `actual` with only the rules that really are enforcing.
  // A jsonld rule in report mode answers "what would the whole pack do", an
  // enforcing one must not quote a value that is not in the document.
  struct Finals {
    std::string canonical, title, description;
  };
  Finals planned, actual;
  auto record = [&](Kind kind, const RulePlan* rp) {
    const bool enforced = rp != nullptr && rp->decision.mode == Mode::kEnforce;
    const std::string pv = FinalValue(kind, facts.For(kind), rp, base);
    const std::string av =
        enforced ? pv : FinalValue(kind, facts.For(kind), nullptr, base);
    Finals* both[2] = {&planned, &actual};
    const std::string* vals[2] = {&pv, &av};
    for (int i = 0; i < 2; ++i) {
      switch (kind) {
        case Kind::kCanonical:
          both[i]->canonical = *vals[i];
          break;
        case Kind::kTitle:
          both[i]->title = *vals[i];
          break;
        default:
          both[i]->description = *vals[i];
      }
    }
  };

  for (Kind kind : kKindOrder) {
    const auto& hit = sel.by_kind[static_cast<int>(kind)];
    const bool head_kind = kind == Kind::kCanonical || kind == Kind::kTitle ||
                           kind == Kind::kDescription;
    if (!hit.has_value()) {
      if (head_kind) record(kind, nullptr);
      continue;
    }
    const Rule& rule = *hit->rule;

    RulePlan rp;
    rp.rule = &rule;
    rp.decision.rule_id = rule.id;
    rp.decision.kind = kind;
    rp.decision.mode =
        EffectiveMode(ctx.global_mode, sel.site->mode, rule.enforce);

    if (head_kind) {
      Target target = ComputeTarget(pack, rule, *hit, ctx.url);
      if (target.status != TargetStatus::kOk) {
        Finish(&rp, Action::kNone,
               target.status == TargetStatus::kNoValue ? Reason::kNoValue
                                                       : Reason::kInvalidValue,
               std::string());
        record(kind, nullptr);
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
          rp.decision.after = TruncateUtf8(target.logical, kMaxAfterBytes);
        }
      }
      record(kind, &rp);
    } else if (kind == Kind::kHreflang) {
      HreflangTarget t =
          ComputeHreflangTarget(pack, rule, *hit, ctx.url, page_url);
      if (t.status != HreflangStatus::kOk) {
        Reason why = Reason::kInvalidValue;
        if (t.status == HreflangStatus::kNoValue) why = Reason::kNoValue;
        if (t.status == HreflangStatus::kNoSelf) {
          why = Reason::kClusterWithoutSelf;
        }
        Finish(&rp, Action::kNone, why, std::string());
        plan.rules.push_back(std::move(rp));
        continue;
      }
      const std::vector<HCand> cands = MakeHreflangCands(facts.hreflangs);
      PlanHreflang(t, rule.on_present, cands, &rp);
      rp.decision.after = HreflangAfter(rp);
    } else {
      // Blocks too large or too deeply nested to read: leave the page's
      // JSON-LD alone.
      std::vector<JCand> cands;
      cands.reserve(facts.jsonlds.size());
      bool unreadable = false;
      for (const ElementFact& f : facts.jsonlds) {
        cands.push_back(MakeJsonCand(f));
        if (cands.back().unreadable) unreadable = true;
      }
      const Finals& fin =
          rp.decision.mode == Mode::kEnforce ? actual : planned;
      JsonTarget t = ComputeJsonTarget(rule, *hit, ctx.url, fin.title,
                                       fin.description, fin.canonical);
      if (t.status != TargetStatus::kOk) {
        Finish(&rp, Action::kNone, Reason::kInvalidValue, std::string());
      } else if (unreadable) {
        Finish(&rp, Action::kNone, Reason::kPresent, std::string());
      } else {
        PlanJsonLd(t, rule.on_present, cands, &rp);
        for (const PlanOp& op : rp.ops) {
          if (op.type == OpType::kInsert || op.type == OpType::kSetValue) {
            rp.decision.after = JsonAfter(t);
          }
        }
      }
    }
    plan.added_bytes += AddedBytes(rp, extra_bytes_per_change);
    plan.rules.push_back(std::move(rp));
  }

  if (plan.added_bytes > max_added_bytes) {
    plan.skip = SkipReason::kSizeLimit;
    plan.rules.clear();
  }
  return plan;
}

}  // namespace pagespeed::packs
