// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Pure planner tests: facts in, plan out, no HTML parsing.

#include "lib/packs/planner.h"

#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "lib/packs/decision.h"
#include "lib/packs/pack_loader.h"

namespace pagespeed::packs {
namespace {

Pack MakePack(
    std::string_view kind, std::string_view on_present,
    std::string_view value = "{\"template\":\"https://t.test{path}\"}",
    std::string_view extra = "") {
  std::string json = absl::StrCat(
      R"({"pack":{"id":"edge-seo","version":"0.1.0"},)",
      R"("sites":[{"host":"t.test","mode":"enforce"}],"rules":[{"id":"r1",)",
      R"("kind":")", kind, R"(","enforce":true,"value":)", value,
      R"(,"on_present":")", on_present, R"("})", extra, "]}");
  auto p = LoadPack(json, "");
  EXPECT_TRUE(p.ok()) << p.status() << "\n" << json;
  return p.ok() ? *std::move(p) : Pack();
}

PageContext Ctx(std::string_view path = "/x") {
  PageContext c;
  c.url = {"https", "t.test", std::string(path), ""};
  c.global_mode = Mode::kEnforce;
  return c;
}

// A fake element handle: the planner never dereferences it.
net_instaweb::HtmlElement* Handle(int n) {
  return reinterpret_cast<net_instaweb::HtmlElement*>(
      static_cast<uintptr_t>(n * 16));
}

ElementFact Fact(int id, std::string value, bool in_head = true) {
  ElementFact f;
  f.element = Handle(id);
  f.in_head = in_head;
  f.value = std::move(value);
  f.has_value_attr = true;
  return f;
}

PageFacts Facts() {
  PageFacts f;
  f.head = Handle(100);
  return f;
}

RulePlan Only(const Plan& plan) {
  EXPECT_EQ(plan.rules.size(), 1u);
  return plan.rules.front();
}

TEST(PlannerTest, HostNotListedSkipsThePage) {
  Pack pack = MakePack("canonical", "keep");
  PageContext ctx = Ctx();
  ctx.url.host = "other.test";
  Plan plan = BuildPlan(pack, ctx, Facts());
  EXPECT_EQ(plan.skip, SkipReason::kHostNotListed);
  EXPECT_TRUE(plan.rules.empty());
}

TEST(PlannerTest, NoHeadSkipsThePage) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts;  // head == nullptr
  EXPECT_EQ(BuildPlan(pack, Ctx(), facts).skip, SkipReason::kNoHead);
}

TEST(PlannerTest, KeepInsertsOnlyWhenAbsent) {
  Pack pack = MakePack("canonical", "keep");
  Plan plan = BuildPlan(pack, Ctx(), Facts());
  const RulePlan rp = Only(plan);
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kInsert);
  EXPECT_EQ(rp.ops[0].value, "https://t.test/x");
  EXPECT_EQ(rp.decision.action, Action::kInsert);
  EXPECT_EQ(rp.decision.reason, Reason::kAbsent);
  EXPECT_EQ(rp.decision.defect, "canonical-missing");
  EXPECT_FALSE(rp.decision.new_hash.empty());
}

TEST(PlannerTest, KeepLeavesEveryPresentFormAlone) {
  Pack pack = MakePack("canonical", "keep");
  for (const auto& [value, in_head] :
       {std::pair{"https://other.test/", true}, std::pair{"", true},
        std::pair{"javascript:void(0)", true},
        std::pair{"https://other.test/", false}}) {
    PageFacts facts = Facts();
    facts.canonicals.push_back(Fact(1, value, in_head));
    Plan plan = BuildPlan(pack, Ctx(), facts);
    EXPECT_TRUE(Only(plan).ops.empty()) << value;
    EXPECT_EQ(Only(plan).decision.action, Action::kNone) << value;
  }
}

