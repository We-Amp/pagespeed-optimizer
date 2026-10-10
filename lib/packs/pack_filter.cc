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
#include "lib/packs/planner.h"

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

// Decoded value of an attribute, tolerant of non-ASCII bytes and entities:
// the kernel's own decoded value is empty for those, so the escaped value is
// read and decoded here. `present` is false when the attribute is absent or
// has no value.
std::string AttrValue(const HtmlElement* element, HtmlName::Keyword kw,
                      bool* present = nullptr) {
  const char* escaped = element->EscapedAttributeValue(kw);
  if (present != nullptr) *present = escaped != nullptr;
  if (escaped == nullptr) return std::string();
  return net_instaweb::SafeDecodeHtmlEntities(escaped);
}

// Elements that may appear in <head>. The first other element (usually
// <body>) ends the head even when the document never writes </head>.
bool IsHeadContent(HtmlName::Keyword kw, std::string_view lower_name) {
  switch (kw) {
    case HtmlName::kTitle:
    case HtmlName::kMeta:
    case HtmlName::kLink:
    case HtmlName::kBase:
    case HtmlName::kScript:
    case HtmlName::kStyle:
    case HtmlName::kNoscript:
      return true;
    default:
      return lower_name == "template" || lower_name == "svg" ||
             lower_name == "math" || lower_name == "noembed" ||
             lower_name == "noframes";
  }
}

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
      if (!r.enabled || !r.enforce) continue;
      if ((r.kind == Kind::kCanonical || r.kind == Kind::kHreflang) &&
          r.on_present != OnPresent::kKeep) {
        can_modify_urls_ = true;
      }
      if (r.kind == Kind::kJsonLd) may_inject_scripts_ = true;
    }
  }
}

void PackFilter::StartDocument() {
  facts_ = PageFacts();
  inert_.clear();
  open_head_ = nullptr;
  open_title_ = -1;
  open_title_raw_.clear();
  open_jsonld_ = -1;
  commented_rule_ = nullptr;
  modified_ = false;
  would_modify_ = false;
  skip_reason_ = SkipReason::kNone;
  decisions_.clear();
}

void PackFilter::EndLogicalHead(HtmlElement* ender) {
  // An inert element (svg, noscript, ...) that is still open here swallowed
  // the end of the head; where the head really ends is not knowable.
  if (!inert_.empty()) facts_.malformed_head = true;
  facts_.insert_before = ender;
  open_head_ = nullptr;
}

void PackFilter::StartElement(HtmlElement* element) {
  const std::string name = absl::AsciiStrToLower(element->name_str());
  const HtmlName::Keyword kw = element->keyword();

  // The head ends at </head>, at <body>, or at the first element that cannot
  // be head content, whichever comes first.
  if (open_head_ != nullptr && element != open_head_) {
    if (kw == HtmlName::kBody || (inert_.empty() && !IsHeadContent(kw, name))) {
      EndLogicalHead(element);
    }
  }

  if (IsInertTag(name)) {
    inert_.push_back(element);
    return;
  }
  if (!inert_.empty()) return;

  const bool in_head = open_head_ != nullptr;
  if (kw == HtmlName::kHead) {
    if (facts_.head == nullptr) {
      facts_.head = element;
      open_head_ = element;
    }
  } else if (kw == HtmlName::kBase) {
    if (facts_.base_href.empty()) {
      facts_.base_href = AttrValue(element, HtmlName::kHref);
    }
  } else if (kw == HtmlName::kLink) {
    bool has_rel = false;
    const std::string rel = AttrValue(element, HtmlName::kRel, &has_rel);
    if (!has_rel) return;
    bool canonical = false;
    bool alternate = false;
    std::vector<std::string> others;
    std::vector<std::string> others_but_alternate;
    for (std::string_view tok :
         absl::StrSplit(rel, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty())) {
      if (absl::EqualsIgnoreCase(tok, "canonical")) {
        canonical = true;
      } else {
        others.emplace_back(tok);
        if (absl::EqualsIgnoreCase(tok, "alternate")) {
          alternate = true;
        } else {
          others_but_alternate.emplace_back(tok);
        }
      }
    }
    if (alternate && others_but_alternate.empty() && !canonical) {
      // An alternate link without other meanings: an hreflang entry when it
      // names a language. (A link with extra rel tokens is never touched.)
      const HtmlElement::Attribute* attr = nullptr;
      for (auto it = element->attributes().begin();
           it != element->attributes().end(); ++it) {
        if (absl::EqualsIgnoreCase(it->name_str(), "hreflang")) {
          attr = &*it;
          break;
        }
      }
      if (attr != nullptr) {
        ElementFact f;
        f.element = element;
        f.in_head = in_head;
        f.code = attr->escaped_value() == nullptr
                     ? std::string()
                     : net_instaweb::SafeDecodeHtmlEntities(
                           attr->escaped_value());
        f.value = AttrValue(element, HtmlName::kHref, &f.has_value_attr);
        facts_.hreflangs.push_back(std::move(f));
      }
    }
    if (!canonical) return;
    ElementFact f;
    f.element = element;
    f.in_head = in_head;
    f.value = AttrValue(element, HtmlName::kHref, &f.has_value_attr);
    f.other_rel_tokens = std::move(others);
    facts_.canonicals.push_back(std::move(f));
  } else if (kw == HtmlName::kTitle) {
    ElementFact f;
    f.element = element;
    f.in_head = in_head;
    facts_.titles.push_back(std::move(f));
    open_title_ = static_cast<int>(facts_.titles.size()) - 1;
    open_title_raw_.clear();
  } else if (kw == HtmlName::kScript) {
    std::string type = AttrValue(element, HtmlName::kType);
    if (absl::EqualsIgnoreCase(absl::StripAsciiWhitespace(type),
                               "application/ld+json")) {
      ElementFact f;
      f.element = element;
      f.in_head = in_head;
      facts_.jsonlds.push_back(std::move(f));
      open_jsonld_ = static_cast<int>(facts_.jsonlds.size()) - 1;
    }
  } else if (kw == HtmlName::kMeta) {
    const std::string nm = AttrValue(element, HtmlName::kName);
    if (!absl::EqualsIgnoreCase(nm, "description")) return;
    ElementFact f;
    f.element = element;
    f.in_head = in_head;
    f.value = AttrValue(element, HtmlName::kContent, &f.has_value_attr);
    facts_.descriptions.push_back(std::move(f));
  }
}

