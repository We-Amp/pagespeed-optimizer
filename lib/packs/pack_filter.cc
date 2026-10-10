// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/pack_filter.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "lib/html/html_name.h"
#include "lib/html/safe_entity_decode.h"
#include "lib/packs/matcher.h"

namespace pagespeed::packs {

using net_instaweb::HtmlCharactersNode;
using net_instaweb::HtmlElement;
using net_instaweb::HtmlName;

std::optional<PageUrl> ParsePageUrl(std::string_view url) {
  PageUrl out;
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return std::nullopt;
  out.scheme = absl::AsciiStrToLower(url.substr(0, scheme_end));
  if (out.scheme != "http" && out.scheme != "https") return std::nullopt;
  url.remove_prefix(scheme_end + 3);
  const size_t frag = url.find('#');
  if (frag != std::string_view::npos) url = url.substr(0, frag);
  const size_t auth_end = url.find_first_of("/?");
  const std::string_view authority = url.substr(0, auth_end);
  std::string_view rest = auth_end == std::string_view::npos
                              ? std::string_view()
                              : url.substr(auth_end);
  if (authority.find('@') != std::string_view::npos) return std::nullopt;
  out.host = NormalizeHost(authority);
  if (out.host.empty()) return std::nullopt;
  const size_t q = rest.find('?');
  std::string_view path = rest.substr(0, q);
  if (q != std::string_view::npos) out.query = std::string(rest.substr(q + 1));
  out.path = path.empty() ? "/" : std::string(path);
  return out;
}

namespace {

bool IsInertTag(std::string_view lower_name) {
  return lower_name == "svg" || lower_name == "math" ||
         lower_name == "template" || lower_name == "noscript" ||
         lower_name == "noembed" || lower_name == "noframes";
}

}  // namespace

PackFilter::PackFilter(net_instaweb::HtmlParse* parser,
                       std::shared_ptr<const Pack> pack, PageUrl url,
                       Mode global_mode, PackFilterOptions options)
    : parser_(parser),
      pack_(std::move(pack)),
      url_(std::move(url)),
      global_mode_(global_mode),
      options_(options) {
  if (global_mode_ == Mode::kEnforce && pack_ != nullptr) {
    for (const Rule& r : pack_->rules) {
      if (r.enabled && r.enforce && r.kind == Kind::kCanonical &&
          r.on_present != OnPresent::kKeep) {
        can_modify_urls_ = true;
      }
    }
  }
}

void PackFilter::StartDocument() {
  facts_ = PageFacts();
  inert_.clear();
  open_head_ = nullptr;
  open_title_ = nullptr;
  open_title_raw_.clear();
  modified_ = false;
  would_modify_ = false;
  skip_reason_ = SkipReason::kNone;
  decisions_.clear();
}

void PackFilter::StartElement(HtmlElement* element) {
  const std::string name = absl::AsciiStrToLower(element->name_str());
  if (IsInertTag(name)) {
    inert_.push_back(element);
    return;
  }
  if (!inert_.empty()) return;

  const HtmlName::Keyword kw = element->keyword();
  const bool in_head = open_head_ != nullptr;
  if (kw == HtmlName::kHead) {
    if (facts_.head == nullptr) {
      facts_.head = element;
      open_head_ = element;
    }
  } else if (kw == HtmlName::kBase) {
    if (facts_.base_href.empty()) {
      const char* href = element->AttributeValue(HtmlName::kHref);
      if (href != nullptr) facts_.base_href = href;
    }
  } else if (kw == HtmlName::kLink) {
    const char* rel = element->AttributeValue(HtmlName::kRel);
    if (rel == nullptr) return;
    bool canonical = false;
    std::vector<std::string> others;
    for (std::string_view tok :
         absl::StrSplit(rel, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty())) {
      if (absl::EqualsIgnoreCase(tok, "canonical")) {
        canonical = true;
      } else {
        others.emplace_back(tok);
      }
    }
    if (!canonical) return;
    ElementFact f;
    f.element = element;
    f.in_head = in_head;
    const char* href = element->AttributeValue(HtmlName::kHref);
    f.has_value_attr = href != nullptr;
    if (href != nullptr) f.value = href;
    f.other_rel_tokens = std::move(others);
    facts_.canonicals.push_back(std::move(f));
  } else if (kw == HtmlName::kTitle) {
    ElementFact f;
    f.element = element;
    f.in_head = in_head;
    facts_.titles.push_back(std::move(f));
    open_title_ = &facts_.titles.back();
    open_title_raw_.clear();
  } else if (kw == HtmlName::kMeta) {
    const char* nm = element->AttributeValue(HtmlName::kName);
    if (nm == nullptr || !absl::EqualsIgnoreCase(nm, "description")) return;
    ElementFact f;
    f.element = element;
    f.in_head = in_head;
    const char* content = element->AttributeValue(HtmlName::kContent);
    f.has_value_attr = content != nullptr;
    if (content != nullptr) f.value = content;
    facts_.descriptions.push_back(std::move(f));
  }
}

void PackFilter::EndElement(HtmlElement* element) {
  if (!inert_.empty()) {
    if (inert_.back() == element) inert_.pop_back();
    return;
  }
  if (element == open_head_) {
    open_head_ = nullptr;
  } else if (open_title_ != nullptr && open_title_->element == element) {
    open_title_->value = net_instaweb::SafeDecodeHtmlEntities(open_title_raw_);
    open_title_ = nullptr;
  }
}

void PackFilter::Characters(HtmlCharactersNode* characters) {
  if (open_title_ != nullptr && inert_.empty() &&
      characters->parent() == open_title_->element) {
    open_title_raw_.append(characters->contents());
    open_title_->text_nodes.push_back(characters);
  }
}

void PackFilter::EndDocument() {
  if (pack_ == nullptr) return;
  PageContext ctx;
  ctx.url = url_;
  ctx.global_mode = global_mode_;
  Plan plan = BuildPlan(*pack_, ctx, facts_, options_.max_added_bytes);
  skip_reason_ = plan.skip;
  if (plan.skip != SkipReason::kNone) return;

  for (const RulePlan& rp : plan.rules) {
    if (!rp.ops.empty()) would_modify_ = true;
  }
  if (!Rewritable(plan)) {
    skip_reason_ = SkipReason::kNotRewritable;
    would_modify_ = false;
    return;
  }
  for (const RulePlan& rp : plan.rules) decisions_.push_back(rp.decision);
  Apply(plan);
}

bool PackFilter::Rewritable(const Plan& plan) const {
  for (const RulePlan& rp : plan.rules) {
    if (rp.decision.mode != Mode::kEnforce) continue;
    for (const PlanOp& op : rp.ops) {
      if (op.type == OpType::kInsert) {
        if (!parser_->IsRewritable(facts_.head)) return false;
        continue;
      }
      if (!parser_->IsRewritable(op.target.element)) return false;
      for (const HtmlCharactersNode* t : op.target.text_nodes) {
        if (!parser_->IsRewritable(t)) return false;
      }
    }
  }
  return true;
}

void PackFilter::Apply(const Plan& plan) {
  for (const RulePlan& rp : plan.rules) {
    if (rp.decision.mode != Mode::kEnforce) continue;
    for (const PlanOp& op : rp.ops) ApplyOp(*rp.rule, rp.decision, op);
  }
}

void PackFilter::AddDebugComment(const Rule& rule, const PackDecision& decision,
                                 HtmlElement* before, HtmlElement* head) {
  if (!options_.debug_comments) return;
  HtmlElement* parent = before != nullptr ? before->parent() : head;
  auto* comment = parser_->NewCommentNode(
      parent, absl::StrCat(" pagespeed-pack ", pack_->info.id, "@",
                           pack_->info.version, " rule=", rule.id,
                           " action=", ActionName(decision.action), " "));
  if (before != nullptr) {
    parser_->InsertNodeBeforeNode(before, comment);
  } else {
    parser_->AppendChild(head, comment);
  }
}

void PackFilter::ApplyOp(const Rule& rule, const PackDecision& decision,
                         const PlanOp& op) {
  HtmlElement* el = op.target.element;
  switch (op.type) {
    case OpType::kRemove:
      if (parser_->DeleteNode(el)) modified_ = true;
      return;
    case OpType::kRemoveCanonicalToken: {
      auto* rel = el->FindAttribute(HtmlName::kRel);
      if (rel != nullptr) {
        rel->SetValue(absl::StrJoin(op.target.other_rel_tokens, " "));
        modified_ = true;
      }
      return;
    }
    case OpType::kSetValue: {
      AddDebugComment(rule, decision, el, nullptr);
      if (op.kind == Kind::kTitle) {
        for (HtmlCharactersNode* t : op.target.text_nodes) {
          parser_->DeleteNode(t);
        }
        auto* text = parser_->NewCharactersNode(el, op.value);
        parser_->AppendChild(el, text);
      } else {
        const HtmlName::Keyword attr =
            op.kind == Kind::kCanonical ? HtmlName::kHref : HtmlName::kContent;
        auto* a = el->FindAttribute(attr);
        if (a != nullptr) {
          a->SetValue(op.value);
        } else {
          parser_->AddAttribute(el, attr, op.value);
        }
      }
      modified_ = true;
      return;
    }
    case OpType::kInsert: {
      HtmlElement* head = facts_.head;
      HtmlElement* fresh = nullptr;
      switch (op.kind) {
        case Kind::kCanonical:
          fresh = parser_->NewElement(head, HtmlName::kLink);
          parser_->AddAttribute(fresh, HtmlName::kRel, "canonical");
          parser_->AddAttribute(fresh, HtmlName::kHref, op.value);
          break;
        case Kind::kTitle:
          fresh = parser_->NewElement(head, HtmlName::kTitle);
          break;
        default:
          fresh = parser_->NewElement(head, HtmlName::kMeta);
          parser_->AddAttribute(fresh, HtmlName::kName, "description");
          parser_->AddAttribute(fresh, HtmlName::kContent, op.value);
          break;
      }
      AddDebugComment(rule, decision, nullptr, head);
      parser_->AppendChild(head, fresh);
      if (op.kind == Kind::kTitle) {
        parser_->AppendChild(fresh,
                             parser_->NewCharactersNode(fresh, op.value));
      }
      modified_ = true;
      return;
    }
  }
}

}  // namespace pagespeed::packs
