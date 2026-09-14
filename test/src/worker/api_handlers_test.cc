// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Operational API Handlers Unit Tests
//
// Tests the route handlers for /v1/health, /v1/stats, /v1/metrics,
// /v1/config (GET + PATCH) using mock data.

#include "src/worker/api_handlers.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "nlohmann/json.hpp"
#include "src/product_version/version.h"
#include "src/worker/serve_stats.h"
#include "src/worker/shared_config.h"
#include "src/worker/worker.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

class ApiHandlersIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);

    config_ = std::make_shared<WorkerConfig>();
    config_->jpeg_quality = 85;
    config_->webp_quality = 75;
    config_->avif_quality = 60;
    config_->cache_path = "/tmp/test";

    // ApiContext holds a std::mutex (Issue #167) and is non-movable;
    // construct directly via new to skip the temporary + move.
    ctx_ = std::unique_ptr<ApiContext>(new ApiContext{
        .stats = stats_,
        .get_config = [this]() -> std::shared_ptr<const WorkerConfig> {
          return config_;
        },
        .update_config =
            [this](std::shared_ptr<const WorkerConfig> c) {
              config_ = std::const_pointer_cast<WorkerConfig>(c);
            },
        .http_active_connections = [] { return 5; },
        .in_flight_work = [] { return 2; },
        .cache_entries = [] { return uint64_t{450}; },
        .cache_bytes = [] { return uint64_t{314572800}; },
        .cache_degraded = [this] { return cache_degraded_; },
        .num_threads = 8,
        .max_connections = 128,
        .start_time =
            std::chrono::steady_clock::now() - std::chrono::seconds(3600),
    });

    HttpServerConfig server_config;
    server_config.port = 0;
    // An empty token now fails closed; these fixtures
    // exercise the routes, not the credential gate.
    server_config.allow_unauthenticated = true;
    handler_ = std::make_unique<NullMessageHandler>();
    server_ =
        std::make_unique<HttpServer>(loop_, server_config, handler_.get());
    RegisterOperationalRoutes(*server_, *ctx_);
    ASSERT_TRUE(server_->Start());
    StartLoopThread();
  }

  void TearDown() override {
    if (loop_thread_.joinable()) StopLoopThread();
    server_->Stop();
    uv_run(loop_, UV_RUN_DEFAULT);
    server_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

  void StartLoopThread() {
    uv_async_init(loop_, &stop_async_,
                  [](uv_async_t* handle) { uv_stop(handle->loop); });
    loop_thread_ = std::thread([this] { uv_run(loop_, UV_RUN_DEFAULT); });
  }

  void StopLoopThread() {
    uv_async_send(&stop_async_);
    if (loop_thread_.joinable()) loop_thread_.join();
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
  }

  std::string SendRequest(const std::string& request) {
    // The dispatcher CSRF-gates POST/PATCH to /v1 paths when no
    // API token is configured (the default in these integration tests),
    // requiring X-Requested-With: XMLHttpRequest (the workbench SPA always
    // sends it).  Inject it so handler-behavior tests reach the handler.
    std::string wire = test::InjectCsrfHeaderForV1Mutations(request);
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 2);

    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    (void)sent;
    test::SocketShutdown(sock, SHUT_WR);
    std::string response;
    char buf[4096];
    while (true) {
      ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      response.append(buf, n);
    }
    test::CloseSocket(sock);
    return response;
  }

  // Extract JSON body from HTTP response.
  json ParseJsonBody(const std::string& response) {
    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return {};
    return json::parse(response.substr(body_start + 4));
  }

  uv_loop_t* loop_ = nullptr;
  WorkerStats stats_;
  std::shared_ptr<WorkerConfig> config_;
  std::unique_ptr<ApiContext> ctx_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  uv_async_t stop_async_;
  bool cache_degraded_ = false;
};

TEST_F(ApiHandlersIntegrationTest, HealthEndpoint) {
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["status"], "ok");
  EXPECT_TRUE(j.contains("checks"));
  EXPECT_TRUE(j["checks"]["cache_open"]["pass"].get<bool>());
  EXPECT_TRUE(j["checks"]["cache_configured"]["pass"].get<bool>());
  EXPECT_TRUE(j["ready"].get<bool>());
  EXPECT_GE(j["uptime_seconds"].get<int64_t>(), 3599);
  EXPECT_EQ(j["connections"]["active"], 5);
  EXPECT_EQ(j["connections"]["max"], 128);
  EXPECT_EQ(j["inflight"], 2);
}

// The health document carries no license state — neither a
// "license" object nor a checks.license entry — and nothing in it spells
// "license" at all (an old console keyed on that word would find nothing).
TEST_F(ApiHandlersIntegrationTest, HealthCarriesNoLicenseState) {
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  json j = ParseJsonBody(resp);
  EXPECT_FALSE(j.contains("license"));
  EXPECT_FALSE(j["checks"].contains("license"));
  EXPECT_FALSE(j.contains("fastspring_storefront"));
  EXPECT_FALSE(j.contains("fastspring_product"));
  EXPECT_EQ(j.dump().find("\"license\""), std::string::npos) << j.dump();
  EXPECT_EQ(j.dump().find("license_key"), std::string::npos) << j.dump();
}