void PackFilter::EndElement(HtmlElement* element) {
  if (element == open_head_) {
    // </head>, explicit or implied by the parser at the end of the document.
    if (!inert_.empty()) facts_.malformed_head = true;
    open_head_ = nullptr;
    return;
  }
  if (!inert_.empty()) {
    if (inert_.back() == element) {
      inert_.pop_back();
      // An inert element in <head> that the source never closed was closed by
      // the parser at the end of the head; where the head really ends is not
      // knowable.
      const auto style = element->style();
      if (open_head_ != nullptr && (style == HtmlElement::AUTO_CLOSE ||
                                    style == HtmlElement::UNCLOSED)) {
        facts_.malformed_head = true;
      }
    }
    return;
  }
  if (open_jsonld_ >= 0 && facts_.jsonlds[open_jsonld_].element == element) {
    open_jsonld_ = -1;
    return;
  }
  if (open_title_ >= 0 && facts_.titles[open_title_].element == element) {
    facts_.titles[open_title_].value =
        net_instaweb::SafeDecodeHtmlEntities(open_title_raw_);
    open_title_ = -1;
  }
}

void PackFilter::Characters(HtmlCharactersNode* characters) {
  if (open_jsonld_ >= 0 && inert_.empty() &&
      characters->parent() == facts_.jsonlds[open_jsonld_].element) {
    ElementFact& f = facts_.jsonlds[open_jsonld_];
    f.text_nodes.push_back(characters);
    if (f.unreadable || f.value.size() + characters->contents().size() >
                            kMaxJsonLdScanBytes) {
      f.unreadable = true;
      f.value.clear();
    } else {
      f.value.append(characters->contents());
    }
    return;
  }
  if (open_title_ >= 0 && inert_.empty() &&
      characters->parent() == facts_.titles[open_title_].element) {
    open_title_raw_.append(characters->contents());
    facts_.titles[open_title_].text_nodes.push_back(characters);
  }
}

