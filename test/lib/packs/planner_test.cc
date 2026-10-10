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
#include "lib/packs/url_norm.h"

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
  EXPECT_EQ(rp.decision.after, "https://t.test/x");
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

TEST(PlannerTest, DecisionsHashWhatWasOnThePageAndQuoteWhatTheRuleSets) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://secret.test/page"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  const std::string line = DecisionToJsonLine("t.test", "/x", rp.decision);
  EXPECT_EQ(line.find("secret"), std::string::npos);
  EXPECT_NE(line.find("\"after\":\"https://t.test/x\""), std::string::npos);
  EXPECT_FALSE(rp.decision.before_hash.empty());
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

// ---------------------------------------------------------------------------
// hreflang and jsonld
// ---------------------------------------------------------------------------

Pack MakeRaw(std::string_view rules, std::string_view tables = "") {
  std::string json = absl::StrCat(
      R"({"pack":{"id":"edge-seo","version":"0.1.0"},)",
      R"("sites":[{"host":"t.test","mode":"enforce"}],)",
      tables.empty() ? "" : absl::StrCat(R"("tables":)", tables, ","),
      R"("rules":[)", rules, "]}");
  auto p = LoadPack(json, "");
  EXPECT_TRUE(p.ok()) << p.status() << "\n" << json;
  return p.ok() ? *std::move(p) : Pack();
}

ElementFact Alt(int id, std::string code, std::string href,
                bool in_head = true) {
  ElementFact f;
  f.element = Handle(id);
  f.in_head = in_head;
  f.code = std::move(code);
  f.value = std::move(href);
  f.has_value_attr = true;
  return f;
}

// The page is https://t.test/en/p: "en" and "x-default" point at it.
PageContext EnCtx() { return Ctx("/en/p"); }

Pack HreflangPackFor(std::string_view on_present) {
  return MakeRaw(absl::StrCat(
      R"({"id":"h","kind":"hreflang","enforce":true,"on_present":")",
      on_present,
      R"(","value":{"pattern":{"en":"https://t.test{path}",)"
      R"("de":"https://t.test/de/p","x-default":"https://t.test{path}"}}})"));
}

size_t CountOps(const RulePlan& rp, OpType type) {
  size_t n = 0;
  for (const PlanOp& op : rp.ops) n += op.type == type;
  return n;
}

TEST(HreflangPlannerTest, KeepInsertsTheWholeClusterSortedByCode) {
  Pack pack = HreflangPackFor("keep");
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), Facts()));
  ASSERT_EQ(rp.ops.size(), 3u);
  EXPECT_EQ(rp.ops[0].code, "de");
  EXPECT_EQ(rp.ops[1].code, "en");
  EXPECT_EQ(rp.ops[2].code, "x-default");
  EXPECT_EQ(rp.ops[1].value, "https://t.test/en/p");
  EXPECT_EQ(rp.decision.action, Action::kInsert);
  EXPECT_EQ(rp.decision.reason, Reason::kAbsent);
  EXPECT_EQ(rp.decision.defect, "");
}

TEST(HreflangPlannerTest, KeepNeverTouchesAnExistingCluster) {
  Pack pack = HreflangPackFor("keep");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en_US", "/relative"));
  facts.hreflangs.push_back(Alt(2, "fr", "https://t.test/fr/p", false));
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);
  EXPECT_EQ(rp.decision.defect, "hreflang-invalid");
}

TEST(HreflangPlannerTest, KeepRemovesOnlyExactDuplicates) {
  Pack pack = HreflangPackFor("keep");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en", "https://t.test/en/p"));
  facts.hreflangs.push_back(Alt(2, "EN", "https://T.test/en/p/"));
  facts.hreflangs.push_back(Alt(3, "en", "https://t.test/other"));
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.decision.action, Action::kDedupe);
}

TEST(HreflangPlannerTest, ClusterWithoutSelfIsSkipped) {
  Pack pack = MakeRaw(
      R"({"id":"h","kind":"hreflang","enforce":true,)"
      R"("value":{"pattern":{"en":"https://t.test/elsewhere"}}})");
  const RulePlan rp = Only(BuildPlan(pack, Ctx("/x"), Facts()));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kClusterWithoutSelf);
}

