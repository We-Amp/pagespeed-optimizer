// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Replays the HTML cache policy fuzz harness's embedded seeds, so the seeds
// and the harness's oracles are exercised on every CI run without libFuzzer.
// The test dumps the corpus to a temporary directory exactly as
// `html_cache_policy_fuzz --dump_corpus=DIR` does, reads every file back, and
// runs it through the harness entry point (which aborts the process on an
// oracle violation).  It also checks what the seeds cover, so a seed that
// stops reaching its reason is a red test rather than a silent loss.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/html_cache_policy.h"
#include "lib/cache/html_cache_policy_fuzz.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

TEST(HtmlCachePolicyFuzzSeedsTest, DumpedCorpusRoundTripsAndReplaysClean) {
  test::TempDir temp;
  const std::string dir = temp.path() + "/corpus";
  ASSERT_EQ(fuzz::DumpHtmlCachePolicyFuzzCorpus(dir), 0);

  const std::vector<std::string>& seeds = fuzz::HtmlCachePolicyFuzzSeeds();
  ASSERT_GE(seeds.size(), 12u);

  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  ASSERT_EQ(files.size(), seeds.size());

  size_t replayed = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string bytes = ReadFile(files[i]);
    EXPECT_EQ(bytes, seeds[i]) << files[i];
    fuzz::HtmlCachePolicyFuzzOne(bytes);
    ++replayed;
  }
  EXPECT_EQ(replayed, seeds.size());
}

TEST(HtmlCachePolicyFuzzSeedsTest, SeedsCoverEveryStoreReasonAndSignal) {
  std::set<HtmlStoreReason> reasons;
  std::set<std::string> signals;
  size_t storable = 0;
  for (const std::string& seed : fuzz::HtmlCachePolicyFuzzSeeds()) {
    const HtmlStoreVerdict v = fuzz::ClassifyHtmlCachePolicyFuzzBlob(seed);
    reasons.insert(v.reason);
    if (v.stores()) ++storable;
    if (v.reason == HtmlStoreReason::kPersonalisationSignal) {
      signals.insert(v.detail);
    }
  }
  for (uint8_t i = 0; i < kHtmlStoreReasonCount; ++i) {
    EXPECT_TRUE(reasons.count(static_cast<HtmlStoreReason>(i)) != 0)
        << "no seed reaches "
        << HtmlStoreReasonName(static_cast<HtmlStoreReason>(i));
  }
  EXPECT_GE(storable, 2u);  // a plain store and a no-transform store
  for (std::string_view signal :
       {"meta_csrf_token", "meta_csrf_param", "hidden_token", "hidden_csrf",
        "hidden_authenticity", "hidden_request_verification",
        "hidden_csrfmiddlewaretoken", "post_form_login",
        "post_form_checkout"}) {
    EXPECT_TRUE(signals.count(std::string(signal)) != 0)
        << "no seed reaches " << signal;
  }
}

TEST(HtmlCachePolicyFuzzSeedsTest, SeedsIncludeAnOversizedHeaderValue) {
  size_t longest = 0;
  for (const std::string& seed : fuzz::HtmlCachePolicyFuzzSeeds()) {
    size_t run = 0;
    for (char c : seed) {
      run = (c == '\n') ? 0 : run + 1;
      if (run > longest) longest = run;
    }
  }
  EXPECT_GE(longest, 70000u);
}

TEST(HtmlCachePolicyFuzzSeedsTest, MalformedAndEmptyInputsAreHandled) {
  for (std::string_view blob :
       {std::string_view(), std::string_view("\n\n"), std::string_view(":"),
        std::string_view("::\n::"), std::string_view("a"),
        std::string_view("\0\0\0", 3), std::string_view("x: y\n\n\n\n"),
        std::string_view("\r\n\r\n"),
        std::string_view(":status: 99999999999999999999\n\n")}) {
    fuzz::HtmlCachePolicyFuzzOne(blob);
  }
}

}  // namespace
}  // namespace pagespeed