void PackFilter::EndDocument() {
  if (pack_ == nullptr) return;
  PageContext ctx;
  ctx.url = url_;
  ctx.global_mode = global_mode_;
  // Room for the debug comment in front of every change.
  const size_t extra = options_.debug_comments ? 256 : 0;
  Plan plan = BuildPlan(*pack_, ctx, facts_, options_.max_added_bytes, extra);
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
        if (facts_.insert_before != nullptr &&
            !parser_->IsRewritable(facts_.insert_before)) {
          return false;
        }
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

void PackFilter::InsertAtHeadEnd(net_instaweb::HtmlNode* node) {
  if (facts_.insert_before != nullptr) {
    parser_->InsertNodeBeforeNode(facts_.insert_before, node);
  } else {
    parser_->AppendChild(facts_.head, node);
  }
}

void PackFilter::SetEscaped(HtmlElement* el, HtmlName::Keyword kw,
                            const std::string& escaped) {
  auto* attr = el->FindAttribute(kw);
  if (attr != nullptr) {
    attr->SetEscapedValue(escaped);
    // An attribute that was unquoted would otherwise stay unquoted around a
    // value that may now hold spaces or quotes.
    attr->set_quote_style(HtmlElement::DOUBLE_QUOTE);
  } else {
    parser_->AddEscapedAttribute(el, kw, escaped);
  }
}

void PackFilter::AddDebugComment(const Rule& rule, const PackDecision& decision,
                                 HtmlElement* before) {
  if (!options_.debug_comments) return;
  // One comment per rule, however many links it writes.
  if (decision.kind == Kind::kHreflang) {
    if (commented_rule_ == &rule) return;
    commented_rule_ = &rule;
  }
  HtmlElement* parent = before != nullptr ? before->parent() : facts_.head;
  auto* comment = parser_->NewCommentNode(
      parent, absl::StrCat(" pagespeed-pack ", pack_->info.id, "@",
                           pack_->info.version, " rule=", rule.id,
                           " action=", ActionName(decision.action), " "));
  if (before != nullptr) {
    parser_->InsertNodeBeforeNode(before, comment);
  } else {
    InsertAtHeadEnd(comment);
  }
}

void PackFilter::ApplyOp(const Rule& rule, const PackDecision& decision,
                         const PlanOp& op) {
  HtmlElement* el = op.target.element;
  switch (op.type) {
    case OpType::kRemove:
      if (parser_->DeleteNode(el)) modified_ = true;
      return;
    case OpType::kRemoveCanonicalToken:
      if (el->FindAttribute(HtmlName::kRel) != nullptr) {
        SetEscaped(el, HtmlName::kRel,
                   EscapeAttributeValue(
                       absl::StrJoin(op.target.other_rel_tokens, " ")));
        modified_ = true;
      }
      return;
    case OpType::kSetValue: {
      AddDebugComment(rule, decision, el);
      if (op.kind == Kind::kTitle || op.kind == Kind::kJsonLd) {
        for (HtmlCharactersNode* t : op.target.text_nodes) {
          parser_->DeleteNode(t);
        }
        parser_->AppendChild(el, parser_->NewCharactersNode(el, op.value));
      } else {
        SetEscaped(el,
                   op.kind == Kind::kCanonical || op.kind == Kind::kHreflang
                       ? HtmlName::kHref
                       : HtmlName::kContent,
                   EscapeAttributeValue(op.value));
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
          parser_->AddEscapedAttribute(fresh, HtmlName::kRel, "canonical");
          parser_->AddEscapedAttribute(fresh, HtmlName::kHref,
                                       EscapeAttributeValue(op.value));
          break;
        case Kind::kTitle:
          fresh = parser_->NewElement(head, HtmlName::kTitle);
          break;
        case Kind::kHreflang:
          fresh = parser_->NewElement(head, HtmlName::kLink);
          parser_->AddEscapedAttribute(fresh, HtmlName::kRel, "alternate");
          fresh->AddEscapedAttribute(parser_->MakeName("hreflang"),
                                     EscapeAttributeValue(op.code),
                                     HtmlElement::DOUBLE_QUOTE);
          parser_->AddEscapedAttribute(fresh, HtmlName::kHref,
                                       EscapeAttributeValue(op.value));
          break;
        case Kind::kJsonLd:
          fresh = parser_->NewElement(head, HtmlName::kScript);
          parser_->AddEscapedAttribute(fresh, HtmlName::kType,
                                       "application/ld+json");
          break;
        default:
          fresh = parser_->NewElement(head, HtmlName::kMeta);
          parser_->AddEscapedAttribute(fresh, HtmlName::kName, "description");
          parser_->AddEscapedAttribute(fresh, HtmlName::kContent,
                                       EscapeAttributeValue(op.value));
          break;
      }
      AddDebugComment(rule, decision, nullptr);
      InsertAtHeadEnd(fresh);
      if (op.kind == Kind::kTitle || op.kind == Kind::kJsonLd) {
        parser_->AppendChild(fresh,
                             parser_->NewCharactersNode(fresh, op.value));
      }
      modified_ = true;
      return;
    }
  }
}

}  // namespace pagespeed::packs