TEST(HreflangPlannerTest, AnInvalidExpandedHrefSkipsTheRule) {
  Pack pack = MakeRaw(
      R"({"id":"h","kind":"hreflang","enforce":true,)"
      R"("value":{"pattern":{"en":"{path}"}}})");
  const RulePlan rp = Only(BuildPlan(pack, Ctx("/x"), Facts()));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kInvalidValue);
}

TEST(HreflangPlannerTest, TableLookupUsesTheClusterOfThePage) {
  Pack pack = MakeRaw(
      R"({"id":"h","kind":"hreflang","enforce":true,)"
      R"("value":{"table":"hreflang"}})",
      R"({"hreflang":[{"en":"https://t.test/en/",)"
      R"("de":"https://t.test/de/"}]})");
  const RulePlan hit = Only(BuildPlan(pack, Ctx("/de"), Facts()));
  EXPECT_EQ(hit.ops.size(), 2u);  // "/de" equals "/de/"
  const RulePlan miss = Only(BuildPlan(pack, Ctx("/fr/"), Facts()));
  EXPECT_TRUE(miss.ops.empty());
  EXPECT_EQ(miss.decision.reason, Reason::kNoValue);
}

TEST(HreflangPlannerTest, RepairDropsUnusableElementsAndAddsWhatIsMissing) {
  Pack pack = HreflangPackFor("repair");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en_US", "https://t.test/en/p"));  // code
  facts.hreflangs.push_back(Alt(2, "de", "/de/p"));  // relative
  facts.hreflangs.push_back(Alt(3, "de", "", true));  // empty
  facts.hreflangs.push_back(
      Alt(4, "x-default", "https://t.test/en/p", false));  // body
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  EXPECT_EQ(CountOps(rp, OpType::kRemove), 4u);
  EXPECT_EQ(CountOps(rp, OpType::kInsert), 3u);
  EXPECT_EQ(rp.decision.action, Action::kReplace);
  EXPECT_EQ(rp.decision.defect, "hreflang-invalid");
}

TEST(HreflangPlannerTest, RepairKeepsTheFirstUsableElementPerCode) {
  Pack pack = HreflangPackFor("repair");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en", "https://t.test/en/p"));
  facts.hreflangs.push_back(Alt(2, "x-default", "https://t.test/en/p"));
  facts.hreflangs.push_back(Alt(3, "de", "https://t.test/de/first"));
  facts.hreflangs.push_back(Alt(4, "de", "https://t.test/de/second"));
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].target.element, Handle(4));
  EXPECT_EQ(rp.decision.reason, Reason::kConflict);
}

TEST(HreflangPlannerTest, RepairLeavesAnEqualClusterAlone) {
  Pack pack = HreflangPackFor("repair");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "EN", "https://T.test/en/p"));
  facts.hreflangs.push_back(Alt(2, "x-default", "https://t.test/en/p#top"));
  facts.hreflangs.push_back(Alt(3, "de", "https://t.test/de/p"));
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);
}

TEST(HreflangPlannerTest, RepairPointsASelfEntryAtThePage) {
  Pack pack = HreflangPackFor("repair");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en", "https://t.test/en/other"));
  facts.hreflangs.push_back(Alt(2, "x-default", "https://t.test/en/other"));
  facts.hreflangs.push_back(Alt(3, "de", "https://t.test/de/p"));
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.ops[0].value, "https://t.test/en/p");
  EXPECT_EQ(rp.decision.defect, "hreflang-no-self");
}

TEST(HreflangPlannerTest, ReplaceEndsWithExactlyTheTargetEntries) {
  Pack pack = HreflangPackFor("replace");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en", "https://t.test/en/p"));  // equal
  facts.hreflangs.push_back(Alt(2, "de", "https://t.test/old"));   // rewritten
  facts.hreflangs.push_back(Alt(3, "fr", "https://t.test/fr/p"));  // not ours
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  EXPECT_EQ(CountOps(rp, OpType::kSetValue), 1u);
  EXPECT_EQ(CountOps(rp, OpType::kRemove), 1u);
  EXPECT_EQ(CountOps(rp, OpType::kInsert), 1u);  // x-default
  for (const PlanOp& op : rp.ops) {
    if (op.type == OpType::kRemove) EXPECT_EQ(op.target.element, Handle(3));
    if (op.type == OpType::kSetValue) EXPECT_EQ(op.target.element, Handle(2));
  }
}

