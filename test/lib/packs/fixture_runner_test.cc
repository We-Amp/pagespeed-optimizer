// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Runs every case under packs/edge-seo/fixtures and checks four properties:
//   1. enforce: output and decisions equal the golden files;
//   2. idempotence: the golden output run again is unchanged, and every
//      enforcing decision on it is a no-op;
//   3. report-only: the input comes back byte for byte, and the decisions
//      equal the golden ones with the mode forced to report;
//   4. device-variant independence: the output does not change with the
//      other transforms that follow the pack in the pass.

#include "tools/packs/fixture_runner.h"

#include <regex>
#include <string>
#include <vector>

#include "absl/strings/ascii.h"
#include "gtest/gtest.h"
#include "lib/packs/decision.h"
#include "lib/packs/pack_filter.h"

namespace pagespeed::packs {
namespace {

constexpr char kFixturesDir[] = "packs/edge-seo/fixtures";

std::vector<FixtureCase> Cases() { return ListFixtureCases(kFixturesDir); }

std::string PathOf(const std::string& url) {
  auto p = ParsePageUrl(url);
  return p->path + (p->query.empty() ? "" : "?" + p->query);
}

// Everything before the body: where the pack writes.
std::string HeadOf(const std::string& html) {
  std::string lower = absl::AsciiStrToLower(html);
  const size_t body = lower.find("<body");
  return html.substr(0, body);
}

class FixtureTest : public testing::TestWithParam<FixtureCase> {};

TEST_P(FixtureTest, HoldsTheFourProperties) {
  const FixtureCase& c = GetParam();
  auto run = RunCase(kFixturesDir, c);
  ASSERT_TRUE(run.ok()) << run.status();
  auto expected_html = ReadFile(c.dir + "/expected.html");
  auto expected_decisions = ReadFile(c.dir + "/expected.decisions.jsonl");
  ASSERT_TRUE(expected_html.ok()) << expected_html.status();
  ASSERT_TRUE(expected_decisions.ok()) << expected_decisions.status();
  const std::string host = ParsePageUrl(run->url)->host;
  const std::string path = PathOf(run->url);

  // 1. enforce
  EXPECT_EQ(run->enforce.html, *expected_html);
  EXPECT_EQ(DecisionsToJsonl(host, path, run->enforce.decisions, false),
            *expected_decisions);
  // (The kernel re-serializes some tag whitespace, so "unchanged" is the
  // writer round trip of the input; the caller serves the original bytes.)
  EXPECT_EQ(run->enforce.modified, run->roundtrip.html != *expected_html);

  // 2. idempotence
  EXPECT_EQ(run->idempotent.html, *expected_html);
  EXPECT_FALSE(run->idempotent.modified);
  // (A report-only rule keeps reporting what it would do; it never ran.)
  for (const PackDecision& d : run->idempotent.decisions) {
    if (d.mode != Mode::kEnforce) continue;
    EXPECT_EQ(d.action, Action::kNone)
        << d.rule_id << ": " << ReasonName(d.reason);
  }

  // 3. report-only: byte-identical, nothing modified, same decisions.
  EXPECT_EQ(run->report.html, run->roundtrip.html);
  EXPECT_FALSE(run->report.modified);
  // The expected file records effective modes; a forced-report run has
  // report for every rule.
  std::string forced;
  for (PackDecision d : run->enforce.decisions) {
    d.mode = Mode::kReport;
    forced += DecisionToJsonLine(host, path, d) + "\n";
  }
  EXPECT_EQ(DecisionsToJsonl(host, path, run->report.decisions, false), forced);

  // 4. the other transforms of the pass do not change what the pack did:
  // the head, where the pack writes, is the same whatever follows it.
  EXPECT_EQ(HeadOf(run->other_transforms.html), HeadOf(*expected_html));
  EXPECT_EQ(HeadOf(run->all_transforms.html), HeadOf(*expected_html));
  if (c.name.rfind("global-device-variant", 0) == 0) {
    // Not vacuous: the later transforms did rewrite the body here.
    EXPECT_NE(run->all_transforms.html, *expected_html);
    EXPECT_NE(run->all_transforms.html.find("loading="), std::string::npos);
  } else {
    EXPECT_EQ(run->other_transforms.html, *expected_html);
  }
}

// Whenever an enforcing hreflang rule did anything, the cluster the head ends
// with has a valid code on every entry and an entry for the page itself.
TEST_P(FixtureTest, HreflangClustersAreValidAndHaveSelf) {
  const FixtureCase& c = GetParam();
  auto run = RunCase(kFixturesDir, c);
  ASSERT_TRUE(run.ok()) << run.status();
  bool acted = false;
  for (const PackDecision& d : run->enforce.decisions) {
    if (d.kind == Kind::kHreflang && d.mode == Mode::kEnforce &&
        d.action != Action::kNone) {
      acted = true;
    }
  }
  if (!acted) return;
  static const std::regex kLens(
      "^(x-default|[a-z]{2,3}(-[a-z]{4})?(-([a-z]{2}|\\d{3}))?)$");
  static const std::regex kTag("<link\\b[^>]*>", std::regex::icase);
  static const std::regex kAttr(
      "([a-zA-Z-]+)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>\"']+))");
  const std::string head = HeadOf(run->enforce.html);
  size_t entries = 0;
  bool self = false;
  for (auto tag = std::sregex_iterator(head.begin(), head.end(), kTag);
       tag != std::sregex_iterator(); ++tag) {
    const std::string text = tag->str();
    std::string rel, code, href;
    bool has_code = false;
    for (auto at = std::sregex_iterator(text.begin(), text.end(), kAttr);
         at != std::sregex_iterator(); ++at) {
      const std::string name = absl::AsciiStrToLower((*at)[1].str());
      std::string value = (*at)[2].matched   ? (*at)[2].str()
                          : (*at)[3].matched ? (*at)[3].str()
                                             : (*at)[4].str();
      if (name == "rel") rel = absl::AsciiStrToLower(value);
      if (name == "hreflang") {
        code = absl::AsciiStrToLower(value);
        has_code = true;
      }
      if (name == "href") href = value;
    }
    // Only plain alternate links are entries of the cluster.
    if (!has_code || rel != "alternate") continue;
    ++entries;
    EXPECT_TRUE(std::regex_match(code, kLens)) << code;
    std::string page = run->url;
    while (!href.empty() && href.back() == '/') href.pop_back();
    while (!page.empty() && page.back() == '/') page.pop_back();
    if (href == page) self = true;
  }
  EXPECT_GT(entries, 0u);
  EXPECT_TRUE(self) << "no hreflang entry for " << run->url;
}

INSTANTIATE_TEST_SUITE_P(Cases, FixtureTest, testing::ValuesIn(Cases()),
                         [](const testing::TestParamInfo<FixtureCase>& info) {
                           std::string n = info.param.name;
                           for (char& ch : n) {
                             if (ch == '-') ch = '_';
                           }
                           return n;
                         });

TEST(FixtureSetTest, HasTheMinimumCoverage) { EXPECT_GE(Cases().size(), 12u); }

}  // namespace
}  // namespace pagespeed::packs