// GET /v1/config exposes none of the retired 2.0 licensing keys.
TEST_F(ApiHandlersIntegrationTest, ConfigGetCarriesNoLicensingKeys) {
  std::string resp =
      SendRequest("GET /v1/config HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  json j = ParseJsonBody(resp);
  EXPECT_FALSE(j.contains("license_key"));
  EXPECT_FALSE(j.contains("license_renewal_url"));
  EXPECT_FALSE(j.contains("fastspring_storefront"));
  EXPECT_FALSE(j.contains("fastspring_product"));
  // (rsl_cap_requested_license — the unrelated RSL-CAP setting — may appear.)
  EXPECT_EQ(j.dump().find("\"license\""), std::string::npos) << j.dump();
  EXPECT_EQ(j.dump().find("license_key"), std::string::npos) << j.dump();
}

// A stale console (or a hand-written body) that still PATCHes a
// 2.0 licensing key is answered, not failed — the key comes back under
// `rejected` with a reason naming the change, other fields still apply, and
// nothing license-shaped lands in the config or in pagespeed-shared.conf.
TEST_F(ApiHandlersIntegrationTest,
       ConfigPatchRetiredLicensingKeysRejectedNonFatal) {
  pagespeed::test::TempDir tmp;
  config_->cache_path = tmp.path() + "/cache.vol";
  std::string body =
      R"({"license_key": "abc.def.ghi", "license_renewal_url": "https://r.example", "disable_html": true})";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos) << resp;
  json j = ParseJsonBody(resp);
  ASSERT_TRUE(j["rejected"].contains("license_key")) << resp;
  ASSERT_TRUE(j["rejected"].contains("license_renewal_url")) << resp;
  EXPECT_NE(j["rejected"]["license_key"].get<std::string>().find("2.1"),
            std::string::npos);
  EXPECT_FALSE(j["applied"].contains("license_key"));
  EXPECT_TRUE(j["applied"].contains("disable_html"));
  EXPECT_TRUE(config_->disable_html);

  // The shared config this PATCH rewrote carries no license key of any kind.
  std::ifstream f(SharedConfigFilePath(config_->cache_path));
  ASSERT_TRUE(f.is_open());
  std::string raw((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  for (const char* key :
       {"\nlicense_key=", "\nlicense_valid=", "\nlicense_checked_once="}) {
    EXPECT_EQ(raw.find(key), std::string::npos) << key << "\n" << raw;
  }
  EXPECT_NE(raw.find("disable_html=true"), std::string::npos) << raw;
}

// The daemon's license endpoints are gone — they answer exactly
// like any other unknown route (404), auth-exempt or not.
TEST_F(ApiHandlersIntegrationTest, LicenseRoutesAreGone) {
  for (const char* path : {"/v1/license/activate", "/v1/license/consent",
                           "/v1/license/apply", "/v1/license/trial"}) {
    std::string body = R"({"license_key": "abc"})";
    std::string request = std::string("POST ") + path +
                          " HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Type: application/json\r\n"
                          "X-Requested-With: XMLHttpRequest\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string resp = SendRequest(request);
    EXPECT_NE(resp.find("404"), std::string::npos) << path << ": " << resp;
  }
  std::string resp =
      SendRequest("GET /v1/license/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos) << resp;
}

// The /v1/health "version" field must report the product version GENERATED
// from the workspace VERSION.txt by the //src/product_version:gen_product_version
// genrule (version.h: kPageSpeedVersion = kProductVersion). The genrule bakes
// VERSION.txt into the constant at build time and fails the build if VERSION.txt
// is missing or has no X.Y.Z core, so the constant cannot drift from VERSION.txt
// by construction. (A SemVer -prerelease suffix such as "2.0.21-preview.1" is
// permitted; +build-metadata is not — see the gen_product_version genrule.) The VERSION.txt <-> Directory.Build.props <-> git-tag anchors
// are enforced separately (samples/aspnetcore VersionInvariantTests and the
// release.yml "Verify VERSION.txt matches release tag" guard). This test guards
// the remaining link: that /v1/health reports that generated constant rather
// than a frozen literal — kPageSpeedVersion was a hand-edited "2.0.7" that
// drifted from VERSION.txt across releases 2.0.8..2.0.12.
//
// (Reads the constant, not VERSION.txt off disk: pulling the file in at runtime
// needs the Bazel runfiles library, whose header transitively includes a
// rules_cc header that clang-tidy's compile DB can't resolve — and the genrule
// already guarantees constant == VERSION.txt.)
TEST_F(ApiHandlersIntegrationTest, HealthVersionMatchesGeneratedConstant) {
  const std::string version(kPageSpeedVersion);

  // Sanity: the generated constant is a non-empty SemVer, proving the genrule
  // produced a real version, not an empty/garbage string. The core must be a
  // plain X.Y.Z (three numeric, dot-separated components); a SemVer prerelease
  // and/or build suffix ("-preview.1", "+meta") is permitted so 2.0 can ship
  // prerelease builds (e.g. 2.0.21-preview.1). The suffix begins at the first
  // '-' or '+'; what precedes it is the X.Y.Z core.
  ASSERT_FALSE(version.empty()) << "kPageSpeedVersion is empty";
  const size_t suffix_pos = version.find_first_of("-+");
  const std::string core = version.substr(0, suffix_pos);
  int dots = 0;
  bool core_digits_and_dots_only = true;
  for (char c : core) {
    if (c == '.') {
      ++dots;
    } else if (c < '0' || c > '9') {
      core_digits_and_dots_only = false;
    }
  }
  EXPECT_TRUE(core_digits_and_dots_only && dots == 2)
      << "kPageSpeedVersion '" << version << "' core is not SemVer X.Y.Z";
  if (suffix_pos != std::string::npos) {
    // The prerelease/build suffix may contain only [0-9A-Za-z.+-] per SemVer.
    const std::string suffix = version.substr(suffix_pos);
    const std::string allowed =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.+-";
    EXPECT_EQ(suffix.find_first_not_of(allowed), std::string::npos)
        << "kPageSpeedVersion '" << version
        << "' has an invalid prerelease/build suffix";
  }

  // /v1/health must report that same generated version, not a frozen literal.
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["version"].get<std::string>(), version);
}

TEST_F(ApiHandlersIntegrationTest, StatsEndpoint) {
  // Set some stats.
  stats_.notifications_received.store(100);
  stats_.variants_written.store(50);
  stats_.webp_generated.store(30);
  stats_.errors.store(2);
  stats_.svg_vectorized.store(7);
  stats_.svg_bytes_saved.store(12345);
  stats_.image_no_savings_skipped.store(3);
  stats_.image_unconverted_fallthrough.store(4);

  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["notifications"]["received"], 100);
  EXPECT_EQ(j["variants"]["written"], 50);
  EXPECT_EQ(j["by_format"]["webp"], 30);
  EXPECT_EQ(j["errors"]["total"], 2);
  EXPECT_EQ(j["errors"]["origin_misconfiguration"], 0);
  EXPECT_EQ(j["cache"]["entries"], 450);
  EXPECT_EQ(j["cache"]["size_bytes"], 314572800);
  EXPECT_EQ(j["thread_pool"]["size"], 8);

  // Image no-savings skipped counter, and the distinct "nothing to convert"
  // counter beside it (#1374): an operator must be able to tell the two apart.
  EXPECT_EQ(j["image_no_savings_skipped"], 3);
  EXPECT_EQ(j["image_unconverted_fallthrough"], 4);

  // SVG auto-vectorization stats
  EXPECT_TRUE(j.contains("svg"));
  EXPECT_EQ(j["svg"]["vectorized"], 7);
  EXPECT_EQ(j["svg"]["bytes_saved"], 12345);
  EXPECT_EQ(j["svg"]["candidates_evaluated"], 0);
  EXPECT_EQ(j["svg"]["candidates_rejected"], 0);
  EXPECT_EQ(j["svg"]["fidelity_rejected"], 0);
  EXPECT_EQ(j["svg"]["size_rejected"], 0);
  EXPECT_EQ(j["svg"]["path_count_rejected"], 0);
  EXPECT_EQ(j["svg"]["written"], 0);
  EXPECT_EQ(j["svg"]["vectorize_time_us"], 0);
  EXPECT_EQ(j["svg"]["served"], 0);
}

// Regression guard for the WebSocket /v1/ws/stats drift bug.
//
// The live dashboard is fed by the WS stats stream, which used to be a SECOND
// hand-maintained serializer (Worker::SetStatsProvider) that omitted the
// svg / policy / quality_baselining groups and several cache-reliability
// counters, and emitted `errors` as a bare scalar.  Result: the dashboard
// perpetually rendered 0 for those fields (e.g. SVG "Candidates Evaluated")
// while the REST-fed Metrics page was correct.
//
// Both the REST handler and the WS provider now call BuildStatsJson(), so
// asserting the full contract here covers REST *and* WebSocket and fails if
// anyone reintroduces a partial serializer.
TEST_F(ApiHandlersIntegrationTest, BuildStatsJsonExposesCompleteContract) {
  stats_.svg_candidates_evaluated.store(14);
  stats_.svg_candidates_rejected.store(7);
  stats_.svg_vectorized.store(3);
  stats_.policy_computed.store(5);
  stats_.policy_async_css_enabled.store(2);
  stats_.async_css_suppressed_low_coverage.store(3);
  stats_.async_css_suppressed_unvalidated.store(17);
  stats_.async_css_record_dropped_empty_derivation.store(23);
  stats_.policy_script_deferral_enabled.store(1);
  stats_.quality_capped_jpeg.store(4);
  stats_.quality_skip_reencode.store(6);
  stats_.cache_read_deferred_retries.store(9);
  stats_.cache_read_deferred_successes.store(8);
  stats_.cache_auto_heals.store(1);
  stats_.cache_auto_heal_exhausted.store(2);
  stats_.image_incomplete_matrices.store(11);
  stats_.image_no_savings_skipped.store(12);
  stats_.image_unconverted_fallthrough.store(14);
  stats_.ssimulacra2_declines.store(19);
  stats_.ssimulacra2_decline_tombstone_hits.store(7);
  stats_.errors.store(13);
  stats_.errors_origin_misconfiguration.store(3);

  json j = BuildStatsJson(*ctx_);

  // svg group: the field that was visibly broken (0) on the live dashboard.
  ASSERT_TRUE(j.contains("svg"))
      << "WS-fed dashboard lost the entire svg group";
  EXPECT_EQ(j["svg"]["candidates_evaluated"], 14);
  EXPECT_EQ(j["svg"]["candidates_rejected"], 7);
  EXPECT_EQ(j["svg"]["vectorized"], 3);

  // policy + quality_baselining groups (also WS-omitted).
  ASSERT_TRUE(j.contains("policy"));
  EXPECT_EQ(j["policy"]["computed"], 5);
  EXPECT_EQ(j["policy"]["async_css_enabled"], 2);
  EXPECT_EQ(j["policy"]["async_css_suppressed_low_coverage"], 3);
  EXPECT_EQ(j["policy"]["async_css_suppressed_unvalidated"], 17);
  EXPECT_EQ(j["policy"]["async_css_record_dropped_empty_derivation"], 23);
  EXPECT_EQ(j["policy"]["script_deferral_enabled"], 1);
  ASSERT_TRUE(j.contains("quality_baselining"));
  EXPECT_EQ(j["quality_baselining"]["capped_jpeg"], 4);
  EXPECT_EQ(j["quality_baselining"]["skip_reencode"], 6);

  // cache-reliability + image counters (WS-omitted).
  EXPECT_EQ(j["cache_read_deferred_retries"], 9);
  EXPECT_EQ(j["cache_read_deferred_successes"], 8);
  EXPECT_EQ(j["cache_auto_heals"], 1);
  EXPECT_EQ(j["cache_auto_heal_exhausted"], 2);
  EXPECT_EQ(j["image_incomplete_matrices"], 11);
  EXPECT_EQ(j["image_no_savings_skipped"], 12);
  EXPECT_EQ(j["image_unconverted_fallthrough"], 14);

  // Declines: a verify refusal is a decision with its own counter (mpp
  // #790), never an error and never part of the score average. The
  // tombstone hits mirror it (#1382): a skipped recompute is a decision
  // too.
  ASSERT_TRUE(j.contains("ssimulacra2"));
  EXPECT_EQ(j["ssimulacra2"]["declines"], 19);
  EXPECT_EQ(j["ssimulacra2"]["tombstone_hits"], 7);

  // errors must be an OBJECT {total, origin_misconfiguration}, not a scalar:
  // the dashboard reads stats.errors.total.
  ASSERT_TRUE(j["errors"].is_object())
      << "errors must be an object; the WS provider used to emit a scalar";
  EXPECT_EQ(j["errors"]["total"], 13);
  EXPECT_EQ(j["errors"]["origin_misconfiguration"], 3);
}

// Issue E: the benign purge-fence counter and the origin-refresh group must be
// serialized so the dashboard can show fenced writes distinctly from hard
// failures and surface origin-refresh purge activity.
TEST_F(ApiHandlersIntegrationTest,
       BuildStatsJsonExposesFencedAndOriginRefresh) {
  stats_.alternate_writes.store(40);
  stats_.alternate_write_failures.store(2);
  stats_.alternate_writes_fenced.store(4);
  stats_.origin_refresh_purges.store(3);
  stats_.origin_refresh_rate_limited.store(1);

  json j = BuildStatsJson(*ctx_);

  // The benign fence counter lives alongside writes/write_failures so the
  // dashboard can show "fenced" separately from real failures.
  ASSERT_TRUE(j.contains("alternates"));
  EXPECT_EQ(j["alternates"]["writes"], 40);
  EXPECT_EQ(j["alternates"]["write_failures"], 2);
  EXPECT_EQ(j["alternates"]["writes_fenced"], 4)
      << "alternates.writes_fenced must be serialized (Issue E)";

  // The origin_refresh group (incremented in HandleOriginRefreshed) was never
  // serialized.
  ASSERT_TRUE(j.contains("origin_refresh"))
      << "origin_refresh group must be serialized (Issue E)";
  EXPECT_EQ(j["origin_refresh"]["purges"], 3);
  EXPECT_EQ(j["origin_refresh"]["rate_limited"], 1);
}

TEST_F(ApiHandlersIntegrationTest, MetricsEndpoint) {
  stats_.notifications_received.store(42);
  stats_.webp_generated.store(10);

  std::string resp =
      SendRequest("GET /v1/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("text/plain"), std::string::npos);
  EXPECT_NE(resp.find("pagespeed_notifications_total 42"), std::string::npos);
  EXPECT_NE(resp.find("pagespeed_format_generated_total{format=\"webp\"} 10"),
            std::string::npos);
  EXPECT_NE(resp.find("pagespeed_cache_entries"), std::string::npos);

  // Image no-savings skipped metric, and the fall-through refusal metric
  EXPECT_NE(resp.find("pagespeed_image_no_savings_skipped_total"),
            std::string::npos);
  EXPECT_NE(resp.find("pagespeed_image_unconverted_fallthrough_total"),
            std::string::npos);

  // SVG metrics present
  EXPECT_NE(resp.find("pagespeed_svg_candidates_evaluated_total"),
            std::string::npos);
  EXPECT_NE(resp.find("pagespeed_svg_vectorized_total"), std::string::npos);
  EXPECT_NE(resp.find("pagespeed_svg_vectorize_time_seconds_total"),
            std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, ConfigGetEndpoint) {
  std::string resp =
      SendRequest("GET /v1/config HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["jpeg_quality"], 85);
  EXPECT_EQ(j["webp_quality"], 75);
  EXPECT_EQ(j["avif_quality"], 60);
  EXPECT_EQ(j["disable_html"], false);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchAppliesFields) {
  std::string body = R"({"avif_quality": 20, "target_ssimulacra2": 75.0})";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["applied"]["avif_quality"], 20);
  EXPECT_FLOAT_EQ(j["applied"]["target_ssimulacra2"].get<float>(), 75.0f);
  EXPECT_TRUE(j["rejected"].empty());

  // Verify the config was actually updated.
  EXPECT_EQ(config_->avif_quality, 20);
  EXPECT_FLOAT_EQ(config_->target_ssimulacra2, 75.0f);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchRejectsNonReloadable) {
  std::string body = R"({"num_threads": 16, "jpeg_quality": 90})";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  json j = ParseJsonBody(resp);
  EXPECT_FALSE(j["rejected"].empty());
  EXPECT_TRUE(j["rejected"].contains("num_threads"));
  EXPECT_EQ(j["applied"]["jpeg_quality"], 90);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchClampsQuality) {
  std::string body = "{\"jpeg_quality\": 200}";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  json j = ParseJsonBody(resp);
  // Should be clamped to 100.
  EXPECT_EQ(j["applied"]["jpeg_quality"], 100);
  EXPECT_EQ(config_->jpeg_quality, 100);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchWarnsHighSsimulacra2) {
  std::string body = "{\"target_ssimulacra2\": 95.0}";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  json j = ParseJsonBody(resp);
  EXPECT_FALSE(j["warnings"].empty());
  EXPECT_NE(j["warnings"][0].get<std::string>().find("compression"),
            std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchInvalidJson) {
  std::string body = "not json";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchViewportValidation) {
  std::string body = "{\"mobile_width\": 50}";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  json j = ParseJsonBody(resp);
  EXPECT_TRUE(j["rejected"].contains("mobile_width"));
  // Original config unchanged.
  EXPECT_EQ(config_->mobile_width, 480u);
}

// =============================================================================
// Additional Coverage Tests
// =============================================================================

TEST_F(ApiHandlersIntegrationTest, ConfigPatchPreservesWebBotAuthSharedFields) {
  // Regression: the PATCH handler rebuilds
  // pagespeed-shared.conf.  It must READ-MODIFY-WRITE so fields owned by
  // other writers — the web_bot_auth trio and the RSL-CAP toggle, written
  // at worker startup — survive an unrelated runtime PATCH instead of being
  // silently reset (which disabled the feature in nginx until restart).
  pagespeed::test::TempDir tmp;
  config_->cache_path = tmp.path() + "/cache.vol";

  SharedConfig seeded;
  seeded.web_bot_auth = true;
  seeded.web_bot_auth_verified_bots = "kid1=crawler";
  seeded.web_bot_auth_directory_hosts = "keys.example.com";
  seeded.rsl_cap_enforcement = true;
  std::string shared_path = SharedConfigFilePath(config_->cache_path);
  ASSERT_TRUE(WriteSharedConfigFile(shared_path, seeded));

  // PATCH a shared-but-unrelated key (disable_html) to trigger the rewrite.
  std::string body = R"({"disable_html": true})";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  json j = ParseJsonBody(resp);
  ASSERT_TRUE(j["applied"].contains("disable_html"));

  SharedConfig out = ReadSharedConfigFile(shared_path);
  EXPECT_TRUE(out.disable_html);  // the PATCH took effect
  // ... and the fields this handler does not own survived.
  EXPECT_TRUE(out.web_bot_auth);
  EXPECT_EQ(out.web_bot_auth_verified_bots, "kid1=crawler");
  EXPECT_EQ(out.web_bot_auth_directory_hosts, "keys.example.com");
  EXPECT_TRUE(out.rsl_cap_enforcement);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchDeeplyNestedJson) {
  // Build a deeply nested JSON body (exceeds limit of 32).
  std::string body;
  for (int i = 0; i < 40; ++i) body += "{\"a\":";
  body += "1";
  for (int i = 0; i < 40; ++i) body += "}";

  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("nesting"), std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchNonObjectJson) {
  // Array body should be rejected.
  std::string body = "[1, 2, 3]";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("JSON object"), std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchStringBody) {
  // String body should be rejected (not an object).
  std::string body = R"("just a string")";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchEmptyBody) {
  // Empty object should succeed but not change anything.
  std::string body = "{}";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_TRUE(j["applied"].empty());
  EXPECT_TRUE(j["rejected"].empty());
  EXPECT_TRUE(j["warnings"].empty());
}

TEST_F(ApiHandlersIntegrationTest, StatsEndpointNoServeSavingsWhenNull) {
  // serve_stats defaults to nullptr — serve_savings should be absent.
  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_FALSE(j.contains("serve_savings"));
  // The serve-class block rides the same mmap, so it is absent with it.
  EXPECT_FALSE(j.contains("serve_classes"));
}

TEST_F(ApiHandlersIntegrationTest, StatsJsonExposesServeClasses) {
  // The v8 serve-class partition: the counters the 1.16 module's serve-side
  // writer moves in the module-serves topology (mpp #891).  Emitted whenever
  // the serve-stats mmap is mapped, so a reader can tell "not instrumented"
  // (all zero, worker_pool_threads live) from "no front end" (block absent).
  ServeStats ss{};
  ss.magic = ServeStats::kMagic;
  ss.version = ServeStats::kVersion;
  ss.serve_optimized_total = 41;
  ss.serve_original_cold_total = 7;
  ss.serve_original_pending_total = 12;
  ss.serve_original_declined_total = 0;
  ss.serve_original_skew_total = 1;
  ss.notify_suppressed_total = 3;
  ss.serve_class_unrecognized_total = 2;
  ss.serve_flags_unrecognized_total = 5;
  ctx_->serve_stats = &ss;

  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  ASSERT_TRUE(j.contains("serve_classes"));
  auto& sc = j["serve_classes"];
  EXPECT_EQ(sc["optimized"], 41);
  EXPECT_EQ(sc["original_cold"], 7);
  EXPECT_EQ(sc["original_pending"], 12);
  EXPECT_EQ(sc["original_declined"], 0);
  EXPECT_EQ(sc["original_skew"], 1);
  EXPECT_EQ(sc["notify_suppressed"], 3);
  EXPECT_EQ(sc["class_unrecognized"], 2);
  EXPECT_EQ(sc["flags_unrecognized"], 5);
}

TEST_F(ApiHandlersIntegrationTest, StatsEndpointWithServeSavings) {
  // Create a stack-allocated ServeStats and wire it into the context.
  ServeStats ss{};
  ss.magic = ServeStats::kMagic;
  ss.version = ServeStats::kVersion;
  ss.html_original_bytes = 100000;
  ss.html_optimized_bytes = 80000;
  ss.html_optimized_hits = 50;
  ss.css_original_bytes = 50000;
  ss.css_optimized_bytes = 30000;
  ss.css_optimized_hits = 25;
  ss.js_original_bytes = 200000;
  ss.js_optimized_bytes = 60000;
  ss.js_optimized_hits = 10;
  ss.image_original_bytes = 500000;
  ss.image_optimized_bytes = 100000;
  ss.image_optimized_hits = 100;
  ctx_->serve_stats = &ss;

  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  ASSERT_TRUE(j.contains("serve_savings"));

  auto& sv = j["serve_savings"];
  EXPECT_EQ(sv["html"]["original_bytes"], 100000);
  EXPECT_EQ(sv["html"]["optimized_bytes"], 80000);
  EXPECT_EQ(sv["html"]["hits"], 50);
  EXPECT_EQ(sv["css"]["original_bytes"], 50000);
  EXPECT_EQ(sv["css"]["optimized_bytes"], 30000);
  EXPECT_EQ(sv["css"]["hits"], 25);
  EXPECT_EQ(sv["js"]["original_bytes"], 200000);
  EXPECT_EQ(sv["js"]["optimized_bytes"], 60000);
  EXPECT_EQ(sv["js"]["hits"], 10);
  EXPECT_EQ(sv["image"]["original_bytes"], 500000);
  EXPECT_EQ(sv["image"]["optimized_bytes"], 100000);
  EXPECT_EQ(sv["image"]["hits"], 100);
}

TEST_F(ApiHandlersIntegrationTest, MetricsEndpointWithServeSavings) {
  ServeStats ss{};
  ss.magic = ServeStats::kMagic;
  ss.version = ServeStats::kVersion;
  ss.html_original_bytes = 100000;
  ss.html_optimized_bytes = 80000;
  ss.html_optimized_hits = 50;
  ss.webbotauth_signed_verified = 7;
  ss.webbotauth_signed_invalid = 3;
  ss.webbotauth_other_signature = 2;
  ctx_->serve_stats = &ss;

  std::string resp =
      SendRequest("GET /v1/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(
      resp.find("pagespeed_original_bytes_served_total{type=\"html\"} 100000"),
      std::string::npos);
  EXPECT_NE(
      resp.find("pagespeed_optimized_bytes_served_total{type=\"html\"} 80000"),
      std::string::npos);
  EXPECT_NE(
      resp.find("pagespeed_optimized_hits_served_total{type=\"html\"} 50"),
      std::string::npos);
  // Web Bot Auth verdict counters ride the same serve-stats block.
  EXPECT_NE(
      resp.find(
          "pagespeed_webbotauth_signed_requests_total{result=\"verified\"} 7"),
      std::string::npos);
  EXPECT_NE(
      resp.find(
          "pagespeed_webbotauth_signed_requests_total{result=\"invalid\"} 3"),
      std::string::npos);
  EXPECT_NE(
      resp.find(
          "pagespeed_webbotauth_signed_requests_total{result=\"other\"} 2"),
      std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, MetricsExposesZerocopyAndSwrCounters) {
  ServeStats ss{};
  ss.magic = ServeStats::kMagic;
  ss.version = ServeStats::kVersion;
  ss.zerocopy_torn_aborts = 4;
  ss.zerocopy_proactive_copyouts = 9;
  ss.zerocopy_copy_then_verify_discards = 1;
  ss.swr_coalesced_serves = 6;
  ss.stale_if_error_serves = 2;
  ctx_->serve_stats = &ss;

  std::string resp =
      SendRequest("GET /v1/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("pagespeed_zerocopy_torn_aborts_total 4"),
            std::string::npos);
  EXPECT_NE(resp.find("pagespeed_zerocopy_proactive_copyouts_total 9"),
            std::string::npos);
  EXPECT_NE(resp.find("pagespeed_zerocopy_copy_then_verify_discards_total 1"),
            std::string::npos);
  EXPECT_NE(resp.find("pagespeed_swr_coalesced_serves_total 6"),
            std::string::npos);
  EXPECT_NE(resp.find("pagespeed_stale_if_error_serves_total 2"),
            std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, StatsJsonExposesZerocopyAndSwrCounters) {
  ServeStats ss{};
  ss.magic = ServeStats::kMagic;
  ss.version = ServeStats::kVersion;
  ss.zerocopy_torn_aborts = 4;
  ss.zerocopy_proactive_copyouts = 9;
  ss.zerocopy_copy_then_verify_discards = 1;
  ss.swr_coalesced_serves = 6;
  ss.stale_if_error_serves = 2;
  ctx_->serve_stats = &ss;

  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  ASSERT_TRUE(j.contains("zerocopy"));
  EXPECT_EQ(j["zerocopy"]["torn_aborts"], 4);
  EXPECT_EQ(j["zerocopy"]["proactive_copyouts"], 9);
  EXPECT_EQ(j["zerocopy"]["copy_then_verify_discards"], 1);
  ASSERT_TRUE(j.contains("stale_serves"));
  EXPECT_EQ(j["stale_serves"]["swr_coalesced"], 6);
  EXPECT_EQ(j["stale_serves"]["stale_if_error"], 2);
}

// The HTTP and management-socket metrics surfaces share one builder
// (BuildPrometheusMetricsText, #877) — they previously drifted in both
// directions. These tests pin the merged union on the shared builder
// directly, which covers BOTH surfaces by construction.
TEST_F(ApiHandlersIntegrationTest, MetricsSurfacesShareOneBuilder) {
  stats_.html_assembly_complete.store(4);
  stats_.html_assembly_skipped.store(1);
  stats_.critical_css_aborted.store(2);
  stats_.origin_refresh_purges.store(9);

  ServeStats ss{};
  ss.magic = ServeStats::kMagic;
  ss.version = ServeStats::kVersion;
  ss.webbotauth_signed_verified = 5;

  PrometheusMetricsInputs in{
      .stats = stats_,
      .cache_entries = 11,
      .cache_bytes = 2048,
      .active_connections = 1,
      .max_connections = 100,
      .in_flight_work = 3,
      .serve_stats = &ss,
      .web_bot_auth_verified_bots = "",
  };
  std::string m = BuildPrometheusMetricsText(in);

  // Series that were HTTP-only before the merge (the socket surface lacked
  // them): per-type processed counts, origin-refresh, policy.
  EXPECT_NE(m.find("pagespeed_processed_total{type=\"html\"}"),
            std::string::npos);
  EXPECT_NE(m.find("pagespeed_origin_refresh_purges_total 9"),
            std::string::npos);
  EXPECT_NE(m.find("pagespeed_async_css_suppressed_low_coverage_total"),
            std::string::npos);
  EXPECT_NE(m.find("pagespeed_async_css_suppressed_unvalidated_total"),
            std::string::npos);
  EXPECT_NE(m.find("pagespeed_async_css_record_dropped_empty_derivation_total"),
            std::string::npos);
  // Series that were socket-only before the merge (the HTTP surface lacked
  // them): html-assembly outcomes.
  EXPECT_NE(m.find("pagespeed_html_assembly_total{result=\"complete\"} 4"),
            std::string::npos);
  EXPECT_NE(m.find("pagespeed_html_assembly_total{result=\"skipped\"} 1"),
            std::string::npos);
  EXPECT_NE(m.find("pagespeed_html_assembly_total{result=\"css_aborted\"} 2"),
            std::string::npos);
  // The #871 regression class: webbotauth verdicts present via serve-stats.
  EXPECT_NE(
      m.find("pagespeed_webbotauth_signed_requests_total{result=\"verified\"} "
             "5"),
      std::string::npos);
  // Gauges flow from the inputs struct.
  EXPECT_NE(m.find("pagespeed_cache_entries 11"), std::string::npos);
  EXPECT_NE(m.find("pagespeed_thread_pool_inflight 3"), std::string::npos);
  // No browser manager wired -> browser series omitted entirely.
  EXPECT_EQ(m.find("pagespeed_browser_"), std::string::npos);

  // Omitting serve_stats drops the serve-time series (both surfaces).
  in.serve_stats = nullptr;
  std::string no_ss = BuildPrometheusMetricsText(in);
  EXPECT_EQ(no_ss.find("pagespeed_webbotauth_signed_requests_total"),
            std::string::npos);
  EXPECT_EQ(no_ss.find("pagespeed_original_bytes_served_total"),
            std::string::npos);
}

TEST_F(ApiHandlersIntegrationTest, MetricsEndpointIncludesHtmlAssembly) {
  // Regression guard for the merge direction the HTTP surface was missing:
  // /v1/metrics must now expose the html-assembly series (previously
  // socket-only).
  stats_.html_assembly_complete.store(6);
  std::string resp =
      SendRequest("GET /v1/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("pagespeed_html_assembly_total{result=\"complete\"} 6"),
            std::string::npos);
}

// =============================================================================
// Health Status Derivation Tests
// =============================================================================

TEST_F(ApiHandlersIntegrationTest, HealthStatusOk) {
  // Default fixture: cache configured, not degraded.
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["status"], "ok");
  EXPECT_TRUE(j["checks"]["cache_open"]["pass"].get<bool>());
  EXPECT_TRUE(j["checks"]["cache_configured"]["pass"].get<bool>());
  EXPECT_FALSE(j["checks"].contains("license"));
}

TEST_F(ApiHandlersIntegrationTest, HealthStatusCacheDegraded) {
  cache_degraded_ = true;
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["status"], "degraded");
  EXPECT_FALSE(j["checks"]["cache_open"]["pass"].get<bool>());
  EXPECT_EQ(j["checks"]["cache_open"]["detail"], "cache failed to open");
  // Other checks still pass.
  EXPECT_TRUE(j["checks"]["cache_configured"]["pass"].get<bool>());
  EXPECT_FALSE(j["checks"].contains("license"));
}

TEST_F(ApiHandlersIntegrationTest, HealthStatusNoCacheConfigured) {
  config_->cache_path.clear();
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["status"], "degraded");
  EXPECT_TRUE(j["checks"]["cache_open"]["pass"].get<bool>());
  EXPECT_FALSE(j["checks"]["cache_configured"]["pass"].get<bool>());
  EXPECT_EQ(j["checks"]["cache_configured"]["detail"],
            "no cache path configured");
  EXPECT_FALSE(j["checks"].contains("license"));
}

TEST_F(ApiHandlersIntegrationTest, HealthStatusCacheDegradedAndUnconfigured) {
  // Both cache checks fail independently.
  cache_degraded_ = true;
  config_->cache_path.clear();
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["status"], "degraded");
  EXPECT_FALSE(j["checks"]["cache_open"]["pass"].get<bool>());
  EXPECT_FALSE(j["checks"]["cache_configured"]["pass"].get<bool>());
  EXPECT_FALSE(j["checks"].contains("license"));
}

TEST_F(ApiHandlersIntegrationTest, HealthStatusNullCacheDegradedCallable) {
  // When cache_degraded is null (no cache subsystem), cache_open still passes.
  ctx_->cache_degraded = nullptr;
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["status"], "ok");
  EXPECT_TRUE(j["checks"]["cache_open"]["pass"].get<bool>());
  EXPECT_TRUE(j["checks"]["cache_configured"]["pass"].get<bool>());
  EXPECT_FALSE(j["checks"].contains("license"));
}

// =============================================================================
// API3: PATCH /v1/config — mutex prevents lost updates
// =============================================================================

TEST_F(ApiHandlersIntegrationTest, ConfigPatchSequentialUpdatesPreserved) {
  // Fire two PATCH requests from separate threads. The event loop
  // serialises them; verify both changes are applied correctly.
  auto send_patch = [this](const std::string& field, int value) {
    std::string body = "{\"" + field + "\": " + std::to_string(value) + "}";
    std::string request =
        "PATCH /v1/config HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    return SendRequest(request);
  };

  std::string r1, r2;
  std::thread t1([&] { r1 = send_patch("jpeg_quality", 42); });
  std::thread t2([&] { r2 = send_patch("webp_quality", 33); });
  t1.join();
  t2.join();

  EXPECT_NE(r1.find("200 OK"), std::string::npos);
  EXPECT_NE(r2.find("200 OK"), std::string::npos);

  // Both fields must reflect the last value written.
  EXPECT_EQ(config_->jpeg_quality, 42);
  EXPECT_EQ(config_->webp_quality, 33);
}

// =============================================================================
// API5: Content-Type validation — 415 on non-JSON POST/PATCH
// =============================================================================

TEST_F(ApiHandlersIntegrationTest, ConfigPatchRejectsWrongContentType) {
  std::string body = "{\"jpeg_quality\": 50}";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("415"), std::string::npos);
  EXPECT_NE(resp.find("UNSUPPORTED_MEDIA_TYPE"), std::string::npos);
  // Config unchanged.
  EXPECT_EQ(config_->jpeg_quality, 85);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchAcceptsJsonContentType) {
  std::string body = "{\"jpeg_quality\": 50}";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json; charset=utf-8\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_EQ(config_->jpeg_quality, 50);
}

TEST_F(ApiHandlersIntegrationTest, ConfigPatchNoContentTypeHeader) {
  // Missing Content-Type header on a POST/PATCH with body → 415.
  std::string body = "{\"jpeg_quality\": 50}";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("415"), std::string::npos);
}

