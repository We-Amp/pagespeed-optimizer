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