TEST(HreflangPlannerTest, ReplaceWithTheSameClusterChangesNothing) {
  Pack pack = HreflangPackFor("replace");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en", "https://t.test/en/p"));
  facts.hreflangs.push_back(Alt(2, "x-default", "https://t.test/en/p"));
  facts.hreflangs.push_back(Alt(3, "de", "https://t.test/de/p"));
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);
}

TEST(HreflangPlannerTest, EveryEmittedCodeIsValidAndTheClusterHasSelf) {
  for (std::string_view on_present : {"keep", "repair", "replace"}) {
    Pack pack = HreflangPackFor(on_present);
    const RulePlan rp = Only(BuildPlan(pack, EnCtx(), Facts()));
    bool self = false;
    for (const PlanOp& op : rp.ops) {
      ASSERT_EQ(op.type, OpType::kInsert);
      EXPECT_TRUE(NormalizeHreflangCode(op.code).has_value()) << op.code;
      EXPECT_EQ(NormalizeHreflangCode(op.code), op.code);
      if (NormUrl(op.value) == NormUrl("https://t.test/en/p")) self = true;
    }
    EXPECT_TRUE(self) << on_present;
  }
}

constexpr char kOrgTemplate[] =
    R"({\"@context\":\"https://schema.org\",\"@type\":\"Organization\",)"
    R"(\"name\":\"Acme\"})";

std::string JsonRule(std::string_view on_present, std::string_view tpl,
                     bool enforce = true) {
  return absl::StrCat(R"({"id":"j","kind":"jsonld","enforce":)",
                      enforce ? "true" : "false", R"(,"on_present":")",
                      on_present, R"(","value":{"template":")", tpl, R"("}})");
}

ElementFact Ld(int id, std::string text, bool in_head = true) {
  ElementFact f;
  f.element = Handle(id);
  f.in_head = in_head;
  f.value = std::move(text);
  return f;
}

TEST(JsonLdPlannerTest, KeepInsertsOnlyWhenNoBlockCarriesTheType) {
  Pack pack = MakeRaw(JsonRule("keep", kOrgTemplate));
  RulePlan rp = Only(BuildPlan(pack, Ctx(), Facts()));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kInsert);
  EXPECT_EQ(rp.decision.after, "Organization");

  PageFacts other = Facts();
  other.jsonlds.push_back(Ld(1, R"({"@type":"BreadcrumbList"})"));
  EXPECT_EQ(Only(BuildPlan(pack, Ctx(), other)).ops.size(), 1u);

  PageFacts same = Facts();
  same.jsonlds.push_back(Ld(1, R"({"@type":["Thing","Organization"]})", false));
  rp = Only(BuildPlan(pack, Ctx(), same));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);

  PageFacts graph = Facts();
  graph.jsonlds.push_back(
      Ld(1, R"({"@graph":[{"@type":"WebSite"},{"@type":"Organization"}]})"));
  EXPECT_TRUE(Only(BuildPlan(pack, Ctx(), graph)).ops.empty());

  PageFacts nested = Facts();  // only top-level types count
  nested.jsonlds.push_back(
      Ld(1, R"({"@type":"WebPage","publisher":{"@type":"Organization"}})"));
  EXPECT_EQ(Only(BuildPlan(pack, Ctx(), nested)).ops.size(), 1u);
}

