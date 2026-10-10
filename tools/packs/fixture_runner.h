// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_TOOLS_PACKS_FIXTURE_RUNNER_H_
#define PAGESPEED_TOOLS_PACKS_FIXTURE_RUNNER_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "lib/packs/decision.h"
#include "lib/packs/pack.h"
#include "lib/packs/planner.h"

namespace pagespeed::packs {

// How the page is run through the HTML pass. The pack filter is identical in
// all of them; the variants differ only in the other transforms that follow
// it, the way the worker's HTML pass differs between device classes.
enum class PassVariant {
  kPackOnly,         // PackFilter alone
  kOtherTransforms,  // PackFilter, then the transform filter with none enabled
  kAllTransforms,    // PackFilter, then the transform filter with the
                     // image/lazy-load/preconnect transforms enabled
};

struct RunResult {
  std::string html;
  std::vector<PackDecision> decisions;
  bool modified = false;
  bool would_modify = false;
  SkipReason skip = SkipReason::kNone;
};

struct RunOptions {
  Mode global_mode = Mode::kEnforce;
  bool debug_comments = false;
  PassVariant variant = PassVariant::kPackOnly;
};

// Runs `html` (served at `url`) through the pack filter.
absl::StatusOr<RunResult> RunPackOnHtml(std::shared_ptr<const Pack> pack,
                                        std::string_view url,
                                        std::string_view html,
                                        const RunOptions& options);

// A fixture directory: input.html, request.json, expected.html,
// expected.decisions.jsonl. request.json: {"url": "...", "mode":
// "enforce"|"report" (the global mode, default enforce), "debug": bool,
// "pack": "optional file name next to pack.json"}.
struct FixtureCase {
  std::string name;
  std::string dir;
};

// The case directories under `fixtures_dir`, sorted by name.
std::vector<FixtureCase> ListFixtureCases(const std::string& fixtures_dir);

// Decision lines of a run, one JSON object per line, each ending in '\n'.
// `force_report` rewrites every mode to "report", the shape a report-only
// run must produce.
std::string DecisionsToJsonl(std::string_view host, std::string_view path,
                             const std::vector<PackDecision>& decisions,
                             bool force_report);

// Everything the runner derives for one case, for checking or regenerating
// the expected files.
struct CaseRun {
  std::string input_html;
  std::string url;
  RunResult enforce;  // global mode as the request says
  RunResult report;   // forced report-only
  // The input through the writer with no pack: what "unchanged" looks like,
  // since the kernel re-serializes some tag whitespace.
  RunResult roundtrip;
  RunResult idempotent;  // `enforce` output run again
  // Output under each pass variant (global mode as the request says).
  RunResult other_transforms;
  RunResult all_transforms;
};

absl::StatusOr<CaseRun> RunCase(const std::string& fixtures_dir,
                                const FixtureCase& c);

// Rewrites expected.html and expected.decisions.jsonl of `c` from a run.
absl::Status UpdateExpected(const FixtureCase& c, const CaseRun& run);

absl::StatusOr<std::string> ReadFile(const std::string& path);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_TOOLS_PACKS_FIXTURE_RUNNER_H_
