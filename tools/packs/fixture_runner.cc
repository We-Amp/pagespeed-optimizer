// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "tools/packs/fixture_runner.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "lib/base/string_writer.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"
#include "lib/packs/pack_filter.h"
#include "lib/packs/pack_loader.h"
#include "nlohmann/json.hpp"
#include "src/worker/html_transform_filter.h"

namespace pagespeed::packs {

namespace fs = std::filesystem;

absl::StatusOr<std::string> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return absl::NotFoundError(absl::StrCat("cannot read ", path));
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

absl::StatusOr<RunResult> RunPackOnHtml(std::shared_ptr<const Pack> pack,
                                        std::string_view url,
                                        std::string_view html,
                                        const RunOptions& options) {
  std::optional<PageUrl> page = ParsePageUrl(url);
  if (!page.has_value()) {
    return absl::InvalidArgumentError(absl::StrCat("bad url ", url));
  }
  net_instaweb::HtmlKeywords::Init();
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);

  PackFilterOptions fo;
  fo.debug_comments = options.debug_comments;
  PackFilter filter(&parser, std::move(pack), *page, options.global_mode, fo);
  parser.AddFilter(&filter);

  std::unique_ptr<pagespeed::HtmlTransformFilter> transform;
  if (options.variant != PassVariant::kPackOnly) {
    pagespeed::HtmlTransformConfig cfg;
    const bool all = options.variant == PassVariant::kAllTransforms;
    cfg.enable_critical_css = false;
    cfg.enable_lazy_load = all;
    cfg.enable_image_dimensions = all;
    cfg.enable_lcp_preload = all;
    cfg.enable_preconnect_injection = all;
    transform = std::make_unique<pagespeed::HtmlTransformFilter>(
        &parser, cfg, "", nullptr, page->host, page->scheme);
    parser.AddFilter(transform.get());
  }

  RunResult result;
  net_instaweb::StringWriter writer(&result.html);
  net_instaweb::HtmlWriterFilter writer_filter(&parser);
  writer_filter.set_writer(&writer);
  parser.AddFilter(&writer_filter);

  if (!parser.StartParse(url)) {
    return absl::InvalidArgumentError("parse did not start");
  }
  parser.ParseText(html);
  parser.FinishParse();
  result.decisions = filter.decisions();
  result.modified = filter.modified();
  result.would_modify = filter.would_modify();
  result.skip = filter.skip_reason();
  return result;
}

std::vector<FixtureCase> ListFixtureCases(const std::string& fixtures_dir) {
  std::vector<FixtureCase> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fixtures_dir, ec)) {
    if (!e.is_directory()) continue;
    out.push_back({e.path().filename().string(), e.path().string()});
  }
  std::sort(out.begin(), out.end(),
            [](const FixtureCase& a, const FixtureCase& b) {
              return a.name < b.name;
            });
  return out;
}

std::string DecisionsToJsonl(std::string_view host, std::string_view path,
                             const std::vector<PackDecision>& decisions,
                             bool force_report) {
  std::string out;
  for (PackDecision d : decisions) {
    if (force_report) d.mode = Mode::kReport;
    absl::StrAppend(&out, DecisionToJsonLine(host, path, d), "\n");
  }
  return out;
}

absl::StatusOr<CaseRun> RunCase(const std::string& fixtures_dir,
                                const FixtureCase& c) {
  auto req_text = ReadFile(c.dir + "/request.json");
  if (!req_text.ok()) return req_text.status();
  auto input = ReadFile(c.dir + "/input.html");
  if (!input.ok()) return input.status();

  nlohmann::json req = nlohmann::json::parse(*req_text, nullptr, false);
  if (!req.is_object() || !req.contains("url") || !req["url"].is_string()) {
    return absl::InvalidArgumentError(c.name + ": request.json needs a url");
  }
  CaseRun run;
  run.input_html = *input;
  run.url = req["url"].get<std::string>();
  const Mode global =
      req.value("mode", "enforce") == "report" ? Mode::kReport : Mode::kEnforce;
  const bool debug = req.value("debug", false);
  const std::string pack_file = req.value("pack", "pack.json");

  auto loaded = LoadPackFile(fixtures_dir + "/" + pack_file, "");
  if (!loaded.ok()) return loaded.status();
  auto pack = std::make_shared<const Pack>(*std::move(loaded));

  RunOptions opts;
  opts.global_mode = global;
  opts.debug_comments = debug;

  auto r1 = RunPackOnHtml(pack, run.url, run.input_html, opts);
  if (!r1.ok()) return r1.status();
  run.enforce = *std::move(r1);

  auto rt = RunPackOnHtml(nullptr, run.url, run.input_html, opts);
  if (!rt.ok()) return rt.status();
  run.roundtrip = *std::move(rt);

  RunOptions report = opts;
  report.global_mode = Mode::kReport;
  auto r2 = RunPackOnHtml(pack, run.url, run.input_html, report);
  if (!r2.ok()) return r2.status();
  run.report = *std::move(r2);

  auto r3 = RunPackOnHtml(pack, run.url, run.enforce.html, opts);
  if (!r3.ok()) return r3.status();
  run.idempotent = *std::move(r3);

  opts.variant = PassVariant::kOtherTransforms;
  auto r4 = RunPackOnHtml(pack, run.url, run.input_html, opts);
  if (!r4.ok()) return r4.status();
  run.other_transforms = *std::move(r4);

  opts.variant = PassVariant::kAllTransforms;
  auto r5 = RunPackOnHtml(pack, run.url, run.input_html, opts);
  if (!r5.ok()) return r5.status();
  run.all_transforms = *std::move(r5);
  return run;
}

absl::Status UpdateExpected(const FixtureCase& c, const CaseRun& run) {
  std::optional<PageUrl> page = ParsePageUrl(run.url);
  if (!page.has_value()) return absl::InvalidArgumentError("bad url");
  auto write = [](const std::string& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return out ? absl::OkStatus()
               : absl::InternalError(absl::StrCat("cannot write ", path));
  };
  if (auto s = write(c.dir + "/expected.html", run.enforce.html); !s.ok()) {
    return s;
  }
  return write(c.dir + "/expected.decisions.jsonl",
               DecisionsToJsonl(
                   page->host,
                   page->path + (page->query.empty() ? "" : "?" + page->query),
                   run.enforce.decisions, false));
}

}  // namespace pagespeed::packs