TEST(PlannerTest, KeepRemovesOnlyExactDuplicates) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/p"));
  facts.canonicals.push_back(Fact(2, "https://A.test/p/"));
  facts.canonicals.push_back(Fact(3, "https://b.test/p"));
  Plan plan = BuildPlan(pack, Ctx(), facts);
  const RulePlan rp = Only(plan);
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.decision.action, Action::kDedupe);
  EXPECT_EQ(rp.decision.defect, "canonical-multiple");  // b.test remains
}

TEST(PlannerTest, KeepPrefersTheDuplicateInHead) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/p", false));
  facts.canonicals.push_back(Fact(2, "https://a.test/p", true));
  Plan plan = BuildPlan(pack, Ctx(), facts);
  ASSERT_EQ(Only(plan).ops.size(), 1u);
  EXPECT_EQ(Only(plan).ops[0].target.element, Handle(1));
}

TEST(PlannerTest, KeepReportsConflictWithoutChange) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/p"));
  facts.canonicals.push_back(Fact(2, "https://b.test/p"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kConflict);
}

TEST(PlannerTest, RepairKeepsTheFirstUsableAndDropsTheRest) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/p"));
  facts.canonicals.push_back(Fact(2, "https://b.test/p"));
  facts.canonicals.push_back(Fact(3, "https://c.test/p", false));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.ops[1].target.element, Handle(3));
  EXPECT_EQ(rp.decision.reason, Reason::kConflict);
  EXPECT_EQ(rp.decision.defect, "canonical-multiple");
}

TEST(PlannerTest, RepairFillsAnEmptyElementInHeadInPlace) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, ""));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.decision.action, Action::kFill);
  EXPECT_EQ(rp.decision.reason, Reason::kEmpty);
  EXPECT_EQ(rp.decision.defect, "canonical-unusable");
}

TEST(PlannerTest, RepairMovesAnOutOfHeadElementIntoHead) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/p", false));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[1].type, OpType::kInsert);
  EXPECT_EQ(rp.decision.reason, Reason::kOutOfHead);
}

TEST(PlannerTest, RepairLeavesAUsableElementAloneWhateverItsValue) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://other.test/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);
}

TEST(PlannerTest, RepairResolvesRelativeHrefsAgainstBase) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.base_href = "https://t.test/dir/";
  facts.canonicals.push_back(Fact(1, "x"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx("/dir/x"), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);
}

TEST(PlannerTest, ReplaceLeavesAnEqualElementUntouched) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://t.test/x/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);
}

TEST(PlannerTest, ReplaceRewritesTheFirstInHeadElementAndRemovesTheRest) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/"));
  facts.canonicals.push_back(Fact(2, "https://b.test/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.ops[0].target.element, Handle(1));
  EXPECT_EQ(rp.ops[1].type, OpType::kRemove);
  EXPECT_EQ(rp.decision.action, Action::kReplace);
}

TEST(PlannerTest, ReplaceKeepsAnEqualElementWhenOthersDiffer) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/"));
  facts.canonicals.push_back(Fact(2, "https://t.test/x"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(1));
}

TEST(PlannerTest, ExtraRelTokensAreKeptOnRemoval) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/"));
  ElementFact second = Fact(2, "https://b.test/");
  second.other_rel_tokens = {"alternate"};
  facts.canonicals.push_back(second);
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemoveCanonicalToken);
}

TEST(PlannerTest, TitleUsesTheEscapedTextAndComparesDecoded) {
  Pack pack = MakePack("title", "replace", R"({"template":"Fish & Chips"})");
  PageFacts facts = Facts();
  facts.titles.push_back(Fact(1, "Fish &amp; Chips"));  // value is decoded
  facts.titles.back().value = "Fish & Chips";
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());  // already equal after decoding
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);

  PageFacts none = Facts();
  const RulePlan ins = Only(BuildPlan(pack, Ctx(), none));
  ASSERT_EQ(ins.ops.size(), 1u);
  EXPECT_EQ(ins.ops[0].value, "Fish &amp; Chips");
}

