// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Regenerates the expected files of the pack fixtures from the current
// filter. Review the diff before committing it.
//
//   bazel run //tools/packs:fixture_runner -- --update \
//       "$PWD/packs/edge-seo/fixtures"

#include <cstdio>
#include <string>

#include "tools/packs/fixture_runner.h"

int main(int argc, char** argv) {
  bool update = false;
  std::string dir;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--update") {
      update = true;
    } else {
      dir = a;
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "usage: fixture_runner [--update] FIXTURES_DIR\n");
    return 2;
  }
  int failures = 0;
  for (const auto& c : pagespeed::packs::ListFixtureCases(dir)) {
    auto run = pagespeed::packs::RunCase(dir, c);
    if (!run.ok()) {
      std::fprintf(stderr, "%s: %s\n", c.name.c_str(),
                   std::string(run.status().message()).c_str());
      ++failures;
      continue;
    }
    if (update) {
      auto s = pagespeed::packs::UpdateExpected(c, *run);
      if (!s.ok()) {
        std::fprintf(stderr, "%s: %s\n", c.name.c_str(),
                     std::string(s.message()).c_str());
        ++failures;
      }
    }
    std::printf("%s\n", c.name.c_str());
  }
  return failures == 0 ? 0 : 1;
}