TEST(JsonLdPlannerTest, KeepStandsDownOnABrokenBlock) {
  Pack pack = MakeRaw(JsonRule("keep", kOrgTemplate));
  PageFacts facts = Facts();
  facts.jsonlds.push_back(Ld(1, "{broken"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.defect, "jsonld-invalid");
}

TEST(JsonLdPlannerTest, RepairRemovesBrokenBlocksAndInsertsWhenNoneMatches) {
  Pack pack = MakeRaw(JsonRule("repair", kOrgTemplate));
  PageFacts facts = Facts();
  facts.jsonlds.push_back(Ld(1, "{broken"));
  facts.jsonlds.push_back(Ld(2, "  "));
  RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_EQ(CountOps(rp, OpType::kRemove), 2u);
  EXPECT_EQ(CountOps(rp, OpType::kInsert), 1u);
  EXPECT_EQ(rp.decision.defect, "jsonld-invalid");

  facts.jsonlds.push_back(Ld(3, R"({"@type":"Organization","name":"Mine"})"));
  rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_EQ(CountOps(rp, OpType::kRemove), 2u);
  EXPECT_EQ(CountOps(rp, OpType::kInsert), 0u);
  EXPECT_EQ(rp.decision.action, Action::kRemove);
}

TEST(JsonLdPlannerTest, ReplaceRewritesInPlaceAndSparesOtherTypes) {
  Pack pack = MakeRaw(JsonRule("replace", kOrgTemplate));
  PageFacts facts = Facts();
  facts.jsonlds.push_back(Ld(1, R"({"@type":"BreadcrumbList"})"));
  facts.jsonlds.push_back(Ld(2, R"({"@type":"Organization","name":"Old"})"));
  facts.jsonlds.push_back(Ld(3, R"({"@type":"Organization","name":"Dup"})"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.ops[1].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[1].target.element, Handle(3));
}

TEST(JsonLdPlannerTest, ReplaceStandsDownOnABlockWithOtherTypesToo) {
  Pack pack = MakeRaw(JsonRule("replace", kOrgTemplate));
  for (const char* block :
       {R"({"@graph":[{"@type":"WebPage"},{"@type":"Organization"}]})",
        R"({"@type":["Organization","LocalBusiness"],"address":"x"})"}) {
    PageFacts facts = Facts();
    facts.jsonlds.push_back(Ld(1, R"({"@type":"Organization"})"));
    facts.jsonlds.push_back(Ld(2, block));
    const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
    EXPECT_TRUE(rp.ops.empty()) << block;
    EXPECT_EQ(rp.decision.reason, Reason::kConflict) << block;
  }
}

TEST(JsonLdPlannerTest, ADeeplyNestedBlockIsNeverParsedOrRemoved) {
  for (std::string_view on_present : {"keep", "repair", "replace"}) {
    Pack pack = MakeRaw(JsonRule(on_present, kOrgTemplate));
    PageFacts facts = Facts();
    facts.jsonlds.push_back(Ld(1, std::string(100000, '[') +
                                       std::string(100000, ']')));
    const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
    EXPECT_TRUE(rp.ops.empty()) << on_present;
    EXPECT_EQ(rp.decision.reason, Reason::kPresent) << on_present;
  }
}

TEST(HreflangPlannerTest, RepairNeverDropsTheEntryForThePageInAConflict) {
  Pack pack = HreflangPackFor("repair");
  PageFacts facts = Facts();
  facts.hreflangs.push_back(Alt(1, "en", "https://t.test/en/other"));
  facts.hreflangs.push_back(Alt(2, "en", "https://t.test/en/p"));
  facts.hreflangs.push_back(Alt(3, "x-default", "https://t.test/en/p"));
  facts.hreflangs.push_back(Alt(4, "de", "https://t.test/de/p"));
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(1));
  EXPECT_EQ(rp.decision.reason, Reason::kConflict);
  EXPECT_NE(rp.decision.removed.find("(conflict)"), std::string::npos);
}

TEST(HreflangPlannerTest, TheRemovedListIsCappedAndCountsWhatDidNotFit) {
  Pack pack = HreflangPackFor("repair");
  PageFacts facts = Facts();
  for (int i = 0; i < 300; ++i) {
    facts.hreflangs.push_back(
        Alt(i + 1, "xx_1", absl::StrCat("https://t.test/many/", i)));
  }
  const RulePlan rp = Only(BuildPlan(pack, EnCtx(), facts));
  EXPECT_EQ(CountOps(rp, OpType::kRemove), 300u);
  EXPECT_LE(rp.decision.removed.size(), kMaxAfterBytes);
  const size_t at = rp.decision.removed.rfind("...(+");
  ASSERT_NE(at, std::string::npos) << rp.decision.removed;
  size_t listed = 1;
  for (size_t p = rp.decision.removed.find("; "); p < at;
       p = rp.decision.removed.find("; ", p + 2)) {
    ++listed;
  }
  listed -= 1;  // the "; " before the marker
  const size_t more = std::stoul(rp.decision.removed.substr(at + 5));
  EXPECT_EQ(listed + more, 300u) << rp.decision.removed;
}

TEST(JsonLdPlannerTest, ReplaceLeavesAnEqualBlockAlone) {
  Pack pack = MakeRaw(JsonRule("replace", kOrgTemplate));
  PageFacts facts = Facts();
  facts.jsonlds.push_back(Ld(
      1, R"({ "name": "Acme", "@type": "Organization",)"
         R"( "@context": "https://schema.org" })"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);
}

TEST(JsonLdPlannerTest, DecisionNeverQuotesBlockContents) {
  Pack pack = MakeRaw(JsonRule("replace", kOrgTemplate));
  PageFacts facts = Facts();
  facts.jsonlds.push_back(Ld(1, R"({"@type":"Organization","name":"Secret"})"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  const std::string line = DecisionToJsonLine("t.test", "/x", rp.decision);
  EXPECT_EQ(line.find("Secret"), std::string::npos);
  EXPECT_EQ(line.find("Acme"), std::string::npos);
  EXPECT_NE(line.find("\"after\":\"Organization\""), std::string::npos);
}

TEST(JsonLdPlannerTest, ATemplateThatDoesNotParseOrHasNoTypeIsSkipped) {
  for (std::string_view tpl :
       {R"({\"@type\":\"Thing\",\"name\":\"{title}\")",
        R"({\"name\":\"no type\"})", R"(\"just a string\")"}) {
    Pack pack = MakeRaw(JsonRule("keep", tpl));
    const RulePlan rp = Only(BuildPlan(pack, Ctx(), Facts()));
    EXPECT_TRUE(rp.ops.empty()) << tpl;
    EXPECT_EQ(rp.decision.reason, Reason::kInvalidValue) << tpl;
  }
}

TEST(JsonLdPlannerTest, AnOversizeExpansionIsSkipped) {
  Pack pack = MakeRaw(
      absl::StrCat(R"({"id":"t","kind":"title","enforce":true,"on_present":)",
                   R"("keep","value":{"template":"x"}},)",
                   JsonRule("keep",
                            R"({\"@type\":\"Thing\",\"name\":\"{title}\"})")));
  PageFacts facts = Facts();
  facts.titles.push_back(Fact(1, std::string(kMaxJsonLdBytes + 10, 'a')));
  Plan plan = BuildPlan(pack, Ctx(), facts, /*max_added_bytes=*/1 << 30);
  ASSERT_EQ(plan.rules.size(), 2u);
  EXPECT_EQ(plan.rules[1].decision.reason, Reason::kInvalidValue);
}

TEST(JsonLdPlannerTest, AnUnreadableBlockLeavesTheJsonLdAlone) {
  Pack pack = MakeRaw(JsonRule("replace", kOrgTemplate));
  PageFacts facts = Facts();
  ElementFact big = Ld(1, "");
  big.unreadable = true;
  facts.jsonlds.push_back(std::move(big));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);
}

TEST(JsonLdPlannerTest, ValuesAreJsonEscapedAndCannotCloseTheScript) {
  Pack pack = MakeRaw(JsonRule(
      "keep", R"({\"@type\":\"Thing\",\"name\":\"{title}\"})"));
  PageFacts facts = Facts();
  facts.titles.push_back(Fact(1, "a </script><b> \"q\" \\ end"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  const std::string& v = rp.ops[0].value;
  EXPECT_EQ(v.find('<'), std::string::npos);
  EXPECT_NE(v.find("a \\u003c/script>\\u003cb> \\\"q\\\" \\\\ end"),
            std::string::npos)
      << v;
}

TEST(JsonLdPlannerTest, PlaceholdersSeeTheValuesTheEarlierRulesWrite) {
  const std::string rules = absl::StrCat(
      R"({"id":"c","kind":"canonical","enforce":true,)",
      R"("value":{"template":"https://t.test/canon{path}"}},)",
      R"({"id":"t","kind":"title","enforce":true,)",
      R"("value":{"template":"Title {path}"}},)",
      JsonRule("keep",
               R"({\"@type\":\"Thing\",\"name\":\"{title}\",)"
               R"(\"url\":\"{canonical}\",\"d\":\"[{description}]\"})"));
  Pack pack = MakeRaw(rules);
  Plan plan = BuildPlan(pack, Ctx(), Facts());
  ASSERT_EQ(plan.rules.size(), 3u);
  const std::string& v = plan.rules[2].ops.at(0).value;
  EXPECT_NE(v.find(R"("name":"Title /x")"), std::string::npos) << v;
  EXPECT_NE(v.find(R"("url":"https://t.test/canon/x")"), std::string::npos);
  EXPECT_NE(v.find(R"("d":"[]")"), std::string::npos);  // no rule, no value
}

TEST(JsonLdPlannerTest, PlaceholdersSeeTheValuesThatSurviveTheEarlierRules) {
  const std::string rules = absl::StrCat(
      R"({"id":"c","kind":"canonical","enforce":true,"on_present":"replace",)",
      R"("value":{"template":"https://t.test/new"}},)",
      JsonRule("keep", R"({\"@type\":\"Thing\",\"url\":\"{canonical}\"})"));
  Pack pack = MakeRaw(rules);
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://t.test/old"));
  Plan plan = BuildPlan(pack, Ctx(), facts);
  EXPECT_NE(plan.rules[1].ops.at(0).value.find("https://t.test/new"),
            std::string::npos);
}

TEST(JsonLdPlannerTest, ARulesThatOnlyReportsIsNotQuotedByAnEnforcingOne) {
  // The canonical rule is report-only, so the document keeps its own value;
  // an enforcing jsonld rule must quote that, not the value the report-only
  // rule would write.
  const std::string rules = absl::StrCat(
      R"({"id":"c","kind":"canonical","enforce":false,"on_present":"replace",)",
      R"("value":{"template":"https://t.test/new"}},)",
      JsonRule("keep", R"({\"@type\":\"Thing\",\"url\":\"{canonical}\"})"));
  Pack pack = MakeRaw(rules);
  PageFacts facts = Facts();
  facts.canonicals.push_back(Fact(1, "https://t.test/old"));
  Plan plan = BuildPlan(pack, Ctx(), facts);
  const std::string& enforcing = plan.rules[1].ops.at(0).value;
  EXPECT_NE(enforcing.find("https://t.test/old"), std::string::npos);
  EXPECT_EQ(enforcing.find("https://t.test/new"), std::string::npos);

  // A report-only jsonld rule shows what the whole pack would do.
  const std::string reporting_rules = absl::StrCat(
      R"({"id":"c","kind":"canonical","enforce":true,"on_present":"replace",)",
      R"("value":{"template":"https://t.test/new"}},)",
      JsonRule("keep", R"({\"@type\":\"Thing\",\"url\":\"{canonical}\"})",
               /*enforce=*/false));
  Pack reporting = MakeRaw(reporting_rules);
  Plan rplan = BuildPlan(reporting, Ctx(), facts);
  EXPECT_NE(rplan.rules[1].ops.at(0).value.find("https://t.test/new"),
            std::string::npos);
}

TEST(JsonLdPlannerTest, SizeLimitCountsTheScriptText) {
  Pack pack = MakeRaw(JsonRule("keep", kOrgTemplate));
  Plan plan = BuildPlan(pack, Ctx(), Facts(), /*max_added_bytes=*/50);
  EXPECT_EQ(plan.skip, SkipReason::kSizeLimit);
}

}  // namespace
}  // namespace pagespeed::packs