// The refusal counters have to be READABLE, not just incremented: an operator
// diagnosing "the cache never warms" reaches for the stats surface long before
// the logs, and a counter that exists only in the process is not a diagnostic.
TEST_F(ApiHandlersIntegrationTest, StatsExposesNotificationRefusalsByCause) {
  stats_.notifications_rejected_version.store(3);
  stats_.notifications_rejected_malformed.store(5);
  stats_.notifications_rejected_sentinel.store(7);
  stats_.notifications_rejected_option_context.store(11);
  // Not a refusal, and on the same surface for the same reason: an operator
  // asking "is my front end sending options contexts, and where is that work
  // landing" has nowhere else to look.
  stats_.notifications_accepted_non_default_option_context.store(13);

  json j = ParseJsonBody(
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  EXPECT_EQ(j["notifications"]["rejected_version"], 3);
  EXPECT_EQ(j["notifications"]["rejected_malformed"], 5);
  EXPECT_EQ(j["notifications"]["rejected_sentinel"], 7);
  EXPECT_EQ(j["notifications"]["rejected_option_context"], 11);
  EXPECT_EQ(j["notifications"]["accepted_non_default_option_context"], 13);

  std::string metrics =
      SendRequest("GET /v1/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(metrics.find("pagespeed_notifications_rejected_version_total 3"),
            std::string::npos);
  EXPECT_NE(metrics.find("pagespeed_notifications_rejected_malformed_total 5"),
            std::string::npos);
  EXPECT_NE(metrics.find("pagespeed_notifications_rejected_sentinel_total 7"),
            std::string::npos);
  EXPECT_NE(
      metrics.find("pagespeed_notifications_rejected_option_context_total 11"),
      std::string::npos);
  EXPECT_NE(metrics.find("pagespeed_notifications_accepted_non_default_option_"
                         "context_total 13"),
            std::string::npos);
}

// The shared-config version mismatch reaches the health surface, which is what
// a support bundle carries.  The log line is the signal at the moment it
// happens; this is the signal an hour later, and the mismatch has to be
// legible from either.
TEST_F(ApiHandlersIntegrationTest, HealthReportsSharedConfigVersionMismatch) {
  // Clean surface first: the record is process-wide, so assert the quiet
  // state before dirtying it.  This half also pins that the new check does
  // not spuriously degrade a healthy install.
  ResetSharedConfigVersionSkewForTesting();
  // The assertions below can throw (nlohmann .get on a missing key), so the
  // restore must not depend on reaching the last line — otherwise one failed
  // assertion leaves every later health test in this binary degraded.
  struct SkewStateGuard {
    ~SkewStateGuard() { ResetSharedConfigVersionSkewForTesting(); }
  } skew_guard;
  {
    json j = ParseJsonBody(
        SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_TRUE(j["checks"].contains("shared_config_version"));
    EXPECT_TRUE(j["checks"]["shared_config_version"]["pass"].get<bool>());
    EXPECT_FALSE(j["checks"]["shared_config_version"].contains("detail"));
  }

  // Drive a real parse of an unreadable schema rather than poking the state
  // directly — the surface must reflect what the parser actually did.
  SetSharedConfigMessageHandler(nullptr);
  ParseSharedConfig("version=42\nsocket_path=/from-a-newer-peer.sock\n");

  json j = ParseJsonBody(
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  EXPECT_FALSE(j["checks"]["shared_config_version"]["pass"].get<bool>());
  const std::string detail =
      j["checks"]["shared_config_version"]["detail"].get<std::string>();
  EXPECT_NE(detail.find("42"), std::string::npos) << detail;
  EXPECT_NE(detail.find(std::to_string(kSharedConfigVersion)),
            std::string::npos)
      << detail;
  // Running on defaults is a degradation of the install, not a healthy state.
  EXPECT_EQ(j["status"], "degraded");
}

}  // namespace
}  // namespace pagespeed