TEST(PlannerTest, TitleRepairFillsABlankTitle) {
  Pack pack = MakePack("title", "repair", R"({"template":"T {path}"})");
  PageFacts facts = Facts();
  facts.titles.push_back(Fact(1, "  \n "));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_EQ(rp.decision.action, Action::kFill);
  EXPECT_EQ(rp.decision.defect, "title-missing");
  EXPECT_EQ(rp.ops[0].value, "T /x");
}

TEST(PlannerTest, DescriptionKeepDedupesAfterWhitespaceCollapse) {
  Pack pack = MakePack("description", "keep", R"({"template":"D"})");
  PageFacts facts = Facts();
  facts.descriptions.push_back(Fact(1, "a  b"));
  facts.descriptions.push_back(Fact(2, " a b "));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.decision.action, Action::kDedupe);
  EXPECT_TRUE(rp.decision.defect.empty());
}

TEST(PlannerTest, InvalidExpandedUrlIsSkippedInEveryMode) {
  Pack pack = MakePack("canonical", "replace", R"({"template":"{path}"})");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://a.test/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kInvalidValue);
}

TEST(PlannerTest, EffectiveModeIsTheLowestOfTheThree) {
  Pack pack = MakePack("canonical", "keep");
  PageContext ctx = Ctx();
  ctx.global_mode = Mode::kReport;
  EXPECT_EQ(Only(BuildPlan(pack, ctx, Facts())).decision.mode, Mode::kReport);
  ctx.global_mode = Mode::kEnforce;
  EXPECT_EQ(Only(BuildPlan(pack, ctx, Facts())).decision.mode, Mode::kEnforce);
}

TEST(PlannerTest, SizeLimitDropsTheWholePlan) {
  Pack pack = MakePack("canonical", "keep");
  Plan plan = BuildPlan(pack, Ctx(), Facts(), /*max_added_bytes=*/10);
  EXPECT_EQ(plan.skip, SkipReason::kSizeLimit);
  EXPECT_TRUE(plan.rules.empty());
}

TEST(PlannerTest, DecisionsCarryHashesNeverText) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://secret.test/page"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  const std::string line = DecisionToJsonLine("t.test", "/x", rp.decision);
  EXPECT_EQ(line.find("secret"), std::string::npos);
  EXPECT_NE(line.find("fnv1a64:"), std::string::npos);
}

TEST(HelpersTest, CollapseWhitespace) {
  EXPECT_EQ(CollapseWhitespace("  a \t\n b  "), "a b");
  EXPECT_EQ(CollapseWhitespace(""), "");
  EXPECT_EQ(CollapseWhitespace(" \n"), "");
}

TEST(HelpersTest, ResolveUrl) {
  const std::string base = "https://a.test/d/e/f?q=1#frag";
  EXPECT_EQ(ResolveUrl(base, "g"), "https://a.test/d/e/g");
  EXPECT_EQ(ResolveUrl(base, "../g"), "https://a.test/d/g");
  EXPECT_EQ(ResolveUrl(base, "../../../g"), "https://a.test/g");
  EXPECT_EQ(ResolveUrl(base, "/g"), "https://a.test/g");
  EXPECT_EQ(ResolveUrl(base, "//b.test/g"), "https://b.test/g");
  EXPECT_EQ(ResolveUrl(base, "?z=2"), "https://a.test/d/e/f?z=2");
  EXPECT_EQ(ResolveUrl(base, ""), "https://a.test/d/e/f?q=1");
  EXPECT_EQ(ResolveUrl(base, "http://c.test/x"), "http://c.test/x");
  EXPECT_EQ(ResolveUrl(base, "./g/."), "https://a.test/d/e/g/");
  EXPECT_EQ(ResolveUrl("not a url", "g"), "");
}

}  // namespace
}  // namespace pagespeed::packs
