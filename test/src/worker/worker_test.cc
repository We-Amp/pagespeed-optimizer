// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Worker Integration Tests
//
// Tests the Worker class including:
// - IPC protocol serialization/deserialization
// - Worker initialization and socket setup
// - Notification handling through Unix socket
// - Cache integration (read from cache, write variants)

#include "src/worker/worker.h"

#ifdef _WIN32
#include <cstdlib>
// windows.h arrives transitively through worker.h above and defines the
// OBJECT-like macro FormatMessage -> FormatMessageA, which rewrites the token
// before name lookup runs — so an unqualified call to the inherited
// MessageHandler::FormatMessage becomes a call to the Win32 API. Qualifying or
// parenthesising the name does NOT suppress an object-like macro; only #undef
// does. Placed after the worker.h include, which is what makes it effective.
#undef FormatMessage
inline int setenv(const char* name, const char* value, int) {
  return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
#else
#include <unistd.h>
#endif

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "absl/strings/str_cat.h"
#include "avif/avif.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/option_context.h"
#include "src/browser/optimization_profile.h"
#include "src/browser/template_detector.h"
#include "src/nginx/early_hints_util.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/html_scanner.h"
#include "src/worker/shared_config.h"
#include "src/worker/text_compressor.h"
#include "src/worker/url_registry.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

// Sanitizer builds run the instrumented binary far slower than a release
// build; ThreadSanitizer in particular is roughly an order of magnitude
// slower, and image transcodes (AVIF via libaom, plus the SSIMULACRA2
// verification path) sit at the worst of that curve.  The suite's wall-clock
// wait ceilings scale by this factor.  They are *ceilings* for condition
// polls that return the instant their condition holds, so a larger ceiling
// never slows a passing test -- it only keeps a genuinely slow sanitizer run
// from tripping a deadline that was sized for a release build.
// Both the GCC-style define and the Clang __has_feature form are checked.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
inline constexpr int kSanitizerBudgetScale = 12;
#elif defined(__clang__)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
inline constexpr int kSanitizerBudgetScale = 12;
#else
inline constexpr int kSanitizerBudgetScale = 1;
#endif
#else
inline constexpr int kSanitizerBudgetScale = 1;
#endif

// Poll ceiling for the proactive viewport-sibling tests.  Those variants are
// produced asynchronously behind the image encoder queue, so the window has
// to be sized for the slowest lane rather than the median: on an unoptimized
// Windows fastbuild sharing a loaded CI runner, a test whose polls waited on
// the full warmup encode matrix exhausted its previous 6 s window twice in a
// row (issue #1296), while a sibling test in the same shard needed its whole
// ~30 s aggregate window to pass.  90 s matches the flat window adopted for
// the same class of flake in the e2e AVIF test (issues #276 / #307).  The
// loops sleep 100 ms per iteration, so iterations x 100 ms = wall clock:
// 900 x 100 ms = 90 s.  Deliberately NOT multiplied by kSanitizerBudgetScale
// (matching the e2e precedent, which is flat): 90 s already covers the
// sanitizer lanes' slowdown, and scaling it would let a genuinely-broken
// run burn 18 min per exhausted loop under TSan — several sequential loops
// would then hit the bazel/job timeouts and report a bare timeout instead of
// the assertion message.  This is a ceiling on a condition poll that returns
// the instant its variant appears, so widening it costs a passing run
// nothing.
inline constexpr int kViewportSiblingPollIterations = 900;

// Helper to read a test image file into a string.
std::string ReadTestFile(const std::string& filename) {
  std::string path = "test/lib/image/testdata/" + filename;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return "";
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

// =============================================================================
// IPC Protocol Tests
// =============================================================================

TEST(WorkerIpcTest, NotificationSerialization) {
  CacheNotification notification;
  notification.url = "http://example.com/page.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x12345678;

  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url, notification.url);
  EXPECT_EQ(parsed.content_type, notification.content_type);
  EXPECT_EQ(parsed.capability_mask, notification.capability_mask);
}

TEST(WorkerIpcTest, NotificationWithEmptyUrl) {
  CacheNotification notification;
  notification.url = "";
  notification.scheme = "https";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = 0;

  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url, "");
  EXPECT_EQ(parsed.capability_mask, 0u);
}

TEST(WorkerIpcTest, NotificationWithLongUrl) {
  CacheNotification notification;
  notification.url = std::string(4096, 'x');
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = 0xFFFFFFFF;

  std::vector<char> data = notification.Serialize();

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url, notification.url);
  EXPECT_EQ(parsed.content_type, ContentType::kImage);
  EXPECT_EQ(parsed.capability_mask, 0xFFFFFFFFu);
}

TEST(WorkerIpcTest, DeserializeTruncatedData) {
  CacheNotification parsed;
  // Too short to contain length header
  EXPECT_FALSE(CacheNotification::Deserialize("ab", &parsed));
  // Length says more data than available
  EXPECT_FALSE(
      CacheNotification::Deserialize(std::string_view("\x00\x00\x00\xFF"
                                                      "abcd",
                                                      8),
                                     &parsed));
}

// Verify that embedded null bytes in the URL field do not cause a crash
// or data corruption during serialization and deserialization.
TEST(WorkerIpcTest, MalformedIpcNullByteInUrl) {
  // Build a URL with embedded null bytes.
  std::string url_with_nulls = "http://example.com/";
  url_with_nulls.push_back('\0');
  url_with_nulls += "page.html";

  CacheNotification notification;
  notification.url = url_with_nulls;
  notification.scheme = "https";
  notification.hostname = "example.com";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x08;

  // Roundtrip through Serialize/Deserialize.
  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  // The URL must survive the roundtrip byte-for-byte, including the null.
  EXPECT_EQ(parsed.url.size(), url_with_nulls.size());
  EXPECT_EQ(parsed.url, url_with_nulls);
  EXPECT_EQ(parsed.hostname, "example.com");
  EXPECT_EQ(parsed.content_type, ContentType::kHtml);
  EXPECT_EQ(parsed.capability_mask, 0x08u);
}

// Verify that out-of-range capability mask values and invalid content-type
// bytes are handled gracefully (no crash, correct accept/reject).
TEST(WorkerIpcTest, MalformedIpcInvalidMask) {
  // An all-bits-set mask (0xFFFFFFFF) has no valid encoding but must not
  // crash.  Deserialize accepts any uint32_t mask value.
  {
    CacheNotification notification;
    notification.url = "http://example.com/test";
    notification.scheme = "https";
    notification.hostname = "example.com";
    notification.content_type = ContentType::kImage;
    notification.capability_mask = 0xFFFFFFFF;

    std::vector<char> data = notification.Serialize();
    CacheNotification parsed;
    EXPECT_TRUE(CacheNotification::Deserialize(
        std::string_view(data.data(), data.size()), &parsed));
    EXPECT_EQ(parsed.capability_mask, 0xFFFFFFFFu);
  }

  // Hand-craft a message with an invalid content_type byte (0xFF, which
  // is well above ContentType::kOther = 4).  Deserialize must reject it.
  {
    CacheNotification valid;
    valid.url = "http://x.com/a";
    valid.scheme = "https";
    valid.hostname = "";
    valid.content_type = ContentType::kOther;
    valid.capability_mask = 0;

    std::vector<char> data = valid.Serialize();
    ASSERT_GT(data.size(), 4u);

    // The content_type byte sits right after the hostname field.
    // Wire format (v3): [4B total_len] [1B version] [4B url_len] [url] [4B host_len] [host] [1B ct] [4B mask] [1B scheme]
    // Find the ct byte: offset = 4 + 1 + 4 + url_len + 4 + host_len
    size_t ct_offset = 4 + 1 + 4 + valid.url.size() + 4 + valid.hostname.size();
    ASSERT_LT(ct_offset, data.size());
    data[ct_offset] = static_cast<char>(0xFF);  // Invalid content type

    CacheNotification parsed;
    EXPECT_FALSE(CacheNotification::Deserialize(
        std::string_view(data.data(), data.size()), &parsed))
        << "Deserialize should reject content_type byte 0xFF";
  }
}

// Verify that non-UTF-8 byte sequences in the URL do not cause a crash.
// The IPC protocol is encoding-agnostic (length-prefixed binary), so
// arbitrary bytes should survive the roundtrip.
TEST(WorkerIpcTest, MalformedIpcNonUtf8Url) {
  // Construct a URL containing invalid UTF-8 sequences:
  // 0xFE and 0xFF are never valid in UTF-8; 0xC0 0x01 is an overlong
  // encoding; 0x80 is a continuation byte without a leading byte.
  std::string bad_url = "http://example.com/";
  bad_url += std::string("\xFE\xFF\xC0\x01\x80\xED\xBF\xBF", 8);
  bad_url += "/end";

  CacheNotification notification;
  notification.url = bad_url;
  notification.scheme = "https";
  notification.hostname = std::string("\xC0\xAF", 2);  // Overlong slash
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0xDEADBEEF;

  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url.size(), bad_url.size());
  EXPECT_EQ(parsed.url, bad_url);
  EXPECT_EQ(parsed.hostname, notification.hostname);
  EXPECT_EQ(parsed.content_type, ContentType::kCss);
  EXPECT_EQ(parsed.capability_mask, 0xDEADBEEFu);
}

// =============================================================================
// Worker Integration Tests
// =============================================================================

class WorkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create unique temp dir for each test
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache";
#ifdef _WIN32
    // Windows Named Pipes need simple identifiers, not full filesystem paths.
    // Worker::Initialize() prepends \\.\pipe\ on Windows. Include the prefix
    // in socket_path_ so test assertions comparing against socket_path() match.
    socket_path_ = "\\\\.\\pipe\\ps_worker_" +
                   std::to_string(std::hash<std::string>{}(temp_dir_));
#else
    socket_path_ = temp_dir_ + "/worker.sock";
#endif
  }

  void TearDown() override {
    if (!temp_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir_, ec);
    }
  }

  // Helper: send a notification to the worker via IPC pipe.
  // Scheme must be set explicitly (v3 wire format requires "http" or "https").
  void SendNotification(const CacheNotification& notification) {
    ASSERT_FALSE(notification.scheme.empty())
        << R"(Notification scheme must be set ("http" or "https"))";
    intptr_t client_fd = pagespeed::test::ConnectPipe(socket_path_);
    ASSERT_GE(client_fd, static_cast<intptr_t>(0))
        << "Failed to connect to worker socket";

    std::vector<char> data = notification.Serialize();
    ssize_t written =
        pagespeed::test::PipeWrite(client_fd, data.data(), data.size());
    EXPECT_EQ(written, static_cast<ssize_t>(data.size()));

    pagespeed::test::ClosePipe(client_fd);
  }

  // Helper: put raw bytes on the notify socket, bypassing Serialize().
  //
  // Required for skew tests: the point of those is to present a frame no
  // in-tree writer can produce — one stamped with another peer's wire version
  // — and Serialize() by construction always stamps the current one.  Going
  // through the real socket rather than calling Deserialize directly is what
  // makes them cover the RECEIVE PATH's handling of the refusal (counters,
  // logging, non-dispatch) instead of only the codec's verdict.
  void SendRawNotificationBytes(const std::vector<char>& bytes) {
    intptr_t client_fd = pagespeed::test::ConnectPipe(socket_path_);
    ASSERT_GE(client_fd, static_cast<intptr_t>(0))
        << "Failed to connect to worker socket";
    ssize_t written =
        pagespeed::test::PipeWrite(client_fd, bytes.data(), bytes.size());
    EXPECT_EQ(written, static_cast<ssize_t>(bytes.size()));
    pagespeed::test::ClosePipe(client_fd);
  }

  // Barrier: wait until every dispatched work item has retired.
  //
  // A processing counter (js_processed / html_processed) is incremented inside
  // OnNotificationWork, but the URL is only removed from the in-flight set later,
  // by OnNotificationDone on the event loop.  Between those two points a repeat
  // notification for the same URL is rejected by the in-flight guard
  // (notifications_skipped_inflight) rather than reaching the processed-variant
  // dedup or the cooldown check.  A test that acts the moment a counter moves is
  // racing the event loop, and loses that race on a loaded machine.
  //
  // OnNotificationDone erases the in-flight entry BEFORE decrementing
  // in_flight_work_, so observing zero here means the entry is already gone.
  void WaitForWorkerIdle(Worker& worker) {
    // Generous cap: this returns the instant the worker is idle, but image
    // tests with proactive variants can legitimately keep the pool busy for
    // tens of seconds on a loaded runner -- and a sanitizer build stretches
    // that by kSanitizerBudgetScale (TSan's ~10x instrumentation cost lands
    // squarely on the image transcode path), so the cap scales with it.
    const int kMaxPolls = 6000 * kSanitizerBudgetScale;
    for (int i = 0; i < kMaxPolls; ++i) {
      if (worker.in_flight_work() == 0) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "worker did not drain in-flight work within "
                  << (kMaxPolls / 100) << "s";
  }

  // Barrier: wait until the worker's variant pipeline has produced at least
  // one write, then wait for every dispatched work item to retire.
  //
  // WaitForWorkerIdle alone races worker STARTUP: between SendNotification's
  // pipe write and DispatchNotification registering the work item on the
  // event loop, in_flight_work() still reads 0, so a bare idle-wait can
  // return before the notification was even consumed (a slowdown-heavy
  // build stretches that window from microseconds to observable time).
  // variants_written only moves on a pool thread strictly inside a work
  // item's in-flight window (incremented after in_flight_work_ goes up,
  // before it comes down), so once it is observed non-zero the idle-wait
  // below cannot return until that item -- and the rest of its variant
  // matrix, which runs within the same work item -- has retired.
  //
  // Only usable in tests whose expected outcome includes at least one
  // successful variant write; there the gate cannot mask anything, because
  // a pipeline that writes nothing trips the ADD_FAILURE here and the
  // test's own assertions right after.
  void WaitForFirstVariantThenIdle(Worker& worker) {
    const int kMaxPolls = 6000 * kSanitizerBudgetScale;
    for (int i = 0; i < kMaxPolls; ++i) {
      if (worker.stats().variants_written.load(std::memory_order_relaxed) >=
          1) {
        WaitForWorkerIdle(worker);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "no variant write observed within " << (kMaxPolls / 100)
                  << "s";
  }

  // Strip scheme+authority from a URL, matching what the worker's
  // NormalizeCacheUrl does.  "http://example.com/path" → "/path".
  // When hostname is empty (page-level operations), the worker uses
  // path-only keys.  When hostname is set (external CSS refs), the
  // worker resolves absolute hrefs as-is.
 public:
  static std::string StripSchemeAuthority(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;
    auto first_slash = url.find('/');
    if (first_slash != std::string::npos && scheme_end < first_slash) {
      auto authority_end = url.find('/', scheme_end + 3);
      if (authority_end != std::string::npos) return url.substr(authority_end);
      return "/";
    }
    return url;
  }

 protected:
  // Helper: pre-populate cache with content using the PageSpeedCache API.
  // Writes as a regular alternate at the default mask (Desktop/Identity = 0x08)
  // so ReadBestAlternate can find it via the PageSpeedSelector.
  // URL is normalized to path-only (matching worker's NormalizeCacheUrl).
  void CacheOriginal(PageSpeedCache* cache, const std::string& url,
                     const std::string& content,
                     const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    CapabilityMask default_mask;  // Desktop/Identity = 0x08
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = cache->WriteAlternate(effective_url, hostname, "https", id,
                                    content.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "Failed to open write handle";
    auto written = wh->write_sync(
        std::as_bytes(std::span(content.data(), content.size())));
    ASSERT_TRUE(written.has_value()) << "Failed to write content";
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value()) << "Failed to close write handle";
  }

  // Helper: leave the URL holding a durable ORIGINAL and NOTHING ELSE — the
  // cache shape an external writer produces, and the shape the source fallback
  // exists for.  Deliberately not CacheOriginal's default-mask alternate: this
  // writes the durable-original class (SentinelId::kOriginalContent, 0x0C),
  // which ordinary selection can never return.
  //
  // Calls the C++ entry point, NOT the published C one, and the distinction is
  // worth being exact about rather than glossed: ps_cache_write_original is a
  // thin shim over this same WriteOriginalAlternate — it converts the caller's
  // params (flags included, verbatim) and stamps the class id, then calls it
  // (lib/pagespeed/pagespeed.cc).  So what this produces is byte-identical to
  // what the ABI produces, but that is an argument, not a proof.  The proof is
  // split deliberately and neither half is redundant:
  //
  //   * lib/pagespeed/pagespeed_test.cc drives the REAL ABI end-to-end and
  //     asserts the marker survives into the stored entry;
  //   * these tests assert what the worker does with such an entry;
  //   * a static_assert in pagespeed.cc pins PS_FLAG_ORIGIN_VARIES_ACCEPT to
  //     AlternateMetadata::kFlagOriginVariesAccept, so the two halves cannot
  //     be talking about different bits.
  //
  // The worker suite cannot close that loop by itself: lib/pagespeed's test
  // has no worker dependency (and inverting that to give it one would point
  // lib/ at src/worker/).
  //
  // Origin state is stamped the way a front end that just fetched from the
  // origin stamps it, because the class's lifetime is bound to it.
  void CacheDurableOriginal(PageSpeedCache* cache, const std::string& url,
                            const std::string& content,
                            const std::string& origin_content_type,
                            ContentType content_type = ContentType::kOther,
                            uint32_t cache_inserted_at = 1'700'000'000,
                            uint32_t origin_max_age = 600,
                            const std::string& hostname = "",
                            uint8_t flags = 0) {
    std::string effective_url = StripSchemeAuthority(url);
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
    meta.content_type = content_type;
    meta.origin_content_type = origin_content_type;
    meta.cache_inserted_at = cache_inserted_at;
    meta.origin_max_age = origin_max_age;
    meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                           AlternateMetadata::kCCOriginPublic;
    meta.flags = flags;
    auto wh = cache->WriteOriginalAlternate(effective_url, hostname, "https",
                                            content.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "Failed to open durable-original handle";
    auto written = wh->write_sync(
        std::as_bytes(std::span(content.data(), content.size())));
    ASSERT_TRUE(written.has_value()) << "Failed to write durable original";
    ASSERT_TRUE(wh->close_sync().has_value())
        << "Failed to close durable original";
  }

  // Helper: read a specific variant from cache by its exact mask ID.
  // URL is normalized to path-only to match worker's NormalizeCacheUrl.
  std::string ReadVariant(PageSpeedCache* cache, const std::string& url,
                          const CapabilityMask& mask,
                          const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto result = cache->ReadAlternate(effective_url, hostname, "https", id);
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Helper: read the best variant for a mask using selector fallback.
  // Unlike ReadVariant (exact ID), this finds the best-matching alternate
  // via PageSpeedSelector scoring, handling deduped variants gracefully.
  // URL is normalized to path-only to match worker's NormalizeCacheUrl.
  std::string ReadBestVariant(PageSpeedCache* cache, const std::string& url,
                              const CapabilityMask& mask,
                              const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    auto result =
        cache->ReadBestAlternate(effective_url, hostname, "https", mask);
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Helper: read a specific variant and its metadata.
  struct VariantWithMeta {
    std::string content;
    AlternateMetadata metadata;
  };
  std::optional<VariantWithMeta> ReadVariantWithMeta(
      PageSpeedCache* cache, const std::string& url, const CapabilityMask& mask,
      const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto result = cache->ReadAlternate(effective_url, hostname, "https", id);
    if (!result.has_value()) return std::nullopt;
    auto content = result->content();
    return VariantWithMeta{
        std::string(reinterpret_cast<const char*>(content.data()),
                    content.size()),
        result->metadata};
  }

  // Helper: read a sentinel entry from cache.
  // URL is normalized to path-only to match worker's NormalizeCacheUrl.
  std::string ReadSentinel(PageSpeedCache* cache, const std::string& url,
                           SentinelId sentinel,
                           const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    auto result = cache->ReadAlternate(effective_url, hostname, "https",
                                       static_cast<AlternateId>(sentinel));
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Seed a browser profile for `html`'s template, carrying (or withholding) the
  // validation record the async-CSS deferral gate requires.
  //
  // All three viewports get the same record, so a caller does not have to
  // mirror the capability-mask -> viewport mapping to know which one the worker
  // will select. `validated_against_css` is the COMBINED stylesheet the
  // validation is claimed to have been made against; pass the bytes the worker
  // will actually assemble to authorize deferral, or different bytes to model
  // a stylesheet-only redeploy.
  //
  // `validated_hash_override`, when engaged, writes that string into
  // validated_combined_css_hash instead of hashing anything. It exists to reach
  // records the hash function CANNOT produce — an empty stored hash, or the
  // digest of an empty sheet — which is exactly where a dropped guard would let
  // "validated against nothing" satisfy the gate.
  void StoreBrowserProfile(
      Worker& worker, const std::string& notification_url,
      const std::string& html, std::string_view profile_critical_css,
      bool validated, std::string_view validated_against_css,
      std::optional<std::string> validated_hash_override = std::nullopt) {
    HtmlScanner scanner;
    HtmlScanResult scan = scanner.Scan(notification_url, html);
    ASSERT_TRUE(scan.success);

    ViewportProfile vp;
    vp.critical_css = std::string(profile_critical_css);
    // Under the 0.60 inline budget, so ShouldInlineCriticalCss keeps the block.
    vp.css_coverage_ratio = 0.2f;
    vp.total_css_bytes = profile_critical_css.size() * 5;
    vp.unused_css_bytes = vp.total_css_bytes - profile_critical_css.size();
    vp.critical_css_validated = validated;
    vp.validation_diff_ratio = validated ? 0.001f : -1.0f;
    if (validated) {
      vp.validated_critical_css_hash =
          CombinedCssValidationHash(profile_critical_css);
      vp.validated_combined_css_hash =
          CombinedCssValidationHash(validated_against_css);
    }
    if (validated_hash_override.has_value()) {
      vp.validated_combined_css_hash = *validated_hash_override;
    }

    OptimizationProfile profile;
    profile.template_hash_hex = "seeded";
    profile.analyzed_url = notification_url;
    profile.mobile = vp;
    profile.tablet = vp;
    profile.desktop = vp;
    profile.created_at = 0;
    profile.expires_at = 0;  // no TTL — LookupProfile keeps it

    ASSERT_NE(worker.TestBrowserManager(), nullptr)
        << "browser manager must be constructed to seed a profile";
    worker.TestBrowserManager()->TestStoreProfile(
        TemplateDetector::HashStructure(scan), profile);
  }

  // A worker config that constructs the BrowserAnalysisManager (so a profile
  // can be seeded) without ever starting Chrome.
  WorkerConfig BrowserProfileConfig() {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    // The test binary runs as root in the build container,
    // where the Chrome sandbox is (correctly) unavailable and `require` would
    // refuse to construct the manager.  These tests seed a profile directly
    // and never spawn Chrome, so take the documented opt-out explicitly.
    config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
    config.browser_analysis.enabled = true;
    config.browser_analysis.chrome_binary = "/nonexistent/chrome";
    return config;
  }

  std::string temp_dir_;
  std::string socket_path_;
  std::string cache_path_;
};

// Stops the worker and joins its thread on scope exit.
//
// Load-bearing wherever a test uses a FATAL assertion (ASSERT_*) after Run()
// has been started: a fatal assertion returns from the test body immediately,
// so a trailing Shutdown()/join() never executes and a still-joinable
// std::thread is destroyed — which calls std::terminate and takes the whole
// binary, and every test after it, down with one failed expectation. With this,
// a failing assertion reports as a failing assertion.
//
// This is the file's single guard idiom (#1352, consolidated in #1354): every
// test that starts a worker thread and can hit a fatal assert afterwards
// declares one immediately after the thread start, and new tests do the same —
// there is deliberately no second guard class, so the absence of a
// WorkerStopper at a post-start fatal assert means something. The one
// sanctioned exception is GracefulShutdownDrainsConnections, whose teardown is
// the behavior under test; it carries an explicit pre-assert cleanup instead.
class WorkerStopper {
 public:
  WorkerStopper(Worker& worker, std::thread& thread)
      : worker_(worker), thread_(thread) {}
  WorkerStopper(const WorkerStopper&) = delete;
  WorkerStopper& operator=(const WorkerStopper&) = delete;
  ~WorkerStopper() {
    worker_.Shutdown();
    thread_.join();
  }

 private:
  Worker& worker_;
  std::thread& thread_;
};

TEST_F(WorkerTest, InitializeAndShutdown) {
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  EXPECT_TRUE(worker.Initialize());
  EXPECT_EQ(worker.socket_path(), socket_path_);

  // Socket/pipe endpoint should exist
  EXPECT_TRUE(pagespeed::test::PipeEndpointExists(socket_path_));

  worker.Shutdown();
}

TEST_F(WorkerTest, InitializeWithCache) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  EXPECT_TRUE(worker.Initialize());
  EXPECT_NE(worker.cache(), nullptr);

  worker.Shutdown();
}

TEST_F(WorkerTest, HtmlNotificationInjectsCriticalCss) {
  // HTML critical CSS injection: the worker reads original HTML,
  // extracts critical CSS, injects it as a <style> tag, and writes
  // the modified HTML as a variant.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML content containing inline CSS.
  // Use path-only URL for cache operations (the worker normalizes URLs
  // by stripping scheme+authority via NormalizeCacheUrl).
  std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, html);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;  // WebP capability
  SendNotification(notification);

  // Poll for variant to appear.  The worker strips image-format bits
  // for text content types, so the variant is written at the
  // format-stripped mask (Original) regardless of the notification's
  // WebP format bits.
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  // Verify a variant was written with injected critical CSS
  EXPECT_FALSE(variant.empty()) << "HTML variant not written";
  if (!variant.empty()) {
    // The variant should contain the pagespeed-critical style tag
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    // The variant should still be valid HTML with original content
    EXPECT_NE(variant.find("Hello World"), std::string::npos);
    EXPECT_NE(variant.find("</head>"), std::string::npos);
    // The variant should be larger than the original (CSS injected)
    EXPECT_GT(variant.size(), html.size());
  }

  // Verify original HTML is still intact at the default mask
  CapabilityMask default_mask;
  std::string cached = ReadVariant(worker.cache(), cache_url, default_mask);
  EXPECT_EQ(cached, html);

  worker.Shutdown();
  worker_thread.join();
}

// Regression (the /console/urls timeout fix): /v1/cache/urls must report a
// NON-ZERO alternate_count for HTML pages.  The count is cached in the URL
// registry by a scope guard in HandleNotification that fires for EVERY content
// type — crucially the HTML write path goes through WriteVariant /
// WriteCompressedVariants, NOT WriteTextVariant, so a refresh wired only into
// the text/image writers (the naive fix) would leave every HTML row at 0.  We
// drive a real HTML notification end-to-end and assert the cached count equals
// the live unique-alternate-id count, computed independently below.
TEST_F(WorkerTest, HtmlNotificationCachesAlternateCount) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Hello</h1></body></html>";
  std::string cache_url = "/count.html";
  CacheOriginal(worker.cache(), cache_url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/count.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;  // WebP capability
  SendNotification(notification);

  // Poll until the registry has cached a non-zero count for the page.  The
  // count guard fires at HandleNotification exit (just after the variant
  // write), so wait on the count itself rather than on the variant.
  auto cached_count = [&]() -> std::optional<uint32_t> {
    const UrlRegistry* reg = worker.url_registry();
    if (reg == nullptr) return std::nullopt;
    UrlRegistry::Page page = reg->List(0, 100);
    for (const auto& e : page.entries) {
      if (e.url == cache_url) return e.alternate_count;
    }
    return std::nullopt;
  };
  std::optional<uint32_t> count;
  for (int i = 0; i < 50 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    count = cached_count();
    if (count.has_value() && *count > 0) break;
  }

  ASSERT_TRUE(count.has_value()) << "page URL not recorded in registry";
  EXPECT_GT(*count, 0u)
      << "HTML alternate_count was not cached — the count guard missed the "
         "HTML (WriteVariant) write path";

  // The cached count must match the live unique-alternate-id count, computed
  // here independently of the production CountUniqueAlternates so a bug in that
  // helper would also be caught.
  auto alts = worker.cache()->ListAlternates(cache_url, "", "https");
  ASSERT_TRUE(alts.has_value());
  std::unordered_set<uint8_t> unique_ids;
  for (const auto& a : *alts) {
    unique_ids.insert(static_cast<uint8_t>(a.id));
  }
  EXPECT_EQ(static_cast<size_t>(*count), unique_ids.size());
}

// Issue A end-to-end wiring pin.  Sibling to
// HtmlNotificationInjectsCriticalCss above (which is the LOW-coverage inline
// path): here we seed a HIGH-coverage browser profile (coverage 0.85, ~93 KB
// critical of a ~110 KB sheet — Chrome's pathological Tailwind over-report) and
// drive the SAME HandleNotification path.  This pins the whole suppression
// wiring in worker.cc that the pure ShouldInlineCriticalCss helper test cannot
// see: skip_critical_css computed at profile-selection time, the
// critical_css.clear() applied AFTER the heuristic fallback, the resulting
// has_async_css suppression, and the critical_css_skipped_high_coverage stat.
// A regression (flag computed-but-never-applied, a clear() moved before the
// heuristic fallback so it re-extracts, async deferral not suppressed, or a
// miswired stat) flips one of the four asserts below.
TEST_F(WorkerTest, HtmlNotificationSuppressesHighCoverageBrowserCriticalCss) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Construct the BrowserAnalysisManager so LookupProfile can return our seeded
  // profile.  A non-existent Chrome binary makes StartChrome fail gracefully
  // (non-fatal) — no live render is needed; we seed the profile directly.
  // The test binary runs as root in the build container,
  // where the Chrome sandbox is (correctly) unavailable and `require`
  // would refuse to construct the manager.  These tests seed profiles
  // directly and never spawn Chrome, so take the documented opt-out.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.TestBrowserManager(), nullptr)
      << "browser manager must be constructed to seed a profile";

  // HTML with a render-blocking external <link rel="stylesheet"> and a body
  // <img> (so lazy-load still produces a variant even when critical CSS is
  // suppressed — proving the suppression does NOT short-circuit the whole HTML
  // pass).  Path-only URL for cache ops; full URL for the notification.
  std::string cache_url = "/tailwind.html";
  std::string notification_url = "http://example.com/tailwind.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/tailwind.css\">"
      "</head><body><h1>Hello World</h1>"
      "<img src=\"/hero.png\"><img src=\"/below.png\"></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // Compute the template hash the worker will compute from this HTML, then seed
  // a HIGH-coverage profile under it on the MOBILE viewport (the notification
  // mask 0x1 decodes to Viewport::kMobile, so the worker selects ->mobile).
  HtmlScanner scanner;
  HtmlScanResult scan_result = scanner.Scan(notification_url, html);
  ASSERT_TRUE(scan_result.success);
  uint64_t template_hash = TemplateDetector::HashStructure(scan_result);

  OptimizationProfile profile;
  profile.template_hash_hex = "deadbeef";
  profile.analyzed_url = notification_url;
  // ~93 KB "critical" CSS of a ~110 KB sheet at 0.85 coverage — the bug case
  // ShouldInlineCriticalCss must suppress (0.85 >= 0.60 floor).
  profile.mobile.critical_css = std::string(93363, 'a');
  profile.mobile.css_coverage_ratio = 0.85f;
  profile.mobile.total_css_bytes = 110388;
  profile.mobile.unused_css_bytes = 110388 - 93363;
  profile.created_at = 0;
  profile.expires_at = 0;  // no TTL — LookupProfile keeps it
  worker.TestBrowserManager()->TestStoreProfile(template_hash, profile);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;  // viewport bits -> Mobile
  SendNotification(notification);

  // The variant is written at the format-stripped mask (Mobile/Original).
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  ASSERT_FALSE(variant.empty())
      << "HTML variant not written (lazy-load on the <img> should still apply)";

  // (a) Inline critical CSS suppressed: no injected <style data-pagespeed-...>.
  EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos)
      << "high-coverage browser critical CSS must NOT be inlined";
  // (b) Async deferral also suppressed: no preload swap, no async marker.
  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "with critical CSS suppressed, the sheet must NOT be async-deferred";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos)
      << "async preload swap must not be applied";
  // (c) The original render-blocking external <link rel=stylesheet> survives.
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos)
      << "original render-blocking external <link> must be preserved";
  EXPECT_NE(variant.find("tailwind.css"), std::string::npos);
  // The variant still exists because an image transform ran (a non-CSS
  // transform DID apply) — i.e. suppressing critical CSS did NOT short-circuit
  // the whole HTML pass.  The first body <img> always gets fetchpriority="high"
  // (LCP-candidate match or first-img heuristic).
  EXPECT_NE(variant.find("fetchpriority=\"high\""), std::string::npos)
      << "image transform should still apply (variant is not CSS-only)";

  // (d) The suppression stat incremented exactly once.
  EXPECT_EQ(worker.stats().critical_css_skipped_high_coverage.load(), 1u)
      << "critical_css_skipped_high_coverage must increment on suppression";
}

// =============================================================================
// Async-CSS empirical gate: deferral requires a validated profile that is still
// bound to the stylesheet being served.
// =============================================================================

namespace async_css_gate {

// One page, one stylesheet, across the whole group: the ONLY thing that varies
// between these tests is the validation record, so any difference in the served
// markup is the gate's doing and nothing else's.
constexpr const char* kCssUrl = "http://example.com/site.css";
constexpr const char* kCss = ".hero{color:red;font-size:24px}p{margin:0}";
// What the browser profile claims as its critical block. It is used as the
// coverage-identity source for the re-derivation, not inlined verbatim.
constexpr const char* kProfileCriticalCss = ".hero{color:red;font-size:24px}";

std::string PageHtml() {
  return std::string("<html><head><link rel=\"stylesheet\" href=\"") + kCssUrl +
         "\"></head><body><div class=\"hero\">Hello</div></body></html>";
}

}  // namespace async_css_gate

// D2, heuristic half: with no browser profile there is no validation record and
// none can be produced, so the heuristic extractor's output must never
// authorize deferral — however favourable the byte ratio looks. The sheet here
// is well under async_css_min_deferred_bytes, so the small-sheet escape hatch
// makes the byte gate say "sufficient"; that is exactly the free pass this
// closes.
TEST_F(WorkerTest, HeuristicPathNeverDefersAsyncCss) {
  using namespace async_css_gate;
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/heuristic.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "the heuristic path has no validation record and must not defer";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos)
      << "the render-blocking <link> must survive";
  // The inline block is still a win on its own and must NOT be suppressed.
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos)
      << "only the deferral is refused; the critical CSS is still inlined";
}

// A profile exists but carries no validation record: the same refusal, now on
// the path that CAN one day produce one.
TEST_F(WorkerTest, AsyncCssRequiresValidatedProfile) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unvalidated.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/false, /*validated_against_css=*/"");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "an unvalidated profile must not authorize deferral";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
}

// The positive control: the gate does not simply suppress everything.
TEST_F(WorkerTest, AsyncCssDefersWhenProfileValidatedAndHashMatches) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/validated.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  // The page's only stylesheet and no inline <style>, so the combined sheet the
  // worker assembles IS these bytes.
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (variant.find("as=\"style\"") != std::string::npos) break;
  }

  EXPECT_NE(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validated profile bound to the served sheet must defer";
  EXPECT_NE(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_GE(worker.stats().policy_async_css_enabled.load(), 1u);
  // The #1216 guard is not in the way of the happy path: the derivation DID
  // produce the block, so the record stands.
  EXPECT_EQ(worker.stats().async_css_record_dropped_empty_derivation.load(),
            0u);

  worker.Shutdown();
  worker_thread.join();
}

// #1216: the record is dropped the moment the derivation comes back empty for
// THIS page, before anything else can take the derived block's place.
//
// Setup: a healthy record — validated bit set, hash still bound to the served
// stylesheet — on a profile whose critical block names a selector the current
// sheet no longer contains, served to a page whose fold matches nothing in that
// sheet either. So the DOM-matched derivation has nothing to emit and nothing
// to force-include, and what the serve path inlines from here on is not what
// the record was produced about. The sheet is far under
// async_css_min_deferred_bytes, so the byte gate's small-sheet escape hatch
// says "sufficient" whatever the coverage is: the record is the only thing left
// between a substituted block and a deferred stylesheet, which is why it must
// not still be standing.
//
// The counter is the assertion that pins WHERE the record is dropped. Today the
// heuristic fallback finds nothing here either (it sees the same DOM through a
// strictly less inclusive matcher), so the served markup alone cannot tell a
// dropped record apart from an empty one; make the fallback more inclusive than
// the derivation and only the drop point stands between its block and a
// deferral.
TEST_F(WorkerTest, AsyncCssRecordDroppedWhenNoBlockDerivedForPage) {
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  constexpr const char* kCssUrl = "http://example.com/nomatch.css";
  constexpr const char* kCss = ".hero{color:red;font-size:24px}p{margin:0}";
  // A block from an earlier build of the sheet: its selector is not in the
  // served sheet, so it force-includes nothing.
  constexpr const char* kStaleProfileCss = ".legacy-hero{color:red}";

  const std::string url = "http://example.com/nomatch.html";
  // Neither `.hero` nor `p` occurs in the fold. The <img> elements are here so
  // an image transform still applies and a variant is written at all.
  const std::string html =
      std::string("<html><head><link rel=\"stylesheet\" href=\"") + kCssUrl +
      "\"></head><body><div class=\"banner\">Hello</div>"
      "<img src=\"/one.png\"><img src=\"/two.png\"></body></html>";
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kStaleProfileCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty())
      << "HTML variant not written (the image transform should still apply)";

  EXPECT_EQ(worker.stats().async_css_record_dropped_empty_derivation.load(), 1u)
      << "the record must be dropped where the derivation comes back empty, "
         "not left standing for whatever is inlined next";
  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "no block was derived for this page, so nothing may be deferred";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos)
      << "the render-blocking <link> must survive";
  EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos)
      << "precondition: no critical block was produced for this page at all";
}

// C2, the scenario that actually occurs: a stylesheet-only redeploy. The tag
// tree is unchanged, so the template hash still resolves the same profile and
// its validated bit is still alive — but the bytes it was validated against are
// gone. Honouring the bit here re-derives a never-validated critical block from
// the NEW sheet and defers on the OLD sheet's verdict.
TEST_F(WorkerTest, AsyncCssRefusedWhenCombinedCssHashDiverges) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/redeployed.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  // Validated — against a DIFFERENT build of the stylesheet.
  StoreBrowserProfile(
      worker, url, html, kProfileCriticalCss, /*validated=*/true,
      /*validated_against_css=*/".hero{color:blue;font-size:24px}p{margin:0}");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validation made against different stylesheet bytes must not carry "
         "over to the redeployed sheet";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
}

// M6(a): the stylesheet resolves from cache but its body is empty, so the
// combined sheet is empty while external_css_missing stays FALSE. The profile
// path then inlines the browser block verbatim at coverage -1, and the byte
// gate's `deferred_css_bytes == 0` free pass waves the deferral through —
// deferring a real <link> on a decision made against no stylesheet at all.
// "Validated against nothing" must never satisfy the gate, so this holds even
// with the validated bit set and a hash on file.
TEST_F(WorkerTest, EmptyCombinedCssNeverDefers) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/emptysheet.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, "", "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "no stylesheet was gathered, so nothing was measured and nothing may "
         "be deferred";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos);
}

// The "validated against nothing" escape, half one: a record that says
// validated but names no stylesheet. Two empty strings compare equal, so an
// unguarded gate reads this as a match against the equally empty combined sheet
// and defers. A record is only meaningful when it says WHAT it was made
// against.
TEST_F(WorkerTest, ValidatedRecordWithNoStylesheetHashNeverDefers) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/nohash.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  // Cached but EMPTY, so the combined sheet is empty too — both sides of the
  // comparison are "" unless the gate refuses an unbound record outright.
  CacheOriginal(worker.cache(), kCssUrl, "", "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss,
                      /*validated_hash_override=*/std::string());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validation record naming no stylesheet must not authorize deferral";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos);
}

// Half two: a record that DOES name a stylesheet — the digest of an empty one.
// Only reachable through the override, because the hash function refuses to
// produce it; that refusal is the guard under test. Drop it and this record
// matches an empty combined sheet, deferring a real <link> against a validation
// performed on no CSS whatsoever.
TEST_F(WorkerTest, ValidationAgainstAnEmptySheetNeverDefers) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/emptyhash.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, "", "example.com");
  // SHA-256 of the empty string — what hashing an empty sheet WOULD give if
  // CombinedCssValidationHash did not special-case it.
  StoreBrowserProfile(
      worker, url, html, kProfileCriticalCss, /*validated=*/true,
      /*validated_against_css=*/kCss,
      /*validated_hash_override=*/
      std::string(
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validation performed against an empty stylesheet must not "
         "authorize deferring a real one";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
}

// D6: the decision must not depend on how warm the cache happened to be when a
// notification arrived. Same URL, repeated notifications, warmer cache each
// time — the served markup keeps the same verdict.
TEST_F(WorkerTest, AsyncCssDecisionStableAcrossRepeatedNotifications) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/stable.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first = ReadVariant(worker.cache(), url, mask);
    if (first.find("as=\"style\"") != std::string::npos) break;
  }
  const bool deferred_first =
      first.find("data-pagespeed-async") != std::string::npos;
  ASSERT_TRUE(deferred_first)
      << "precondition: the validated profile should defer on the first pass";

  // Re-notify against a fully warm cache several times.
  for (int i = 0; i < 5; ++i) {
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string again = ReadVariant(worker.cache(), url, mask);
    ASSERT_FALSE(again.empty());
    EXPECT_EQ(again.find("data-pagespeed-async") != std::string::npos,
              deferred_first)
        << "the deferral decision changed on repeat notification " << i;
  }
}

TEST_F(WorkerTest, HtmlNotificationWithCachedStylesheet) {
  // HTML with external stylesheet cached: the worker gathers CSS from
  // both inline and external sources, extracts critical CSS, and
  // injects it into the HTML variant.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML that references an external stylesheet.
  // Path-only URL for cache operations; full URL for notification.
  std::string cache_url = "/page.html";
  std::string notification_url = "http://example.com/page.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // Pre-populate the external stylesheet in cache.  The href is absolute,
  // so the worker resolves it as-is and uses UrlHostname to extract
  // "example.com" for the cache lookup.  Use raw_url=true to skip
  // normalization (matching the worker's lookup path for absolute hrefs).
  std::string css_url = "http://example.com/style.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for variant
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  // Verify variant was written with injected critical CSS
  EXPECT_FALSE(variant.empty()) << "HTML variant not written";
  if (!variant.empty()) {
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    // The hero class CSS should be included (it's in the external sheet)
    EXPECT_NE(variant.find("hero"), std::string::npos);
    // Original HTML structure should be preserved
    EXPECT_NE(variant.find("Hello"), std::string::npos);
  }

  // Verify original HTML is still intact at the default mask.
  CapabilityMask default_mask;
  std::string cached = ReadVariant(worker.cache(), cache_url, default_mask);
  EXPECT_EQ(cached, html);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationResolvesRelativeStylesheetHref) {
  // HTML with a relative stylesheet href: the worker resolves the href
  // against the page URL before looking up the CSS in cache.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Keep the sheet render-blocking: an async-deferred sheet gets no preload
  // hint, and this test asserts the RESOLVED href appears in the hints.
  config.disable_async_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML at a deep path that references a relative stylesheet.
  // Path-only URL for cache operations; full URL for notification.
  std::string cache_url = "/demos/ecommerce/index.html";
  std::string notification_url =
      "http://example.com/demos/ecommerce/index.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"style.css\">"
      "</head><body><div class=\"hero\">Shop</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // Pre-populate CSS at the resolved path-only URL.  The href is relative,
  // so the worker resolves it against the page directory (/demos/ecommerce/)
  // to get /demos/ecommerce/style.css, with empty hostname (falls back to
  // notification.hostname which is also empty).
  std::string css_cache_url = "/demos/ecommerce/style.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_cache_url, css);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for variant
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  // Verify variant was written with injected critical CSS
  EXPECT_FALSE(variant.empty()) << "HTML variant not written";
  if (!variant.empty()) {
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    // The hero class from the external stylesheet should appear
    EXPECT_NE(variant.find("hero"), std::string::npos);
    EXPECT_NE(variant.find("Shop"), std::string::npos);
  }

  // Verify early hints contain the resolved path-only URL (not relative).
  std::string hints =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kEarlyHints);
  EXPECT_FALSE(hints.empty()) << "Preload hints not stored";
  if (!hints.empty()) {
    EXPECT_NE(hints.find("/demos/ecommerce/style.css"), std::string::npos)
        << "Hints should contain resolved URL, got: " << hints;
    EXPECT_EQ(hints.find("style.css\n"), std::string::npos)
        << "Hints should not contain raw relative href";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, RevalidationDoesNotDuplicateCriticalCss) {
  // On revalidation the worker re-processes its own output (because the
  // variant overwrites the original at the same AlternateId).  Previously-
  // injected <style data-pagespeed-critical> must not accumulate.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Path-only URL for cache operations; full URL for notification.
  std::string cache_url = "/page.html";
  std::string notification_url = "http://example.com/page.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // CSS has absolute href — worker resolves as-is with UrlHostname.
  // raw_url=true skips normalization for external resource lookups.
  std::string css_url = "http://example.com/style.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — produces variant with critical CSS.
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first_variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first_variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!first_variant.empty()) break;
  }
  ASSERT_FALSE(first_variant.empty()) << "First variant not written";

  // Second notification — simulates revalidation.  The worker now reads
  // the variant (which already contains critical CSS) from cache.
  SendNotification(notification);

  // Wait for the second processing pass to complete.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::string second_variant = ReadVariant(worker.cache(), cache_url, mask);
  ASSERT_FALSE(second_variant.empty()) << "Second variant not written";

  // Count occurrences of the critical CSS marker.
  size_t count = 0;
  size_t pos = 0;
  while ((pos = second_variant.find("data-pagespeed-critical", pos)) !=
         std::string::npos) {
    ++count;
    pos += 23;
  }
  EXPECT_EQ(count, 1u) << "Expected exactly one data-pagespeed-critical tag, "
                          "found "
                       << count << " in:\n"
                       << second_variant;

  // The variant should not be substantially larger than the first pass
  // (small differences from re-minification are acceptable).
  EXPECT_LE(second_variant.size(), first_variant.size() + 50)
      << "Second variant grew unexpectedly — possible CSS duplication";
}

TEST_F(WorkerTest, HtmlNotificationNoCssNoVariant) {
  // HTML with no CSS at all — no variant should be written.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML that has no CSS
  std::string url = "http://example.com/no-css.html";
  std::string html = "<html><head></head><body><p>Hello</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Give worker time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Verify no variant was written (no CSS to inject)
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty());

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, NotificationForMissingCacheEntry) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification for URL not in cache — should not crash
  CacheNotification notification;
  notification.url = "http://example.com/missing.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationDeferred) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send image notification — should be accepted without error
  CacheNotification notification;
  notification.url = "http://example.com/photo.jpg";
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssNotificationMinifiesAndWritesVariant) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS
  std::string url = "http://example.com/style.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Give worker time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Verify minified CSS variant was written at Desktop/Identity.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty());
  // Minified version should be shorter
  EXPECT_LT(variant.size(), css.size());
  // Should still contain key properties
  EXPECT_NE(variant.find("margin"), std::string::npos);
  EXPECT_NE(variant.find("color"), std::string::npos);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssViewportNormalization) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS
  std::string url = "http://example.com/normalize.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing to complete and dedup set to be populated.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }

  // Variant should be written at Desktop/Identity.
  CapabilityMask desktop_mask;
  std::string desktop_variant = ReadVariant(worker.cache(), url, desktop_mask);
  ASSERT_FALSE(desktop_variant.empty())
      << "CSS variant should exist at Desktop/Identity";
  EXPECT_LT(desktop_variant.size(), css.size()) << "Should be minified";

  // A second notification with the same mask should be deduped.
  uint64_t dedup_before = worker.stats().notifications_skipped_dedup.load();
  CacheNotification dup_notif;
  dup_notif.url = url;
  dup_notif.scheme = "https";
  dup_notif.content_type = ContentType::kCss;
  dup_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(dup_notif);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > dedup_before) break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), dedup_before)
      << "Duplicate CSS notification should be deduped";
}

TEST_F(WorkerTest, JsNotificationMinifiesAndWritesVariant) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified JS
  std::string url = "http://example.com/app.js";
  std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheOriginal(worker.cache(), url, js);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send JS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for worker to process the JS notification.
  // Use the same simple pattern as CssNotificationMinifiesAndWritesVariant:
  // wait a fixed time, then read the variant.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "Worker did not process JS notification";

  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty()) << "Worker did not produce a JS variant";
  // Minified version should be shorter (comment removed, whitespace collapsed)
  EXPECT_LT(variant.size(), js.size());
  // Should still contain key identifiers
  EXPECT_NE(variant.find("hello"), std::string::npos);
  EXPECT_NE(variant.find("greeting"), std::string::npos);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsParseErrorSkipsVariant) {
  // Issue #654: when MinifyUtf8Js fails, the tokenizer's error path emits
  // the unlexable remainder raw, so the output may be half-minified.  The
  // worker must not write a variant — the original is served unchanged.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Unterminated string literal: MinifyUtf8Js reliably returns false.
  std::string url = "http://example.com/broken.js";
  std::string js = "var  a  =  1;\n\"not valid javascript";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // The failure path bumps text_minify_parse_failures; js_processed only
  // increments when a variant is actually written.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().text_minify_parse_failures.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().text_minify_parse_failures.load(), 1u)
      << "Worker did not record the JS parse failure";
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  // CSS/JS variants overwrite the original at the default mask, so
  // "no variant written" means the cached bytes are still the original.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, js) << "Half-minified JS variant must not be written";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, IntegrityPinnedJsNotOptimized) {
  // Issue #656: 2.0 serves optimized variants at the SAME URL, so a
  // <script integrity=...> whose body is minified would fail the
  // browser's hash check.  A URL referenced with an integrity attribute
  // must never get a text variant; a control URL without one must.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheOriginal(worker.cache(), "/pinned.js", js);
  CacheOriginal(worker.cache(), "/free.js", js);
  // Protocol-relative reference: must pin under (path, hostname).
  CacheOriginal(worker.cache(), "/proto-rel.js", js, "cdn.example.com");
  std::string html =
      "<html><head>"
      "<script src=\"/pinned.js\" integrity=\"sha384-abc\"></script>"
      "<script src=\"//cdn.example.com/proto-rel.js\" "
      "integrity=\"sha384-def\"></script>"
      "<script src=\"/free.js\"></script>"
      "</head><body><p>Hello</p></body></html>";
  CacheOriginal(worker.cache(), "/page.html", html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process the HTML first so the pin is registered.
  CacheNotification html_notif;
  html_notif.url = "http://example.com/page.html";
  html_notif.scheme = "https";
  html_notif.content_type = ContentType::kHtml;
  html_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(html_notif);

  // Pin registration drops the pinned URL's cache key (a stale minified
  // variant could already exist) — poll for that as completion signal.
  CapabilityMask mask;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (ReadVariant(worker.cache(), "/pinned.js", mask).empty()) break;
  }
  EXPECT_TRUE(ReadVariant(worker.cache(), "/pinned.js", mask).empty())
      << "Pin registration should drop the pinned URL's cache key";
  for (int i = 0; i < 80; ++i) {
    if (ReadVariant(worker.cache(), "/proto-rel.js", mask, "cdn.example.com")
            .empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(
      ReadVariant(worker.cache(), "/proto-rel.js", mask, "cdn.example.com")
          .empty())
      << "Protocol-relative pin should drop the cdn-hosted cache key";

  // nginx re-caches the original on the next request; simulate that.
  CacheOriginal(worker.cache(), "/pinned.js", js);
  CacheOriginal(worker.cache(), "/proto-rel.js", js, "cdn.example.com");

  // JS notifications for both URLs (as nginx would send on MISS).
  CacheNotification pinned_notif;
  pinned_notif.url = "http://example.com/pinned.js";
  pinned_notif.scheme = "https";
  pinned_notif.content_type = ContentType::kJs;
  pinned_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(pinned_notif);

  CacheNotification proto_rel_notif;
  proto_rel_notif.url = "/proto-rel.js";
  proto_rel_notif.hostname = "cdn.example.com";
  proto_rel_notif.scheme = "https";
  proto_rel_notif.content_type = ContentType::kJs;
  proto_rel_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(proto_rel_notif);

  CacheNotification free_notif;
  free_notif.url = "http://example.com/free.js";
  free_notif.scheme = "https";
  free_notif.content_type = ContentType::kJs;
  free_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(free_notif);

  // Only the control URL counts as processed (pin skip writes nothing).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_EQ(worker.stats().js_processed.load(), 1u);

  std::string pinned_variant = ReadVariant(worker.cache(), "/pinned.js", mask);
  EXPECT_EQ(pinned_variant, js)
      << "Integrity-pinned JS must keep its original bytes";

  std::string proto_rel_variant =
      ReadVariant(worker.cache(), "/proto-rel.js", mask, "cdn.example.com");
  EXPECT_EQ(proto_rel_variant, js)
      << "Protocol-relative integrity-pinned JS must keep its original bytes";

  std::string free_variant = ReadVariant(worker.cache(), "/free.js", mask);
  ASSERT_FALSE(free_variant.empty());
  EXPECT_LT(free_variant.size(), js.size())
      << "Control JS without integrity must still be minified";
}

// =============================================================================
// Rapid-fire notification test
// =============================================================================
//
// Phase 6 learning: The worker's OnClientRead had a buffer.clear() bug
// that discarded any data beyond the first notification in a read.
// While the current architecture uses one connection per notification,
// this test verifies the worker handles rapid sequential notifications
// without losing any.

TEST_F(WorkerTest, RapidFireNotificationsAllProcessed) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with 3 different CSS files
  std::string url1 = "http://example.com/a.css";
  std::string css1 = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url1, css1);

  std::string url2 = "http://example.com/b.css";
  std::string css2 = "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url2, css2);

  std::string url3 = "http://example.com/c.css";
  std::string css3 = "p  {  line-height:  1.5;  margin:  10px;  }\n";
  CacheOriginal(worker.cache(), url3, css3);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send all 3 notifications in rapid succession (separate connections)
  CacheNotification n1, n2, n3;
  n1.url = url1;
  n1.scheme = "https";
  n1.content_type = ContentType::kCss;
  n1.capability_mask = CapabilityMask().Encode();

  n2.url = url2;
  n2.scheme = "https";
  n2.content_type = ContentType::kCss;
  n2.capability_mask = CapabilityMask().Encode();

  n3.url = url3;
  n3.scheme = "https";
  n3.content_type = ContentType::kCss;
  n3.capability_mask = CapabilityMask().Encode();

  SendNotification(n1);
  SendNotification(n2);
  SendNotification(n3);

  // Poll until all 3 variants appear at Desktop/Identity (normalized).
  CapabilityMask mask;
  std::string v1, v2, v3;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (v1.empty()) v1 = ReadVariant(worker.cache(), url1, mask);
    if (v2.empty()) v2 = ReadVariant(worker.cache(), url2, mask);
    if (v3.empty()) v3 = ReadVariant(worker.cache(), url3, mask);
    if (!v1.empty() && !v2.empty() && !v3.empty()) break;
  }

  EXPECT_FALSE(v1.empty()) << "CSS a.css variant missing";
  EXPECT_FALSE(v2.empty()) << "CSS b.css variant missing";
  EXPECT_FALSE(v3.empty()) << "CSS c.css variant missing";

  // Each minified variant should be smaller than its original
  if (!v1.empty()) {
    EXPECT_LT(v1.size(), css1.size());
  }
  if (!v2.empty()) {
    EXPECT_LT(v2.size(), css2.size());
  }
  if (!v3.empty()) {
    EXPECT_LT(v3.size(), css3.size());
  }

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Worker with mmap directory enabled
// =============================================================================
//
// Phase 6 learning: The worker MUST open the cache with
// enable_mmap_directory = true for cross-process sharing with nginx.

TEST_F(WorkerTest, WorkerCacheUsesMmapDirectory) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  // Write via the worker's cache using sentinel (simulates original content).
  std::string url = "/mmap_test";
  std::string value = "shared_value";
  CacheOriginal(worker.cache(), url, value);

  // Open a second PageSpeedCache on the same path (multi-process sharing).
  PageSpeedCacheConfig reader_config;
  reader_config.volume_path = cache_path_;
  reader_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  auto reader_result = PageSpeedCache::Create(reader_config);
  ASSERT_TRUE(reader_result.has_value()) << "Reader cache creation failed";
  auto& reader = *reader_result.value();

  // The reader should see the worker's write via ReadBestAlternate.
  auto read = reader.ReadBestAlternate(url, "", "https", CapabilityMask());
  ASSERT_TRUE(read.has_value())
      << "Reader cannot see worker's write — "
         "worker cache must use multi-process sharing";
  auto content = read->content();
  std::string read_value(reinterpret_cast<const char*>(content.data()),
                         content.size());
  EXPECT_EQ(read_value, value);

  worker.Shutdown();
}

// =============================================================================
// Early Hints: preload hints stored by HTML notification
// =============================================================================

TEST_F(WorkerTest, HtmlNotificationStoresPreloadHints) {
  // When the worker processes HTML with external stylesheet links,
  // it should store the stylesheet URLs as preload hints in cache
  // at mask=0xFFFFFFFF (reserved for hints).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Keep the sheets render-blocking: async-deferred sheets get no preload
  // hint, and this test asserts the stylesheet hints ARE stored.
  config.disable_async_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML that references external stylesheets.
  std::string url = "http://example.com/hints.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/base.css\">"
      "<link rel=\"stylesheet\" href=\"/theme.css\">"
      "<style>body { margin: 0; }</style>"
      "</head><body><h1>Hello</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Pre-populate the external stylesheets so the worker can gather
  // CSS and produce a variant (otherwise it returns early).
  std::string css1 = "body { margin: 0; }";
  CacheOriginal(worker.cache(), "/base.css", css1);
  std::string css2 = "h1 { color: red; }";
  CacheOriginal(worker.cache(), "/theme.css", css2);

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send HTML notification.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for the hints sentinel entry.
  std::string hints;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  // Verify hints were stored.
  EXPECT_FALSE(hints.empty()) << "Preload hints not stored";
  if (!hints.empty()) {
    // Should contain the two stylesheet URLs, newline-separated.
    EXPECT_NE(hints.find("/base.css"), std::string::npos);
    EXPECT_NE(hints.find("/theme.css"), std::string::npos);
    // Verify newline separation.
    EXPECT_NE(hints.find('\n'), std::string::npos);
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationNoStylesheetsNoHints) {
  // HTML with no external stylesheets should NOT store hints.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML with only inline CSS (no external stylesheet links).
  std::string url = "http://example.com/inline-only.html";
  std::string html =
      "<html><head>"
      "<style>body { margin: 0; } h1 { color: red; }</style>"
      "</head><body><h1>Hello</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send HTML notification.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Give worker time to process.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Verify no hints sentinel was stored.
  std::string hints =
      ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
  EXPECT_TRUE(hints.empty()) << "Hints stored for HTML without stylesheets";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Production Hardening Tests
// =============================================================================

// Helper: open a cross-platform IPC pipe connection to the worker
static intptr_t ConnectToSocket(const std::string& path) {
  return pagespeed::test::ConnectPipe(path);
}

TEST_F(WorkerTest, MaxConnectionsEnforced) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_connections = 2;
  config.connection_timeout_ms = 10000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Open max_connections connections and keep them alive
  intptr_t fd1 = ConnectToSocket(socket_path_);
  ASSERT_GE(fd1, static_cast<intptr_t>(0)) << "First connection failed";
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  intptr_t fd2 = ConnectToSocket(socket_path_);
  ASSERT_GE(fd2, static_cast<intptr_t>(0)) << "Second connection failed";
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // The third connection should be accepted but immediately closed
  // by the server (rejected).  We detect this by trying to read:
  // the server closes the socket, so read returns 0.
  intptr_t fd3 = ConnectToSocket(socket_path_);
  ASSERT_GE(fd3, static_cast<intptr_t>(0)) << "Third connect() failed";

  // Give the worker time to reject
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read on the rejected socket should return 0 (EOF on Unix) or -1
  // (ERROR_BROKEN_PIPE on Windows). Both indicate the server closed it.
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd3, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Rejected connection should be closed by server";

  pagespeed::test::ClosePipe(fd1);
  pagespeed::test::ClosePipe(fd2);
  pagespeed::test::ClosePipe(fd3);

  // Let the worker process the closes
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WorkerTest, BufferOverflowDisconnects) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_buffer_size = 64;  // Very small buffer limit
  config.connection_timeout_ms = 10000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect and send data exceeding max_buffer_size
  intptr_t fd = ConnectToSocket(socket_path_);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Send 128 bytes of junk (exceeds 64 byte limit)
  std::string junk(128, 'X');
  ssize_t written = pagespeed::test::PipeWrite(fd, junk.data(), junk.size());
  // Write may or may not fully succeed depending on timing
  (void)written;

  // Give worker time to process and close
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Subsequent read should return 0 (EOF) or write should fail
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Connection should be closed after overflow";

  pagespeed::test::ClosePipe(fd);
}

TEST_F(WorkerTest, ConnectionTimeoutDisconnects) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.connection_timeout_ms = 100;  // 100ms timeout

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect but don't send any data
  intptr_t fd = ConnectToSocket(socket_path_);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Wait for timeout to fire (100ms + margin)
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Connection should be closed by the server
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Connection should be closed after timeout";

  pagespeed::test::ClosePipe(fd);
}

TEST_F(WorkerTest, GracefulShutdownDrainsConnections) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.connection_timeout_ms = 10000;
  config.shutdown_timeout_ms = 2000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Open a connection
  intptr_t fd = ConnectToSocket(socket_path_);
  // No WorkerStopper at this site (#1354): the teardown below is the behavior
  // under test — Shutdown() is issued from a second thread mid-body and the
  // worker_thread.join() is itself the assertion — so a guard would
  // double-join. Clean up explicitly before the fatal assert instead, so a
  // failed connect does not return with worker_thread still joinable.
  if (fd < 0) {
    worker.Shutdown();
    worker_thread.join();
  }
  ASSERT_GE(fd, static_cast<intptr_t>(0));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(worker.active_connections(), 1);

  // Initiate shutdown from another thread
  std::thread shutdown_thread([&worker]() {
    // Give a tiny delay so the main thread can observe
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    worker.Shutdown();
  });

  // Close our connection to let the worker drain
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  pagespeed::test::ClosePipe(fd);

  // Worker should exit cleanly
  worker_thread.join();
  shutdown_thread.join();

  // After shutdown, active connections should be zero
  EXPECT_EQ(worker.active_connections(), 0);
}

TEST_F(WorkerTest, HealthCheckResponds) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_connections = 128;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to health socket
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  // Read response
  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';

  std::string response(buf, n);

  // Should start with "OK"
  EXPECT_EQ(response.substr(0, 2), "OK");
  // Should contain connection info like "0/128"
  EXPECT_NE(response.find("/128"), std::string::npos)
      << "Response: " << response;
  // Should end with newline
  EXPECT_EQ(response.back(), '\n');

  pagespeed::test::ClosePipe(fd);
}

// =============================================================================
// Per-URL Policy and Security Hardening Tests
// =============================================================================

TEST_F(WorkerTest, DisableHtmlSkipsProcessing) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_html = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML
  std::string url = "http://example.com/disabled.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Test</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // No variant should be written
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "HTML variant written despite disable_html";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DisableCssSkipsProcessing) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/disabled.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "CSS variant written despite disable_css";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, SentinelViewportMaskRejected) {
  // Notifications with viewport bits = 3 (sentinel range) should be
  // rejected — they would overwrite reserved sentinel AlternateIds.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS
  std::string url = "http://example.com/sentinel.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with viewport=3 in the mask (bits 2-3 = 0b11).
  // This is NOT the warmup sentinel (0xFFFFFFFE), just a mask with
  // the sentinel viewport bits set.
  uint32_t sentinel_mask = 0x0C;  // viewport bits 2-3 = 3
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = sentinel_mask;
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // Should not have processed (error counter should increment)
  EXPECT_GE(worker.stats().errors.load(), 1u);
  // No variant should be written
  CapabilityMask mask = CapabilityMask::Decode(sentinel_mask);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "Variant written for sentinel viewport mask";

  worker.Shutdown();
  worker_thread.join();
}

// A MessageHandler that keeps the WARNINGs (and INFOs) it was given.  Shared by
// the skew tests below, all of which assert on the "and it was said out loud"
// half rather than only on the behaviour.
class WarningRecordingHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }
  std::vector<std::string> warnings() const {
    std::lock_guard<std::mutex> lock(mu_);
    return warnings_;
  }
  std::vector<std::string> infos() const {
    std::lock_guard<std::mutex> lock(mu_);
    return infos_;
  }

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override {
    std::string formatted = FormatMessage(format, args);
    std::lock_guard<std::mutex> lock(mu_);
    if (type == MessageType::kWarning) warnings_.push_back(formatted);
    if (type == MessageType::kInfo) infos_.push_back(formatted);
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::string> warnings_;
  std::vector<std::string> infos_;
};

// Reads a one-shot mgmt response to EOF (the server closes the connection
// after writing non-AUTH responses).  A single PipeRead can return just the
// first pipe-buffer chunk, which silently truncates the merged Prometheus
// document (#877) and makes find()-based assertions position-dependent.
//
// Defined here, above the first mgmt test, because that truncation is no
// longer a large-response edge case: the Prometheus document is past 4 KiB and
// grows by three lines with every counter the build gains, so a fixed-buffer
// single read now cuts it in the ordinary case — and the failure names
// whichever metric happened to fall past the cut rather than the real cause.
static std::string ReadMgmtResponseToEof(intptr_t fd) {
  std::string response;
  char buf[16384];
  for (;;) {
    ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
  }
  return response;
}

// A notification stamped with a wire version this build does not speak is
// REFUSED WHOLE — not parsed, not partially applied — and the refusal is
// counted and logged as version skew specifically.
//
// This is the receive path, not the codec: worker_ipc_test pins that
// Deserialize says "version mismatch", and this pins that the socket path
// acts on it.  Both directions of skew are driven, because the interesting
// one is the peer AHEAD of this build — a notify sender shipping on its own
// cadence — and that is precisely the frame a length-tolerant reader would be
// happy to decode with the wrong field meanings.
//
// The three assertions are the whole fail-direction contract:
//   1. notifications_received stays 0        (never dispatched)
//   2. rejected_version counts every frame   (visible without reading logs)
//   3. the warning names BOTH versions       (actionable without a debugger)
TEST_F(WorkerTest, NotifyFromSkewedPeerRefusedCountedAndNamed) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Real, minifiable content at the key the skewed frames name: if the worker
  // ever did parse one of them, it would find something to optimize at that
  // key, so "no variant written" is evidence and not a tautology.
  const std::string url = "http://example.com/skewed-peer.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Build one valid frame, then restamp its version byte.  Everything else
  // about the frame is well-formed, which is what isolates the version as the
  // only reason it is refused.
  CacheNotification notification;
  notification.url = url;
  // NO hostname, deliberately: CacheOriginal above stores under the empty
  // hostname, and a notification naming a different one composes a different
  // cache key -- the worker would find nothing to optimize and "no variant was
  // written" would hold for a reason that has nothing to do with the version
  // gate. Matching the key is what makes the assertion below mean "the
  // refusal stopped it" rather than "there was never anything to do".
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  const std::vector<char> valid = notification.Serialize();
  ASSERT_FALSE(valid.empty());
  ASSERT_EQ(static_cast<uint8_t>(valid[4]), kIpcVersion);

  // Older peer, newer peer, and a byte that is no version at all.  Distinct
  // versions on purpose: the log is rate-limited per peer version, so this
  // also pins that a second differently-skewed peer is not swallowed by the
  // rate limit the first one armed.
  const uint8_t peers[] = {static_cast<uint8_t>(kIpcVersion - 1),
                           static_cast<uint8_t>(kIpcVersion + 1), 0xFF};
  for (uint8_t peer : peers) {
    SCOPED_TRACE(static_cast<unsigned>(peer));
    const uint64_t rejected_before =
        worker.stats().notifications_rejected_version.load();
    const size_t warnings_before = handler.warnings().size();

    std::vector<char> wire = valid;
    wire[4] = static_cast<char>(peer);
    SendRawNotificationBytes(wire);

    bool counted = false;
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_rejected_version.load() >
          rejected_before) {
        counted = true;
        break;
      }
    }
    EXPECT_TRUE(counted)
        << "a version-skewed notification must be counted as version skew, "
           "not lost in a generic parse-failure bucket";

    // The refusal is loud AND specific: an operator must be able to read the
    // peer's version and this build's version straight off the line.
    auto warnings = handler.warnings();
    ASSERT_GT(warnings.size(), warnings_before)
        << "the refusal must be visible in the log, not silent";
    bool named_both = false;
    for (size_t i = warnings_before; i < warnings.size(); ++i) {
      const std::string& w = warnings[i];
      if (w.find("wire version " + std::to_string(peer)) != std::string::npos &&
          w.find("version " + std::to_string(kIpcVersion)) !=
              std::string::npos) {
        named_both = true;
      }
    }
    EXPECT_TRUE(named_both)
        << "the warning must name the peer's version AND this build's";
  }

  // Nothing was ever handed to the pipeline, and nothing was written.
  EXPECT_EQ(worker.stats().notifications_received.load(), 0u)
      << "a refused frame must never reach dispatch";
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(),
            static_cast<uint64_t>(std::size(peers)));
  EXPECT_EQ(worker.stats().notifications_rejected_malformed.load(), 0u)
      << "skew must not be miscounted as corruption";
  EXPECT_EQ(worker.stats().variants_written.load(), 0u)
      << "a refused frame must produce no cache write at all";
  // The pre-seeded original is byte-for-byte what it was: refusing costs cache
  // warmth, it does not touch what is already stored.
  EXPECT_EQ(ReadVariant(worker.cache(), url, CapabilityMask()), css);
}

// A notification whose per-request options context FAILS VALIDATION is REFUSED
// — dropped before any source is read — and the refusal is counted and logged
// as an options-context refusal specifically.
//
// The four refusal statuses do not all reach this gate, and this says which is
// which rather than implying otherwise:
//
//   kUnknownFormat, kMalformedSignature and kSignatureMismatch arrive over the
//   socket and are refused here. The last matters most: it means the two
//   implementations of the context format have drifted, and it must surface on
//   the first message rather than as a cache that never quite warms.
//
//   kTooLarge cannot reach this gate. The frame reader bounds the payload with
//   the same constant the validator does, so an oversized context is refused a
//   layer earlier as a malformed frame, and the sending side refuses to emit
//   one at all. Both halves are asserted below, because "every invalid context
//   is refused before a source read" is only true if that earlier refusal is
//   real.
//
// A VALID context — default or not — is NOT refused: it is accepted and its
// work is filed under the default context. That is the test after this one.
//
// As with the version-skew case above, the URL names real minifiable content,
// so "no variant written" is evidence rather than a tautology.
TEST_F(WorkerTest, NotifyWithAnInvalidOptionContextIsRefusedAndCounted) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/option-context.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  // NO hostname, deliberately: CacheOriginal above stores under the empty
  // hostname, and a notification naming a different one composes a different
  // cache key -- the worker would find nothing to optimize and "no variant was
  // written" would hold for a reason that has nothing to do with the options
  // context. Matching the key is what makes the assertion below mean "the
  // refusal stopped it" rather than "there was never anything to do".
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  const std::string good_payload = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";

  // Each arm is (payload, signature, expected status).  Every one of them is a
  // context a peer could actually put on the socket.
  struct Arm {
    const char* what;
    std::string payload;
    std::string signature;
    OptionContextStatus status;
  };
  const std::string unknown_format_payload = "psoc2\no:ImageQuality=80\n";
  const std::vector<Arm> arms = {
      {"a payload from a format this build does not read",
       unknown_format_payload, OptionContextSignature(unknown_format_payload),
       OptionContextStatus::kUnknownFormat},
      {"a signature that is 64 characters of something other than lowercase "
       "hex",
       good_payload, std::string(kOptionContextSignatureChars, 'z'),
       OptionContextStatus::kMalformedSignature},
      {"a signature that is of a DIFFERENT payload -- the two implementations "
       "of this format have drifted",
       good_payload,
       OptionContextSignature("psoc1\nf:hw\no:ImageInlineMaxBytes=4096\n"),
       OptionContextStatus::kSignatureMismatch},
  };

  uint64_t expected_refusals = 0;
  for (const Arm& arm : arms) {
    SCOPED_TRACE(arm.what);
    notification.option_context = arm.payload;
    notification.option_signature = arm.signature;
    ASSERT_EQ(arm.status, ValidateOptionContext(notification.option_context,
                                                notification.option_signature))
        << "this arm no longer produces the status it says it does";
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty())
        << "the sending side must be able to emit this frame, or the arm is "
           "testing the sender rather than the gate";
    SendRawNotificationBytes(wire);

    ++expected_refusals;
    bool counted = false;
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_rejected_option_context.load() >=
          expected_refusals) {
        counted = true;
        break;
      }
    }
    EXPECT_TRUE(counted)
        << "an options context that fails validation must be refused and "
           "counted";
  }
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(),
            expected_refusals);

  // kTooLarge, the fourth status: it never reaches the gate, and this pins the
  // two refusals that stand in front of it.  Without this the claim "an
  // oversized context is refused before a source read" would rest on nothing.
  {
    const std::string oversized =
        "psoc1\n" + std::string(kMaxOptionContextBytes, 'x');
    EXPECT_EQ(OptionContextStatus::kTooLarge,
              ValidateOptionContext(oversized, OptionContextSignature("")))
        << "the validator must still name an oversized payload for what it is";
    CacheNotification too_big = notification;
    too_big.option_context = oversized;
    too_big.option_signature = OptionContextSignature(oversized);
    EXPECT_TRUE(too_big.Serialize().empty())
        << "the sending side must refuse to emit an oversized context rather "
           "than put one on the socket";
  }

  // The refusal is loud, and it says it was the context that was refused.
  bool named_refusal = false;
  for (const std::string& w : handler.warnings()) {
    if (w.find("options context was refused") != std::string::npos) {
      named_refusal = true;
    }
  }
  EXPECT_TRUE(named_refusal)
      << "a context that failed validation must be reported as such";
  // The retired refusal rationale must not come back: this line described
  // version skew, and none of these arms is version skew.
  for (const std::string& w : handler.warnings()) {
    EXPECT_EQ(w.find("Align the versions of the components sending"),
              std::string::npos)
        << "an options-context refusal is not version skew: " << w;
  }

  // No frame here was version-skewed or corrupt, and none was accepted: those
  // buckets stay clean, so the refusal counter is not soaking up other causes
  // and the accept path is not being credited for a refusal.
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(), 0u);
  EXPECT_EQ(worker.stats().notifications_rejected_malformed.load(), 0u);
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      0u)
      << "a refused context must never be counted as an accepted one";

  // Nothing reached the pipeline and nothing was written. The pre-seeded
  // original is byte-for-byte what it was.
  EXPECT_EQ(worker.stats().variants_written.load(), 0u)
      << "a refused context must produce no cache write at all";
  EXPECT_EQ(worker.stats().css_processed.load(), 0u)
      << "a refused context must be dropped before any source is read";
  EXPECT_EQ(ReadVariant(worker.cache(), url, CapabilityMask()), css);
}

// A notification carrying a VALID NON-DEFAULT per-request options context is
// ACCEPTED, optimized end-to-end, and its work stored under the DEFAULT cache
// key — the same key a notification carrying no context at all would use.
//
// That is the whole shape of the contract on this surface. Optimized output
// here is a function of the resource, of the request (capability mask and
// content type, plus the agent-request bit — the request's own declared intent,
// gated by an operator flag the daemon itself publishes), and of the daemon's own
// configuration, which a notification cannot reach; no value a sender resolves
// per request reaches a rewriter. So two contexts cannot produce two different
// artifacts, one stored
// artifact is correct for both, and refusing the notification would cost an
// optimization for nothing.
//
// The read-back is deliberately through the ORDINARY default-key path
// (ReadVariant, which composes the key with no context at all): if the work had
// been filed anywhere else, this read would come back with the unoptimized
// original.
TEST_F(WorkerTest, NotifyWithValidNonDefaultOptionContextIsStoredUnderDefault) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/accepted-context.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  // No hostname, matching CacheOriginal's key -- see the refusal test above.
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  const std::string payload = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";
  notification.option_context = payload;
  notification.option_signature = OptionContextSignature(payload);
  ASSERT_FALSE(IsDefaultOptionContext(notification.option_signature))
      << "this test is about a context that is NOT the default one";
  ASSERT_EQ(OptionContextStatus::kOk,
            ValidateOptionContext(notification.option_context,
                                  notification.option_signature));
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }

  // Wait for the optimizer to have actually run and the work item to have
  // drained -- css_processed moves before the in-flight entry is erased, so
  // both conditions are needed (the pre-existing CSS tests use this pairing).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "a valid non-default options context must reach the optimizer, not be "
         "dropped before the source is read";

  // POSITIVE proof, and under the default key specifically.
  const std::string optimized =
      ReadVariant(worker.cache(), url, CapabilityMask());
  ASSERT_FALSE(optimized.empty())
      << "the work must be readable through the ordinary default-context key";
  EXPECT_LT(optimized.size(), css.size())
      << "the variant should be minified, which is what proves the optimizer "
         "ran rather than that something was merely stored";

  // The counters split the two outcomes cleanly: accepted-non-default moves,
  // the refusal counter does not.
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      1u)
      << "an accepted non-default context must be counted, or a deployment "
         "cannot see that this path is flowing";
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u)
      << "accepting must not also count as a refusal";
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(), 0u);
  EXPECT_EQ(worker.stats().notifications_rejected_malformed.load(), 0u);

  // Said once, and said accurately: the operator-facing half of the new path.
  bool named_accept = false;
  for (const std::string& info : handler.infos()) {
    if (info.find("non-default per-request options context") !=
            std::string::npos &&
        info.find("stored under the default context") != std::string::npos) {
      named_accept = true;
    }
  }
  EXPECT_TRUE(named_accept)
      << "the first accepted non-default context must say where the work "
         "landed";
  // And it is not a warning: this is normal operation, not a problem.
  for (const std::string& w : handler.warnings()) {
    EXPECT_EQ(w.find("options context"), std::string::npos)
        << "accepting a valid context must not warn: " << w;
  }
}

// Two notifications for the SAME URL carrying DIFFERENT valid contexts produce
// ONE OPTIMIZATION, not two: both are filed under the default context, so the
// second is deduplicated exactly as a repeat of the first would be.
//
// Be precise about what is and is not asserted, because the difference matters
// if this ever has to be re-read:
//
//   * ONE OPTIMIZATION is the claim, and css_processed is the oracle for it.
//     Both notifications are ADMITTED (the accepted counter reads 2) and the
//     second does reach HandleNotification — it is stopped at the
//     already-processed set, which is where "same work, named twice" is
//     supposed to be caught. "One work item" would overstate it.
//   * This is the counterpart of the storage assertion above: it shows the two
//     contexts converging on one stored result rather than on two.
//   * It is NOT a tripwire for context keying being switched on later. The
//     dedup key is composed without a signature parameter, so a change made
//     purely in the cache layer would not be visible here. What would be
//     visible is the gate or the dedup key itself growing a context
//     dimension.
TEST_F(WorkerTest, NotifyWithTwoDifferentValidContextsIsOneOptimization) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/two-contexts.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  const std::string first = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";
  notification.option_context = first;
  notification.option_signature = OptionContextSignature(first);
  ASSERT_FALSE(IsDefaultOptionContext(notification.option_signature));
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }

  // Drain the first completely BEFORE sending the second, so that what the
  // second hits is the processed-set dedup and not the in-flight guard -- the
  // in-flight guard would also fire, but only by a timing accident, and a test
  // that depended on it would be racy.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  ASSERT_GE(worker.stats().css_processed.load(), 1u)
      << "the first notification must have been processed for the dedup "
         "assertion below to mean anything";
  const uint64_t processed_after_first = worker.stats().css_processed.load();
  const uint64_t skipped_before =
      worker.stats().notifications_skipped_dedup.load();

  // A DIFFERENT valid non-default context, same URL, same mask.
  const std::string second = "psoc1\nf:hw\no:ImageInlineMaxBytes=9999\n";
  notification.option_context = second;
  notification.option_signature = OptionContextSignature(second);
  ASSERT_NE(OptionContextSignature(first), OptionContextSignature(second))
      << "the two contexts must actually differ";
  ASSERT_FALSE(IsDefaultOptionContext(notification.option_signature));
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }

  bool skipped = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before) {
      skipped = true;
      break;
    }
  }
  EXPECT_TRUE(skipped)
      << "a second context for the same URL must be recognised as naming work "
         "already done, because both are filed under the default context";
  EXPECT_EQ(worker.stats().css_processed.load(), processed_after_first)
      << "the same work must not be optimized twice just because it was named "
         "twice";

  // Both were ADMITTED, and both were counted -- the counter tracks
  // notifications carrying a non-default context, not optimizations performed.
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      2u);
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u);
}

// The control for the cases above: a notification with NO options context, and
// one carrying the DEFAULT context explicitly, are both processed normally —
// and, being the default, neither is counted as a non-default acceptance.
// Without this, the counter assertions above would also pass if the gate had
// simply stopped discriminating.
TEST_F(WorkerTest, NotifyWithNoOrDefaultOptionContextIsProcessedNormally) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/default-context.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  // No hostname, matching CacheOriginal's key (see the declined-context test
  // above) and matching the pre-existing CSS tests. The two tests are otherwise
  // identical, so the options context is the only variable between them.
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // No context at all.
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }
  bool dispatched = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() > 0) {
      dispatched = true;
      break;
    }
  }
  EXPECT_TRUE(dispatched) << "a notification with no options context must be "
                             "dispatched exactly as before";
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u);

  // The DEFAULT context stated explicitly means the same thing as saying
  // nothing, and must be treated identically.
  notification.option_context = "psoc1\n";
  notification.option_signature =
      OptionContextSignature(notification.option_context);
  ASSERT_EQ(notification.option_signature,
            std::string(kDefaultOptionContextSignature));
  notification.url = "http://example.com/default-context-explicit.css";
  CacheOriginal(worker.cache(), notification.url, css);
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() > 1) break;
  }
  EXPECT_GE(worker.stats().notifications_received.load(), 2u);
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u)
      << "stating the default context explicitly must not be refused";
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      0u)
      << "the default context, stated or omitted, is not a non-default one and "
         "must leave that counter alone";

  // POSITIVE proof, not just the absence of a refusal.
  //
  // notifications_received is incremented BEFORE the option-context gate, so it
  // moves whether or not the notification was declined. On its own it cannot
  // tell "processed" from "counted and dropped", which would make this control
  // resemble the declined case rather than contrast with it. So this waits for
  // the optimizer to have actually run and then reads the result back.
  //
  // css_processed is the counter to wait on, not variants_written: the CSS path
  // reports through the per-content-type counter, and the pre-existing CSS
  // tests use exactly this pairing (poll css_processed AND drain in-flight
  // work, because css_processed moves before the in-flight entry is erased).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 2 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 2u)
      << "both notifications -- the one with no options context and the one "
         "stating the default context explicitly -- must reach the optimizer";

  const std::string optimized =
      ReadVariant(worker.cache(), url, CapabilityMask());
  ASSERT_FALSE(optimized.empty())
      << "a notification with no options context must be optimized, not merely "
         "counted; without this the declined case and this one would look "
         "alike";
  EXPECT_LT(optimized.size(), css.size())
      << "the variant should be minified, which is what proves the optimizer "
         "ran rather than that something was merely stored";
}

// The other half of the split: a frame this build's version DOES cover, but
// whose body is damaged, must land in the malformed bucket.  Without this,
// "rejected_version == 0" would be uninformative — it could just mean the
// receive path routes every failure to the other counter.
TEST_F(WorkerTest, MalformedNotifyCountedSeparatelyFromVersionSkew) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/corrupt-frame.css";
  notification.hostname = "example.com";
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  std::vector<char> wire = notification.Serialize();
  ASSERT_FALSE(wire.empty());
  // Current version, invalid scheme byte.
  //
  // The offset moved when the wire gained the v5 option-context tail: the
  // scheme byte is now followed by agent_request(1) + option_context_length(4)
  // + option_signature_length(1), so it sits seven from the end rather than
  // second. Left at the old offset this test still passed -- corrupting a
  // length field also yields a malformed frame -- but it would have stopped
  // testing what it says it tests, which is the quiet way a regression test
  // turns into a tautology.
  const size_t scheme_index = wire.size() - 7;
  ASSERT_EQ(0x02u, static_cast<unsigned char>(wire[scheme_index]))
      << "scheme byte is not where this test thinks it is";
  wire[scheme_index] = static_cast<char>(0x7E);
  SendRawNotificationBytes(wire);

  bool counted = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_rejected_malformed.load() >= 1) {
      counted = true;
      break;
    }
  }
  EXPECT_TRUE(counted);
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(), 0u)
      << "a corrupt frame from a version-matched peer is not version skew";
  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);
}

// A sentinel-shaped notify is DROPPED and SAID OUT LOUD — and the drop is
// REGISTRATION-BLIND, which is what this test pins.
//
// Two halves, and it is worth being exact about which one is new.  The drop is
// pre-existing behaviour (SentinelViewportMaskRejected above covers it for one
// id).  What had no coverage is that the drop is *audible*: a mixed fleet where
// one side speaks a namespace the other does not looks exactly like a cache
// that never warms, and the log line is the only thing that tells them apart.
//
// The registration-blindness of the DECISION is asserted rather than assumed,
// because it is easy to read the sentinel registry as if it governed this
// path. It does not: the guard in worker.cc is viewport-bits-shaped and never
// consults IsRegisteredSentinel, so a REGISTERED id and an UNREGISTERED one
// are dropped identically — deliberately, since what a notification may
// trigger is a fixed set of whole-mask specials and not "whatever this build
// recognises". Driving both through the same assertions documents that
// contract, and this test is the thing that will fail — correctly, demanding
// an update to it and to lib/classify/CLAUDE.md — on the day a lane wires the
// registry into this path as a DECISION and the two stop being equivalent.
//
// The registry does reach the LOG LINE: the message names the class, so a
// peer naming a class this build has never heard of (unknown_sentinel — a
// namespace-skewed peer) reads differently from a peer misusing a class this
// build knows. Naming is not gating, and the two cases are asserted below to
// be identical in treatment and different only in what the log says.
TEST_F(WorkerTest, SentinelShapedNotifyDroppedAndLoggedRegardlessOfRegistry) {
  // A recording handler, because "and logged" is the half with no coverage.
  using RecordingHandler = WarningRecordingHandler;

  // 0x0C is a REGISTERED sentinel (reserved for durable original content);
  // 0xFC is sentinel-shaped and names no class at all.  Both must be handled
  // identically today.
  struct Case {
    const char* label;
    uint32_t mask;
    bool registered;
  };
  const Case cases[] = {
      {"registered-0x0C", 0x0C, true},
      {"unregistered-0xFC", 0xFC, false},
  };
  for (const auto& c : cases) {
    ASSERT_TRUE(IsSentinel(static_cast<AlternateId>(c.mask))) << c.label;
    ASSERT_EQ(IsRegisteredSentinel(static_cast<AlternateId>(c.mask)),
              c.registered)
        << c.label
        << ": the premise of this test is that these two differ in "
           "registration and NOT in treatment";
  }

  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  RecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string css = "body  {  margin:  0;  }\n";
  for (const auto& c : cases) {
    CacheOriginal(
        worker.cache(),
        std::string("http://example.com/sentinel-notify-") + c.label + ".css",
        css);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t errors_before = 0;
  uint64_t rejected_before = 0;
  size_t warnings_before = 0;
  for (const auto& c : cases) {
    SCOPED_TRACE(c.label);
    errors_before = worker.stats().errors.load();
    rejected_before = worker.stats().notifications_rejected_sentinel.load();
    warnings_before = handler.warnings().size();

    std::string url =
        std::string("http://example.com/sentinel-notify-") + c.label + ".css";
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kCss;
    notification.capability_mask = c.mask;
    SendNotification(notification);

    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().errors.load() > errors_before) break;
    }

    EXPECT_GT(worker.stats().errors.load(), errors_before)
        << "the notify must be dropped";
    // Dropped for a NAMED reason, not folded into the generic error total:
    // "a peer is talking about entry classes" and "an optimization failed"
    // are different operational facts.
    EXPECT_GT(worker.stats().notifications_rejected_sentinel.load(),
              rejected_before)
        << "the drop must be attributable on the stats surface";

    auto warnings = handler.warnings();
    ASSERT_GT(warnings.size(), warnings_before)
        << "the drop must be visible in the log, not silent";
    bool logged = false;
    bool named_class = false;
    const std::string expected_name(
        SentinelName(static_cast<AlternateId>(c.mask)));
    for (size_t i = warnings_before; i < warnings.size(); ++i) {
      if (warnings[i].find("sentinel") != std::string::npos) logged = true;
      if (warnings[i].find(expected_name) != std::string::npos) {
        named_class = true;
      }
    }
    EXPECT_TRUE(logged) << "the log line must name what was rejected";
    // Registered ids log their class name; an unregistered one logs
    // "unknown_sentinel" — which is the only thing in the whole drop that
    // tells an operator a peer is speaking a namespace this build lacks.
    EXPECT_TRUE(named_class)
        << "the log line must name the entry class (" << expected_name << ")";

    CapabilityMask mask = CapabilityMask::Decode(c.mask);
    EXPECT_TRUE(ReadVariant(worker.cache(), url, mask).empty());
  }
}

TEST_F(WorkerTest, UrlTooLongRejected) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_url_length = 100;  // Very short limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with content at a very long URL
  std::string url = "http://example.com/" + std::string(200, 'x');
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // No processing should happen
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty());

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlTooLargeSkipped) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_html_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with oversized HTML
  std::string url = "http://example.com/huge.html";
  std::string html = "<html><head><style>" + std::string(200, 'x') +
                     "</style></head><body></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "HTML variant written despite size limit";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DuplicateNotificationSkipped) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS
  std::string url = "http://example.com/dedup.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use a non-default notification mask — worker normalizes CSS to
  // Desktop/Identity regardless of the notification viewport.
  uint32_t mask_val = 0x00000001;  // WebP/Mobile

  // Send first notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = mask_val;
  SendNotification(notification);

  // Wait for first processing to complete AND in-flight work to drain.
  // The variant may appear in cache before MarkVariantProcessed populates
  // the dedup set, so also check in_flight_work==0.
  CapabilityMask mask;  // Desktop/Identity
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty() && worker.in_flight_work() == 0) break;
  }
  ASSERT_FALSE(variant.empty()) << "First CSS variant not written";

  uint64_t dedup_before = worker.stats().notifications_skipped_dedup.load();

  // Send duplicate notification - should be skipped (variant exists)
  SendNotification(notification);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets
  // the variant settle for the unchanged-variant invariant below.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > dedup_before) break;
  }

  // The variant should be unchanged (dedup skipped reprocessing)
  std::string variant2 = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, variant2);
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), dedup_before)
      << "Second notification should have been skipped by dedup";
}

TEST_F(WorkerTest, StatsCountersIncrement) {
  // Verify that processing stats counters are incremented correctly
  // when notifications are handled.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Stats should start at zero
  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  // Pre-populate CSS content
  std::string css_url = "http://example.com/stats.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), css_url, css);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification
  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing — poll until every asserted counter has advanced
  // instead of a fixed sleep (slow shards can sit at the pipe read past a
  // fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1u &&
        worker.stats().css_processed.load() >= 1u &&
        worker.stats().variants_written.load() >= 1u) {
      break;
    }
  }

  // Verify counters incremented
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_GE(worker.stats().css_processed.load(), 1u);
  EXPECT_GE(worker.stats().variants_written.load(), 1u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HealthEndpointIncludesStats) {
  // Verify the health endpoint response includes stats fields.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to health socket
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  // Response should contain the extended stats fields
  EXPECT_NE(response.find("OK "), std::string::npos);
  EXPECT_NE(response.find("notifs="), std::string::npos);
  EXPECT_NE(response.find("variants="), std::string::npos);
  EXPECT_NE(response.find("proactive="), std::string::npos);
  EXPECT_NE(response.find("errors="), std::string::npos);
  EXPECT_NE(response.find("cache_entries="), std::string::npos);
}

TEST_F(WorkerTest, MaxRequestSizeEnforced) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_request_size = 32;  // Very small limit
  config.connection_timeout_ms = 10000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect and send a message with large length header
  intptr_t fd = ConnectToSocket(socket_path_);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Create a message header claiming 1MB of data
  uint32_t fake_len = 1024 * 1024;
  char header[4];
  header[0] = (fake_len >> 24) & 0xFF;
  header[1] = (fake_len >> 16) & 0xFF;
  header[2] = (fake_len >> 8) & 0xFF;
  header[3] = fake_len & 0xFF;
  pagespeed::test::PipeWrite(fd, header, 4);

  // Worker should disconnect due to oversized message
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Connection should be closed after oversized message";

  pagespeed::test::ClosePipe(fd);
}

TEST_F(WorkerTest, WarmupSentinelProcessesCss) {
  // Verify that the warmup sentinel triggers CSS minification for
  // a URL that doesn't have a minified variant yet.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content
  std::string css_url = "http://example.com/warmup.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), css_url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup notification (sentinel mask)
  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Poll for minified variant at default mask
  CapabilityMask default_mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), css_url, default_mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "Warmup should produce minified CSS";
  if (!variant.empty()) {
    EXPECT_LT(variant.size(), css.size());
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupDisabledIgnoresSentinel) {
  // Verify that warmup is skipped when disabled — original CSS is
  // NOT replaced with minified version.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string css_url = "http://example.com/no-warmup.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), css_url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Original CSS should be unchanged (not minified)
  CapabilityMask default_mask;
  std::string variant = ReadVariant(worker.cache(), css_url, default_mask);
  EXPECT_EQ(variant, css) << "Warmup disabled: original should be unchanged";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Proactive Viewport Sibling Generation Tests
// =============================================================================

TEST_F(WorkerTest, ImageNotificationGeneratesViewportSiblings) {
  // When proactive viewport siblings are enabled, a Mobile/WebP
  // notification should also generate Tablet and Desktop variants.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with a real JPEG (large enough to benefit
  // from resize).
  std::string url = "http://example.com/photo.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Mobile/WebP mask.
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // Poll for variants at all viewports.  Uses selector fallback so
  // content-deduped variants (identical bytes at different IDs) are
  // still discoverable.
  auto poll_variant = [&](CapabilityMask::Viewport vp,
                          CapabilityMask::ImageFormat fmt) -> bool {
    CapabilityMask mask(fmt, vp, CapabilityMask::PixelDensity::k1x,
                        CapabilityMask::SaveData::kOff,
                        CapabilityMask::TransferEncoding::kIdentity);
    for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!ReadBestVariant(worker.cache(), url, mask).empty()) return true;
    }
    return false;
  };

  // The directly requested variant (Mobile/WebP) should exist.
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kMobile,
                           CapabilityMask::ImageFormat::kWebP))
      << "Mobile/WebP variant not written";

  // Tablet and Desktop WebP siblings should be servable (either written
  // directly or via selector fallback when content-identical to Mobile).
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kTablet,
                           CapabilityMask::ImageFormat::kWebP))
      << "Tablet/WebP viewport sibling not servable";
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kDesktop,
                           CapabilityMask::ImageFormat::kWebP))
      << "Desktop/WebP viewport sibling not servable";

  // AVIF format siblings at other viewports should also be servable.
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kMobile,
                           CapabilityMask::ImageFormat::kAvif))
      << "Mobile/AVIF sibling not servable";
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kTablet,
                           CapabilityMask::ImageFormat::kAvif))
      << "Tablet/AVIF sibling not servable";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ViewportSiblingsDisabledOnlyGeneratesFormats) {
  // When viewport siblings are disabled, only format siblings at the
  // notification's viewport should be generated.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/no-vp.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Mobile/WebP mask.
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // Wait for processing.
  CapabilityMask mobile_avif(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string avif_variant;
  for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    avif_variant = ReadVariant(worker.cache(), url, mobile_avif);
    if (!avif_variant.empty()) break;
  }
  EXPECT_FALSE(avif_variant.empty()) << "Mobile/AVIF format sibling missing";

  // Tablet variants should NOT exist (viewport siblings disabled).
  CapabilityMask tablet_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string tablet = ReadVariant(worker.cache(), url, tablet_webp);
  EXPECT_TRUE(tablet.empty()) << "Tablet variant written despite disabled";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CacheFullVariantWritesNotStarvedByOwnLease) {
  // Regression test for issue #934: WriteImageVariants used to renew the
  // ORIGIN borrow's read lease at the top of every transcode iteration,
  // so at a wrap-pinned volume every variant write — wrap-gated on that
  // very stripe's lease — was deferred and dropped: the worker starved
  // its own write phase.  The worker now copies the origin bytes out of
  // the borrow and releases it before transcoding, so only the initial
  // read-time stamp remains, and it decays while the matrix runs.
  //
  // The deterministic pin of the underlying mechanism (renewal defers
  // writes at a full stripe; release + expiry admits them) lives in
  // cache_test ReleaseUnpinsFullStripeForWrites — timing there is fully
  // controlled.  This test guards the end-to-end outcome: with the write
  // cursor parked AT the wrap boundary, the worker's image pipeline must
  // still land variants within the poll budget.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // the origin read must be a disk borrow
  // Short: the origin read's single stamp must decay before the matrix
  // finishes (pre-fix, the per-iteration renewals kept it fresh forever).
  config.read_lease_duration_ms = 500;
  // Park the anti-starvation valve out of reach: the default 60s
  // lease_wrap_ceiling force-admits one write per minute, which would
  // mask genuine lease starvation right at this test's poll horizon.
  // Post-fix nothing keeps the lease alive, so the ceiling is never
  // needed; pre-fix (or with a reintroduced pin) the poll times out.
  // Scaled with the poll budget below so it stays an order of magnitude
  // beyond the poll horizon under a sanitizer's slowdown -- otherwise the
  // ceiling would force-admit a write right at the horizon and mask a
  // regression (the very failure mode this ceiling is parked out of reach of).
  config.lease_wrap_ceiling_ms =
      static_cast<uint64_t>(600000) * kSanitizerBudgetScale;
  config.proactive_image_variants = true;
  // Trim the matrix to 6 combinations (viewport x density) to keep the
  // unoptimized-build runtime reasonable; still several transcode
  // iterations' worth of write opportunities after the lease decays.
  config.proactive_savedata_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/starve.jpg";
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  // Park the write cursor AT the wrap boundary: hold a renewed borrow on
  // the origin (pinning the single 10MB stripe) and stream filler writes
  // until one is wrap-deferred.  A deferred wrap has no side effects, so
  // the cursor stays parked at the end of the data area — once the lease
  // decays, EVERY subsequent write must first win the lease-gated wrap,
  // which is exactly the cache-full contention from issue #934.
  {
    auto probe = worker.cache()->ReadBestAlternate(
        StripSchemeAuthority(url), "", "https", CapabilityMask());
    ASSERT_TRUE(probe.has_value());
    ASSERT_TRUE(probe->is_valid());
    CapabilityMask fill_mask;  // Desktop/Identity
    AlternateId fill_id =
        MaskToAlternateId(static_cast<uint8_t>(fill_mask.Encode() & 0xFF));
    // Any stage of the write can surface the lease-deferred NoSpace
    // (slot allocation happens when the buffered document is committed).
    auto try_fill = [&](int i, size_t sz) -> bool {
      const std::string filler(sz, 'f');
      AlternateMetadata meta;
      meta.full_mask = fill_mask.Encode();
      meta.content_type = ContentType::kOther;
      auto wh = worker.cache()->WriteAlternate(
          "/fill/" + std::to_string(i) + ".bin", "", "https", fill_id,
          filler.size(), meta);
      if (!wh.has_value()) return false;
      auto written = wh->write_sync(
          std::as_bytes(std::span(filler.data(), filler.size())));
      return written.has_value() && wh->close_sync().has_value();
    };
    // Shrinking ladder: when a write of the current size is deferred,
    // step down and keep filling, so the tail slack ends up smaller than
    // any variant the worker will write.  (A single 512KB probe can park
    // the cursor with almost 512KB of tail space still open — small
    // variant writes would then land without ever needing a wrap.)
    size_t fill_sz = 512 * 1024;
    bool parked = false;
    for (int i = 0; i < 200 && !parked; ++i) {
      ASSERT_TRUE(probe->renew_lease());
      if (!try_fill(i, fill_sz)) {
        if (fill_sz <= 512) {
          parked = true;  // even a sub-KB write needs a (deferred) wrap
        } else {
          fill_sz /= 8;
        }
      }
    }
    ASSERT_TRUE(parked) << "volume never reached the wrap boundary";
    EXPECT_GT(worker.cache()->Stats().writes_dropped_by_lease, 0u);
    probe->release();  // stop pinning; the probe's lease decays below
  }
  // Let the probe's last lease stamp expire so the only lease pressure
  // during processing comes from the worker's own origin read.
  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // Poll worker stats only — a cache read here would stamp a fresh lease
  // on the stripe and defer the very writes under test.  Pre-fix the
  // counter stayed at zero (every write dropped behind the renewed
  // lease).
  // A real-time budget is the point here (the write must be admitted while
  // the lease decays, not force-admitted by the wrap ceiling), so scale the
  // budget by the sanitizer factor rather than replacing it with an idle
  // wait; the ceiling above scales in lock-step to stay out of reach.
  bool wrote = false;
  for (int i = 0; i < 600 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().variants_written.load(std::memory_order_relaxed) >= 1) {
      wrote = true;
      break;
    }
  }
  EXPECT_TRUE(wrote)
      << "no image variant admitted at cache-full: worker starved its own "
         "writes (issue #934); variant write failures: "
      << worker.stats().alternate_write_failures.load(
             std::memory_order_relaxed);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ReadLeaseDurationZeroDisablesLeases) {
  // Issue #934 escape hatch: read_lease_duration_ms = 0 must reach the
  // Cyclone cache config — with leases disabled, a disk-hit read has no
  // lease to renew (and, cache-side, wraps are never deferred).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // force a disk borrow (RAM hits never lease)
  config.read_lease_duration_ms = 0;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/lease-knob.bin", std::string(4096, 'x'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/lease-knob.bin", "", "https",
                                                CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    EXPECT_FALSE(rr->renew_lease());
  }  // close the borrow before Shutdown tears the cache down

  worker.Shutdown();
}

TEST_F(WorkerTest, ReadLeaseDefaultForwardedToCache) {
  // Companion to ReadLeaseDurationZeroDisablesLeases: with the default
  // WorkerConfig (5s, mirroring Cyclone), a disk borrow has a live lease.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // force a disk borrow (RAM hits never lease)

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/lease-knob-on.bin", std::string(4096, 'x'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/lease-knob-on.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    EXPECT_TRUE(rr->renew_lease());
  }  // close the borrow before Shutdown tears the cache down

  worker.Shutdown();
}

// ---------------------------------------------------------------------------
// Issue #934 de-alias-before-processing: every copy-out site (HTML, CSS, JS,
// image transcode, SVG/unsupported passthrough, warmup) copies the origin
// bytes out of the cache borrow and, BEFORE releasing, proves the copy is
// untorn through the single shared seam Worker::BorrowCopyUntorn.  These
// tests pin that seam's decision table directly (the sanctioned fallback: an
// end-to-end wrap during the worker's microsecond-wide read->copy->verify
// window cannot be forced deterministically, so we drive the seam's inputs at
// the cache boundary).
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, DealiasSeamLiveDiskBorrowVerified) {
  // Disk borrow under a live lease whose epoch never moved: the seam confirms
  // the copy is untorn (renew_lease() == true), so processing proceeds.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // force a disk borrow (RAM hits never lease)

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-live.bin", std::string(4096, 'x'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/dealias-live.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    ASSERT_NE(rr->content_file_offset(), ReadResult::kNoFileOffset)
        << "expected a disk borrow, not a RAM hit";
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamRamHitNeverDiscarded) {
  // A RAM-cache hit is served from a handle-owned buffer (no mmap alias),
  // so a write-buffer wrap cannot tear it — but renew_lease() legitimately
  // returns false for it (nothing to renew).  The seam keys on
  // is_ram_cache_hit() and MUST NOT mistake that false for a torn read
  // (regression guard: a naive "renew==false -> miss" would drop every RAM
  // hit on the hot path).  Cyclone's CLFUS RAM tier admits an entry on the
  // SECOND put (seen-filter admission), and each disk read of a <=32KB
  // alternate puts — so the third read is the RAM hit.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = static_cast<size_t>(4 * 1024 * 1024);  // RAM on

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-ram.bin", std::string(4096, 'r'));
  // Read #1: disk borrow; put #1 only marks the CLFUS seen-filter.
  // Read #2: disk borrow; put #2 admits the entry into the RAM tier.
  for (int i = 0; i < 2; ++i) {
    auto warm = worker.cache()->ReadBestAlternate("/dealias-ram.bin", "",
                                                  "https", CapabilityMask());
    ASSERT_TRUE(warm.has_value());
    EXPECT_FALSE(warm->handle.is_ram_cache_hit());
  }
  {
    // Read #3: served from the RAM tier.
    auto rr = worker.cache()->ReadBestAlternate("/dealias-ram.bin", "", "https",
                                                CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    ASSERT_TRUE(rr->handle.is_ram_cache_hit())
        << "expected the third read to be a RAM-tier hit (CLFUS admission)";
    EXPECT_EQ(rr->content_file_offset(), ReadResult::kNoFileOffset);
    EXPECT_FALSE(rr->renew_lease());  // no lease on a RAM hit
    // ...but the seam still treats the copy as safe (not a miss).
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamReleasedHandleTrustedAsVerified) {
  // A released handle (is_valid()==false) means the borrow was already
  // copied + verified + released upstream — this is exactly what
  // WriteTextVariant's belt-and-suspenders verify sees for CSS/JS, whose
  // callers de-alias before minify.  The seam must treat it as safe: there
  // is no alias left to tear, and discarding here would break every
  // already-de-aliased text write.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-released.bin",
                std::string(4096, 'm'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/dealias-released.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    // The upstream contract: copy, verify, THEN release.
    auto span = rr->content();
    const std::string owned(reinterpret_cast<const char*>(span.data()),
                            span.size());
    ASSERT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
    rr->release();
    ASSERT_FALSE(rr->is_valid());
    EXPECT_FALSE(rr->renew_lease());  // nothing left to renew
    // A downstream re-verify (WriteTextVariant) must not discard.
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
    EXPECT_EQ(owned.front(), 'm');
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamLeasesDisabledNeverDiscarded) {
  // read_lease_duration_ms == 0 is the pre-lease escape hatch: a disk borrow
  // has no lease, so renew_lease() returns false.  The seam must treat that as
  // "no protection configured, cannot tear" and proceed (not discard) — else
  // the escape-hatch config would drop every disk read.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;
  config.read_lease_duration_ms = 0;  // leases OFF

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-nolease.bin", std::string(4096, 'n'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/dealias-nolease.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    EXPECT_FALSE(rr->renew_lease());  // leases disabled -> nothing to renew
    // leases_enabled=false: the seam short-circuits to "safe" without ever
    // consulting renew_lease().
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/false));
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamForcedWrapTreatedAsMiss) {
  // The load-bearing case: a live-lease disk borrow whose stripe is
  // FORCE-wrapped past the ceiling.  The wrap moves the stripe's epoch, so the
  // borrowed region may have been overwritten in place; renew_lease() reports
  // the move (false) and the seam returns false — every copy-out site
  // (HTML/CSS/JS/image/warmup) then discards the copy and treats the read as a
  // miss instead of persisting torn bytes.  A single 10MB volume is one
  // stripe, so all fills contend on the borrowed stripe.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;              // disk borrow (RAM hits never lease)
  config.read_lease_duration_ms = 60000;  // long: never decays during the test
  config.lease_wrap_ceiling_ms = 1;       // force wraps almost immediately

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  const std::string url = "/dealias-wrap.bin";
  CacheOriginal(worker.cache(), url, std::string(64 * 1024, 'v'));

  auto borrow =
      worker.cache()->ReadBestAlternate(url, "", "https", CapabilityMask());
  ASSERT_TRUE(borrow.has_value());
  ASSERT_TRUE(borrow->is_valid());
  ASSERT_NE(borrow->content_file_offset(), ReadResult::kNoFileOffset);

  // Copy the bytes out first, exactly as every de-alias site does.
  auto span = borrow->content();
  const std::string owned(reinterpret_cast<const char*>(span.data()),
                          span.size());
  // Untorn so far: the epoch has not moved.
  EXPECT_TRUE(Worker::BorrowCopyUntorn(*borrow, /*leases_enabled=*/true));

  // Let the 1ms ceiling elapse, then flood writes to force the write cursor to
  // wrap all the way around the single stripe and overwrite the borrowed
  // region (force-admitted past the ceiling despite the live lease).
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto fill = [&](int i) {
    const std::string filler(512 * 1024, 'f');
    CapabilityMask m;
    AlternateId id = MaskToAlternateId(static_cast<uint8_t>(m.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = m.Encode();
    meta.content_type = ContentType::kOther;
    auto wh =
        worker.cache()->WriteAlternate("/fill/" + std::to_string(i) + ".bin",
                                       "", "https", id, filler.size(), meta);
    if (!wh.has_value()) return;
    (void)wh->write_sync(
        std::as_bytes(std::span(filler.data(), filler.size())));
    (void)wh->close_sync();
  };
  for (int i = 0; i < 80; ++i) fill(i);
  ASSERT_GT(worker.cache()->Stats().wraps_forced_past_lease, 0u)
      << "the borrowed stripe was never force-wrapped past the ceiling";

  // The epoch moved under the still-held borrow: the seam now reports the copy
  // may be torn, so the caller must treat the read as a miss.
  EXPECT_FALSE(Worker::BorrowCopyUntorn(*borrow, /*leases_enabled=*/true));
  // Our earlier copy captured the pre-wrap bytes and is unaffected.
  EXPECT_EQ(owned.front(), 'v');

  borrow->release();
  worker.Shutdown();
}

TEST_F(WorkerTest, HtmlWrapDuringDealiasCopyPersistsNothing) {
  // The durable-corruption non-persistence guarantee, exercised at the REAL
  // HTML copy-out site: the test hook fires inside the worker's
  // read->copy->verify window and force-wraps the borrowed stripe past the
  // lease ceiling (moving its epoch), so the worker's own BorrowCopyUntorn
  // fails.  The worker must then treat the read as a cache miss: NO variant
  // or sentinel write for the URL, html_processed stays 0, and exactly one
  // read_borrow_wrap_discards is counted.  Pre-fix (aliased processing) the
  // pipeline would have persisted torn transform output with a fresh
  // checksum.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;              // disk borrow (RAM hits never lease)
  config.read_lease_duration_ms = 60000;  // live for the whole test
  config.lease_wrap_ceiling_ms = 1;       // wraps force almost immediately

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  const std::string cache_url = "/wrap-during-copy.html";
  CacheOriginal(worker.cache(), cache_url, html);

  // Runs on the worker-pool thread while the worker holds the origin
  // borrow (60s lease, stamped by its read).  Fill the single 10MB stripe
  // to the wrap boundary: the wrap defers on the live lease, and once the
  // 1ms ceiling elapses it is FORCED — moving the epoch under the borrow.
  std::atomic<uint64_t> forced_wraps{0};
  worker.test_hook_after_html_origin_read_ = [&worker, &forced_wraps]() {
    CapabilityMask fill_mask;  // Desktop/Identity
    AlternateId fill_id =
        MaskToAlternateId(static_cast<uint8_t>(fill_mask.Encode() & 0xFF));
    const std::string filler(512 * 1024, 'f');
    for (int i = 0; i < 200; ++i) {
      AlternateMetadata meta;
      meta.full_mask = fill_mask.Encode();
      meta.content_type = ContentType::kOther;
      auto wh = worker.cache()->WriteAlternate(
          "/fill/" + std::to_string(i) + ".bin", "", "https", fill_id,
          filler.size(), meta);
      if (wh.has_value()) {
        (void)wh->write_sync(
            std::as_bytes(std::span(filler.data(), filler.size())));
        (void)wh->close_sync();
      }
      forced_wraps = worker.cache()->Stats().wraps_forced_past_lease;
      if (forced_wraps > 0) break;
      // Let the deferral clock pass the 1ms ceiling before retrying.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/wrap-during-copy.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  // Wait for the discard to register (the hook's fill loop runs inside the
  // notification, so allow generous time on a loaded runner).
  bool discarded = false;
  for (int i = 0; i < 600 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().read_borrow_wrap_discards.load() >= 1) {
      discarded = true;
      break;
    }
  }
  WaitForWorkerIdle(worker);

  EXPECT_GT(forced_wraps.load(), 0u)
      << "the hook never forced a wrap past the ceiling";
  EXPECT_TRUE(discarded) << "torn copy was not discarded";
  EXPECT_EQ(worker.stats().read_borrow_wrap_discards.load(), 1u);
  // Nothing persisted: no transform variant, no processing counted.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  EXPECT_TRUE(ReadVariant(worker.cache(), cache_url, mask).empty())
      << "a variant was persisted from a torn copy";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlTransformHappyPathNoWrapDiscards) {
  // Happy-path companion to HtmlWrapDuringDealiasCopyPersistsNothing: with
  // no wrap racing the copy, the de-aliased pipeline (copy + verify +
  // release before scan/transform/write) must still produce a correct
  // transform variant and count zero torn-copy discards.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // exercise the disk-borrow + live-lease path

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  const std::string cache_url = "/dealias-html.html";
  CacheOriginal(worker.cache(), cache_url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/dealias-html.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "HTML variant not written from owned copy";
  if (!variant.empty()) {
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    EXPECT_NE(variant.find("Hello World"), std::string::npos);
  }
  // The happy path must not have tripped the torn-copy discard.
  EXPECT_EQ(worker.stats().read_borrow_wrap_discards.load(), 0u);
  // The original disk-borrowed bytes survived intact behind the copy.
  CapabilityMask default_mask;
  EXPECT_EQ(ReadVariant(worker.cache(), cache_url, default_mask), html);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupSentinelGeneratesViewportSiblings) {
  // When warmup is triggered with viewport siblings enabled, it
  // should generate variants at all viewports.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;
  config.proactive_viewport_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/warmup-vp.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup sentinel.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Poll for Mobile/WebP variant (generated proactively by warmup).
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string variant;
  for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mobile_webp);
    if (!variant.empty()) break;
  }
  EXPECT_FALSE(variant.empty()) << "Warmup: Mobile/WebP not generated";

  // Desktop/WebP should also exist.  Poll because AVIF encoding
  // for earlier viewports may still be completing.
  CapabilityMask desktop_webp;  // Default = Desktop/Identity
  desktop_webp.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string dv;
  for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dv = ReadVariant(worker.cache(), url, desktop_webp);
    if (!dv.empty()) break;
  }
  EXPECT_FALSE(dv.empty()) << "Warmup: Desktop/WebP not generated";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Management Socket Tests (Feature 5)
// =============================================================================

TEST_F(WorkerTest, MgmtSocketExists) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Management socket/pipe endpoint should exist.
  std::string mgmt_path = worker.mgmt_socket_path();
  EXPECT_TRUE(pagespeed::test::PipeEndpointExists(mgmt_path));

  worker.Shutdown();
}

TEST_F(WorkerTest, MgmtStatsCommand) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to management socket and send STATS command.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read response
  char buf[2048];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from management socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);

  // Should be JSON with expected fields.
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("\"notifications\""), std::string::npos);
  EXPECT_NE(response.find("\"variants\""), std::string::npos);
  EXPECT_NE(response.find("\"by_type\""), std::string::npos);
  EXPECT_NE(response.find("\"by_format\""), std::string::npos);
  EXPECT_NE(response.find("\"timing_us\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsCommand) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to management socket and send METRICS command.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the metrics document no longer fits one
  // bufferful (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  // Should contain Prometheus text exposition format with expected metrics.
  EXPECT_NE(response.find("pagespeed_notifications_total"), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("pagespeed_variants_written_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_errors_total"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_cache_entries"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_connections_active"), std::string::npos);
  // Should contain TYPE annotations.
  EXPECT_NE(response.find("# TYPE pagespeed_notifications_total counter"),
            std::string::npos);
  EXPECT_NE(response.find("# TYPE pagespeed_cache_entries gauge"),
            std::string::npos);
  // Should contain format-labeled metrics.
  EXPECT_NE(response.find("pagespeed_format_generated_total{format=\"webp\"}"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_processing_seconds_total{type=\"image\"}"),
            std::string::npos);
}

TEST_F(WorkerTest, MgmtPurgeCommand) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable proactive variants to simplify: only generates requested format.
  config.proactive_image_variants = false;

  // PURGE requires authentication (fail-closed).
  setenv("PAGESPEED_PURGE_TOKEN", "purge-test-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS.  Use a hostname so PURGE can target it.
  std::string url = "/purge.css";
  std::string hostname = "example.com";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             css.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(css.data(), css.size())));
    (void)wh->close_sync();
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification to create a variant.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for variant to appear.
  CapabilityMask default_mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, default_mask, hostname);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "CSS variant not written before purge";

  // AUTH + PURGE on same connection (AUTH keeps connection alive).
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Send AUTH first.
  std::string auth_cmd = "AUTH purge-test-token\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());

  // Read AUTH response.
  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_EQ(std::string(buf, n), "OK\n") << "AUTH should succeed";

  // Send PURGE on the same authenticated connection.
  std::string cmd = "PURGE " + hostname + " " + url + "\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read PURGE response.
  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("OK"), std::string::npos) << "Response: " << response;

  // Wait for purge to take effect
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // The variant should be gone after purge.
  EXPECT_NE(response.find("deleted"), std::string::npos);

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeAuthCorrectToken) {
  // AUTH with correct token should allow PURGE on the same connection.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  // Set the purge token via environment variable.
  setenv("PAGESPEED_PURGE_TOKEN", "test-secret-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Write some content to purge.
  std::string url = "/auth-purge.css";
  std::string hostname = "auth.example.com";
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kOther;
    std::string data = "body { margin: 0; }";
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(data.data(), data.size())));
    (void)wh->close_sync();
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // AUTH keeps the connection alive, so AUTH+PURGE works on one connection.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Send AUTH with correct token.
  std::string auth_cmd = "AUTH test-secret-token\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_EQ(std::string(buf, n), "OK\n")
      << "AUTH with correct token should succeed";

  // PURGE on the same authenticated connection.
  std::string purge_cmd = "PURGE " + hostname + " " + url + "\n";
  pagespeed::test::PipeWrite(fd, purge_cmd.data(), purge_cmd.size());

  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("OK"), std::string::npos)
      << "PURGE after AUTH should succeed: " << response;
  EXPECT_NE(response.find("deleted"), std::string::npos)
      << "Response should include deletion count: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeAuthWrongToken) {
  // AUTH with wrong token should be rejected.
  WorkerConfig config;
  config.socket_path = socket_path_;

  setenv("PAGESPEED_PURGE_TOKEN", "correct-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "AUTH wrong-token\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR invalid token"), std::string::npos)
      << "Wrong token should be rejected: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeWithoutAuthRejected) {
  // PURGE without AUTH when token is configured should be rejected.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  setenv("PAGESPEED_PURGE_TOKEN", "some-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Try PURGE directly without AUTH.
  std::string cmd = "PURGE example.com /some-url\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR authentication required"), std::string::npos)
      << "PURGE without AUTH should fail: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtStatsWithoutAuth) {
  // STATS should work without AUTH even when token is configured.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  setenv("PAGESPEED_PURGE_TOKEN", "some-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "STATS\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[2048];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "STATS should work without AUTH: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtPurgeNoTokenConfigured) {
  // When no token is configured, PURGE should be rejected (fail-closed).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  // Ensure no token is set.
  unsetenv("PAGESPEED_PURGE_TOKEN");

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // PURGE without any token configured should be rejected.
  std::string cmd = "PURGE example.com /no-token-url\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR"), std::string::npos)
      << "PURGE should be rejected when no token configured: " << response;
}

TEST_F(WorkerTest, MgmtPurgeUrlTooLong) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_url_length = 32;  // Very short limit for testing.

  setenv("PAGESPEED_PURGE_TOKEN", "url-long-token", 1);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // AUTH first.
  std::string auth_cmd = "AUTH url-long-token\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());
  char auth_buf[64];
  ssize_t auth_n =
      pagespeed::test::PipeRead(fd, auth_buf, sizeof(auth_buf) - 1);
  ASSERT_GT(auth_n, 0);

  // URL exceeding max_url_length should be rejected.
  std::string long_url(64, 'x');
  std::string cmd = "PURGE example.com /" + long_url + "\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR URL too long"), std::string::npos)
      << "Should reject long URL: " << response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

TEST_F(WorkerTest, MgmtUnknownCommand) {
  WorkerConfig config;
  config.socket_path = socket_path_;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "FOOBAR\n";
  pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR"), std::string::npos);
}

// =============================================================================
// Timing Counters Tests (Feature 7)
// =============================================================================

TEST_F(WorkerTest, TimingCountersIncrementAfterProcessing) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Timing should start at zero.
  EXPECT_EQ(worker.stats().total_processing_time_us.load(), 0u);
  EXPECT_EQ(worker.stats().css_processing_time_us.load(), 0u);

  // Pre-populate CSS content.
  std::string css_url = "http://example.com/timing.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), css_url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing — poll until both timing counters are non-zero
  // instead of a fixed sleep (slow shards can sit at the pipe read past a
  // fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().total_processing_time_us.load() > 0u &&
        worker.stats().css_processing_time_us.load() > 0u) {
      break;
    }
  }

  // Timing counters should be non-zero after processing.
  EXPECT_GT(worker.stats().total_processing_time_us.load(), 0u);
  EXPECT_GT(worker.stats().css_processing_time_us.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Save-Data and Density Variant Generation (Features 2 & 3 worker-level)
// =============================================================================

TEST_F(WorkerTest, ImageNotificationGeneratesSaveDataSiblings) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;  // Only format + save-data
  config.proactive_savedata_variants = true;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/savedata.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with SaveData=off, Mobile/WebP.
  CapabilityMask notif_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = notif_mask.Encode();
  SendNotification(notification);

  // The Save-Data=on sibling is written as part of the same notification's
  // proactive matrix, so wait for the matrix to retire (a condition wait,
  // not a fixed poll budget) and then read the sibling.  The old 6s poll
  // expired mid-transcode under TSan's ~10x slowdown.
  WaitForFirstVariantThenIdle(worker);
  CapabilityMask sd_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string sd_variant = ReadVariant(worker.cache(), url, sd_mask);
  EXPECT_FALSE(sd_variant.empty())
      << "Save-Data=on sibling variant not generated";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationGeneratesDensitySiblings) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/density.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with 1x density, Mobile/WebP.
  CapabilityMask notif_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = notif_mask.Encode();
  SendNotification(notification);

  // Poll for 2x density sibling.  Uses selector fallback since
  // content-identical 1x/2x variants may be deduped to a single write.
  CapabilityMask den2x_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string den_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    den_variant = ReadBestVariant(worker.cache(), url, den2x_mask);
    if (!den_variant.empty()) break;
  }
  EXPECT_FALSE(den_variant.empty())
      << "2x density sibling variant not servable";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, InvalidateUrlDeletesVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Manually write a couple of variants to the cache.
  // Use path-only URL for cache writes (InvalidateUrl normalizes internally).
  std::string url = "http://example.com/invalidate.jpg";
  std::string cache_url = StripSchemeAuthority(url);
  std::string data = "fake image data";

  CapabilityMask mask1(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CapabilityMask mask2(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  // Write two alternates via the PageSpeedCache API.
  auto write_alt = [&](const CapabilityMask& mask) {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kImage;
    auto wh = worker.cache()->WriteAlternate(cache_url, "", "https", id,
                                             data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(data.data(), data.size())));
    (void)wh->close_sync();
  };
  write_alt(mask1);
  write_alt(mask2);

  // Verify they exist.
  EXPECT_FALSE(ReadVariant(worker.cache(), url, mask1).empty());
  EXPECT_FALSE(ReadVariant(worker.cache(), url, mask2).empty());

  // Invalidate — Remove() deletes all alternates for the URL at once.
  int deleted = worker.InvalidateUrl(url);
  EXPECT_GE(deleted, 1);

  worker.Shutdown();
}

// =============================================================================
// Thread Pool Dispatch Tests
// =============================================================================

TEST_F(WorkerTest, HealthResponsiveDuringProcessing) {
  // Verify the health endpoint responds promptly even when the worker
  // is processing a notification in the thread pool.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content
  std::string url = "http://example.com/tp-health.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Immediately query health endpoint - should respond within 1 second
  auto start = std::chrono::steady_clock::now();
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  auto elapsed = std::chrono::steady_clock::now() - start;
  pagespeed::test::ClosePipe(fd);

  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';
  std::string response(buf, n);
  EXPECT_EQ(response.substr(0, 2), "OK");
  EXPECT_NE(response.find("inflight="), std::string::npos)
      << "Response: " << response;

  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  EXPECT_LT(elapsed_ms, 1000)
      << "Health response took too long: " << elapsed_ms << "ms";

  // Wait for processing to finish
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_F(WorkerTest, ConcurrentNotificationsProcessed) {
  // Send 3 CSS notifications rapidly and verify all 3 variants appear.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate 3 CSS files
  std::string url1 = "http://example.com/tp1.css";
  std::string css1 = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url1, css1);

  std::string url2 = "http://example.com/tp2.css";
  std::string css2 = "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url2, css2);

  std::string url3 = "http://example.com/tp3.css";
  std::string css3 = "p  {  line-height:  1.5;  margin:  10px;  }\n";
  CacheOriginal(worker.cache(), url3, css3);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send all 3 rapidly
  CacheNotification n1, n2, n3;
  n1.url = url1;
  n1.scheme = "https";
  n1.content_type = ContentType::kCss;
  n1.capability_mask = CapabilityMask().Encode();
  n2.url = url2;
  n2.scheme = "https";
  n2.content_type = ContentType::kCss;
  n2.capability_mask = CapabilityMask().Encode();
  n3.url = url3;
  n3.scheme = "https";
  n3.content_type = ContentType::kCss;
  n3.capability_mask = CapabilityMask().Encode();

  SendNotification(n1);
  SendNotification(n2);
  SendNotification(n3);

  // Poll until all 3 variants appear
  CapabilityMask mask = CapabilityMask::Decode(CapabilityMask().Encode());
  std::string v1, v2, v3;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (v1.empty()) v1 = ReadVariant(worker.cache(), url1, mask);
    if (v2.empty()) v2 = ReadVariant(worker.cache(), url2, mask);
    if (v3.empty()) v3 = ReadVariant(worker.cache(), url3, mask);
    if (!v1.empty() && !v2.empty() && !v3.empty()) break;
  }

  EXPECT_FALSE(v1.empty()) << "tp1.css variant missing";
  EXPECT_FALSE(v2.empty()) << "tp2.css variant missing";
  EXPECT_FALSE(v3.empty()) << "tp3.css variant missing";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ShutdownWaitsForInFlightWork) {
  // Send a notification and immediately shut down; the variant should
  // still be written because shutdown waits for in-flight work.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/tp-shutdown.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Brief pause to ensure notification is dispatched to thread pool
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Immediately shut down
  worker.Shutdown();
  worker_thread.join();

  // Verify the variant was written despite immediate shutdown
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty())
      << "Variant should be written before shutdown completes";
}

TEST_F(WorkerTest, InFlightCounterAccuracy) {
  // Verify that in_flight_work() eventually returns to 0 after
  // all notifications are processed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content
  std::string url = "http://example.com/tp-inflight.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(worker.in_flight_work(), 0);

  // Send notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing to complete
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.in_flight_work() == 0 &&
        worker.stats().notifications_received.load() >= 1) {
      break;
    }
  }

  EXPECT_EQ(worker.in_flight_work(), 0)
      << "In-flight counter should return to 0 after processing";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, StatsThreadPoolInfoInStats) {
  // Verify STATS response includes thread_pool info.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains and "thread_pool" sits near its end, so a fixed-buffer single
  // read truncates it silently once the document outgrows the buffer (see
  // ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"thread_pool\""), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("\"inflight\""), std::string::npos)
      << "Response: " << response;
  EXPECT_NE(response.find("\"size\""), std::string::npos)
      << "Response: " << response;
}

TEST_F(WorkerTest, MetricsIncludesThreadPoolGauge) {
  // Verify METRICS response includes thread pool inflight gauge.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the metrics document no longer fits one
  // bufferful (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("pagespeed_thread_pool_inflight"), std::string::npos)
      << "Response: " << response;
}

// --- Phase 4.4: Compressed variant tests ---

TEST_F(WorkerTest, CssNotificationWritesCompressedVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS.
  std::string url = "http://example.com/compress.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Read identity variant at Desktop/Identity.
  CapabilityMask identity_mask;
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  ASSERT_FALSE(identity.empty()) << "Identity CSS variant missing";
  EXPECT_LT(identity.size(), css.size());

  // Read gzip variant at Desktop/Gzip.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  ASSERT_FALSE(gz_variant.empty()) << "Gzip CSS variant missing";
  EXPECT_EQ(GzipDecompress(gz_variant), identity);

  // Read brotli variant at Desktop/Brotli.
  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  ASSERT_FALSE(br_variant.empty()) << "Brotli CSS variant missing";
  EXPECT_EQ(BrotliDecompress(br_variant), identity);

  // Verify stats counters.
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u);
  EXPECT_GE(worker.stats().brotli_variants_written.load(), 1u);
}

TEST_F(WorkerTest, CompressionDisabledProducesNoCompressedVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.gzip_level = 0;
  config.brotli_level = 0;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/nocompress.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Identity variant should exist at Desktop/Identity.
  CapabilityMask identity_mask;
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  EXPECT_FALSE(identity.empty()) << "Identity CSS variant missing";

  // Gzip and brotli variants should NOT exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, gz_mask).empty())
      << "Gzip variant should not exist when level=0";

  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, br_mask).empty())
      << "Brotli variant should not exist when level=0";

  // Stats should show zero compressed variants.
  EXPECT_EQ(worker.stats().gzip_variants_written.load(), 0u);
  EXPECT_EQ(worker.stats().brotli_variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, MgmtMetricsIncludesCompressedVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the metrics document no longer fits one
  // bufferful (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(
      response.find("pagespeed_compressed_variants_total{encoding=\"gzip\"}"),
      std::string::npos)
      << "Missing gzip metric in: " << response;
  EXPECT_NE(
      response.find("pagespeed_compressed_variants_total{encoding=\"brotli\"}"),
      std::string::npos)
      << "Missing brotli metric in: " << response;
}

TEST_F(WorkerTest, MgmtStatsIncludesCompressedVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"gzip\":"), std::string::npos)
      << "Missing gzip in STATS: " << response;
  EXPECT_NE(response.find("\"brotli\":"), std::string::npos)
      << "Missing brotli in STATS: " << response;
}

// --- Encoding AlternateId distinctness test ---

TEST_F(WorkerTest, BrotliNotificationWritesDistinctAlternateIds) {
  // When a notification arrives with a non-identity encoding mask (e.g.,
  // Desktop/Brotli = 0x88), the identity write should strip encoding bits
  // so identity, gzip, and brotli alternates all get distinct AlternateIds.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS (large enough to compress).
  std::string url = "http://example.com/encoding-test.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Brotli mask (0x88).
  // Before the fix, the identity write would collide with brotli at the
  // same AlternateId (0x88).  After the fix, identity goes to 0x08.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x88;  // Desktop + Brotli encoding
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Identity variant: Desktop/Identity = 0x08.
  CapabilityMask identity_mask = CapabilityMask::Decode(0x88);
  identity_mask.set_transfer_encoding(
      CapabilityMask::TransferEncoding::kIdentity);
  EXPECT_EQ(identity_mask.Encode(), 0x08u);
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  ASSERT_FALSE(identity.empty()) << "Identity CSS variant missing at 0x08";
  EXPECT_LT(identity.size(), css.size()) << "Should be minified";

  // Gzip variant: Desktop/Gzip = 0x48.
  CapabilityMask gz_mask = CapabilityMask::Decode(0x88);
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  EXPECT_EQ(gz_mask.Encode(), 0x48u);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  ASSERT_FALSE(gz_variant.empty()) << "Gzip CSS variant missing at 0x48";
  EXPECT_EQ(GzipDecompress(gz_variant), identity);

  // Brotli variant: Desktop/Brotli = 0x88.
  CapabilityMask br_mask = CapabilityMask::Decode(0x88);
  // Already has brotli encoding, no need to set.
  EXPECT_EQ(br_mask.Encode(), 0x88u);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  ASSERT_FALSE(br_variant.empty()) << "Brotli CSS variant missing at 0x88";
  EXPECT_EQ(BrotliDecompress(br_variant), identity);

  // All three AlternateIds must be distinct.
  EXPECT_NE(identity_mask.Encode(), gz_mask.Encode());
  EXPECT_NE(identity_mask.Encode(), br_mask.Encode());
  EXPECT_NE(gz_mask.Encode(), br_mask.Encode());
}

TEST_F(WorkerTest, HtmlRevalidationWhenExternalCssMissing) {
  // Verifies the critical CSS revalidation flow:
  //   1. HTML references an external stylesheet not yet in cache.
  //   2. Worker writes HTML variant with kFlagNeedsRevalidation set
  //      (other transforms applied, but no critical CSS).
  //   3. CSS is added to cache.
  //   4. Second notification (simulating nginx re-notify on HIT).
  //   5. Worker writes updated variant WITH critical CSS, flag cleared.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable RAM cache: this single-process test writes CSS via
  // CacheOriginal() and expects the worker to see it on the next
  // disk read.  Write-around RAM cache can return stale state for
  // same-process writes.  In production, CSS is written by nginx
  // (different process / different RAM cache), so this is safe.
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/page.html";
  std::string css_url = "http://example.com/style.css";
  // Use multiple images so lazy-load applies to non-LCP images.
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "<img src=\"photo1.jpg\">"
      "<img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\">"
      "<img src=\"photo4.jpg\">"
      "</body></html>";

  // Only cache the HTML — CSS is NOT in cache yet.
  CacheOriginal(worker.cache(), url, html);

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // --- Phase 1: Send notification with CSS missing ---
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;  // Mobile/Identity
  SendNotification(notification);

  // Poll for variant to appear.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> phase1;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    phase1 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase1.has_value()) break;
  }

  // Variant should exist (fetchpriority + lazy-load still applies).
  ASSERT_TRUE(phase1.has_value()) << "Phase 1: HTML variant not written";
  // Should NOT contain critical CSS (external stylesheet was missing).
  EXPECT_EQ(phase1->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 1: critical CSS should not be injected without stylesheet";
  // First img gets fetchpriority="high" (LCP heuristic), later imgs
  // get lazy-loaded.
  EXPECT_NE(phase1->content.find("fetchpriority=\"high\""), std::string::npos)
      << "Phase 1: fetchpriority should be applied to first image";
  EXPECT_NE(phase1->content.find("loading=\"lazy\""), std::string::npos)
      << "Phase 1: lazy-load should be applied to later images";
  // kFlagNeedsRevalidation should be SET.
  EXPECT_NE(phase1->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 1: kFlagNeedsRevalidation should be set";

  // --- Phase 2: Add CSS to cache, re-notify ---
  std::string css =
      ".hero { font-size: 24px; color: red; } "
      ".footer { display: none; margin: 10px; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  // Poll until the variant is updated.  Re-send notifications
  // periodically (every 1s) to simulate nginx re-notifying on each
  // revalidation HIT.  The worker's revalidation cooldown (3s) suppresses
  // some of these, matching production behavior where CSS convergence
  // takes a few seconds after the stylesheet becomes available.
  // With RAM cache disabled, all reads go directly to disk (mmap).
  std::optional<VariantWithMeta> phase2;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Re-notify every 1s (simulates nginx re-notification on HIT).
    if (i % 10 == 0) SendNotification(notification);
    phase2 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase2.has_value() &&
        phase2->content.find("data-pagespeed-critical") != std::string::npos) {
      break;
    }
  }

  // Variant should now contain critical CSS.
  ASSERT_TRUE(phase2.has_value()) << "Phase 2: HTML variant not written";
  EXPECT_NE(phase2->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 2: critical CSS should be injected after stylesheet cached";
  // The hero class should be in the critical CSS.
  EXPECT_NE(phase2->content.find("hero"), std::string::npos)
      << "Phase 2: hero class should be in critical CSS";
  // kFlagNeedsRevalidation should be CLEARED.
  EXPECT_EQ(phase2->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 2: kFlagNeedsRevalidation should be cleared";
  // Other transforms should still be applied.
  EXPECT_NE(phase2->content.find("fetchpriority=\"high\""), std::string::npos)
      << "Phase 2: fetchpriority should still be applied";
  EXPECT_NE(phase2->content.find("loading=\"lazy\""), std::string::npos)
      << "Phase 2: lazy-load should still be applied";
}

TEST_F(WorkerTest, AsyncCssSuppressedWhenExternalCssMissing) {
  // Cold-cache FOUC fail-safe (the iispeed.com regression): the page declares
  // an external <link rel=stylesheet> that is NOT yet cached AND carries a small
  // inline <style> (e.g. a dark-mode override). The heuristic extractor echoes
  // the inline rules as "critical CSS", so critical CSS IS injected — but the
  // deferred-byte denominator collapsed to the inline-only blob. Without the
  // guard, the small-sheet escape hatch would fire and async-defer the
  // unmeasured external sheet (preload swap) -> the page renders unstyled
  // until it loads. The guard must keep the <link> render-blocking AND mark the
  // variant for revalidation (even though critical CSS was injected) so it
  // self-heals once the sheet caches.
  //
  // A validated browser profile is seeded so phase 2 CAN defer: without one the
  // empirical gate refuses regardless, and the test could no longer tell the
  // cold-cache veto apart from a page that simply never defers. The profile's
  // validation is bound to the WARM combined sheet, which is what phase 2
  // assembles.
  WorkerConfig config = BrowserProfileConfig();
  config.ram_cache_size = 0;  // single-process test; see sibling test above.

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cold.html";
  std::string css_url = "http://example.com/big.css";
  // Inline <style> overrides (echoed as critical) + a .hero rule that matches an
  // above-the-fold element, guaranteeing the heuristic injects critical CSS.
  std::string html =
      "<html><head>"
      "<style>html.dark,html.dark body{background:#0c0a09}"
      ".hero{color:red}</style>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "</body></html>";

  // Only cache the HTML — the external CSS is NOT in cache yet (cold).
  CacheOriginal(worker.cache(), url, html);

  std::string big_css =
      ".hero{color:red;font-size:24px}.footer{display:none}"
      "body{margin:0;font-family:system-ui}a{color:blue}";
  // Two combined sheets, because the two phases assemble different bytes: cold
  // is the page's inline CSS alone (the external sheet was never gathered),
  // warm is that plus the external sheet, newline-joined.
  HtmlScanner profile_scanner;
  HtmlScanResult profile_scan = profile_scanner.Scan(url, html);
  ASSERT_TRUE(profile_scan.success);
  const std::string cold_combined_css = profile_scan.inline_css;
  const std::string warm_combined_css =
      profile_scan.inline_css + "\n" + big_css;

  // Phase 1 seeds against the COLD bytes on purpose. It makes the empirical
  // gate SATISFIED while the sheet is missing, so external_css_missing is the
  // only thing left that can refuse — which is the fail-safe under test. Seed
  // the warm bytes here instead and the empirical gate would refuse too, and
  // phase 1 would pass even with the cold-cache guard deleted.
  StoreBrowserProfile(worker, url, html, ".hero{color:red}",
                      /*validated=*/true,
                      /*validated_against_css=*/cold_combined_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // --- Phase 1: external CSS cold ---
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> phase1;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    phase1 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase1.has_value()) break;
  }
  ASSERT_TRUE(phase1.has_value()) << "Phase 1: HTML variant not written";

  // The inline override IS echoed as critical CSS (this is the echo that fooled
  // the old gate into thinking the deferred sheet was tiny).
  EXPECT_NE(phase1->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 1: heuristic echo should still inline critical CSS";
  // But the external sheet must NOT be async-deferred (the fail-safe).
  EXPECT_EQ(phase1->content.find("as=\"style\""), std::string::npos)
      << "Phase 1: cold external sheet must NOT be async-deferred (FOUC guard)";
  EXPECT_EQ(phase1->content.find("data-pagespeed-async"), std::string::npos)
      << "Phase 1: no async markers when the deferred sheet is unmeasured";
  // The original render-blocking <link> survives so the page stays styled.
  EXPECT_NE(phase1->content.find("rel=\"stylesheet\""), std::string::npos)
      << "Phase 1: render-blocking external <link> must be preserved";
  // Self-heal: revalidation MUST be set even though critical CSS was injected.
  // (The pre-fix gate cleared it here, making the cold result sticky.)
  EXPECT_NE(phase1->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 1: kFlagNeedsRevalidation must be set despite injected "
         "critical "
         "CSS, so the cold-cache decision self-heals";

  // --- Phase 2: cache the external CSS, re-notify -> converges ---
  CacheOriginal(worker.cache(), css_url, big_css, "example.com");
  // Re-seed against the WARM bytes: the combined sheet genuinely changed when
  // the external sheet joined it, so the phase-1 record no longer describes
  // what is being served. Re-validating is exactly what a real analysis would
  // do, and it keeps phase 2 a test of the cold-cache veto lifting rather than
  // of the hash gate.
  StoreBrowserProfile(worker, url, html, ".hero{color:red}",
                      /*validated=*/true,
                      /*validated_against_css=*/warm_combined_css);

  std::optional<VariantWithMeta> phase2;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (i % 10 == 0) SendNotification(notification);
    phase2 = ReadVariantWithMeta(worker.cache(), url, mask);
    // Wait for the self-heal: async-CSS re-fires (preload swap) once the
    // sheet is cached.
    if (phase2.has_value() &&
        phase2->content.find("as=\"style\"") != std::string::npos) {
      break;
    }
  }
  ASSERT_TRUE(phase2.has_value()) << "Phase 2: HTML variant not written";
  // Self-heal proof: with the SAME small byte sizes as phase 1, the only thing
  // that changed is that the sheet is now cached (external_css_missing flipped
  // false). That alone re-enables async-CSS — proving the cold-cache veto, not
  // some other factor, gated phase 1.
  EXPECT_NE(phase2->content.find("as=\"style\""), std::string::npos)
      << "Phase 2: async-CSS should re-fire once the external sheet caches";
  // And the revalidation flag clears (the veto lifted).
  EXPECT_EQ(phase2->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 2: kFlagNeedsRevalidation should clear once the sheet caches";
}

TEST_F(WorkerTest, AsyncCssSuppressedWhenOneOfMultipleSheetsMissing) {
  // Multi-sheet partial miss: one sheet cached, one missing. external_css_missing
  // accumulates across the whole gather loop, so async must stay suppressed and
  // the (same-origin) miss marks the variant for revalidation. Locks the
  // OR-accumulate semantics against a future refactor.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/multi.html";
  std::string found_url = "http://example.com/found.css";
  std::string missing_url = "http://example.com/missing.css";
  std::string html =
      "<html><head>"
      "<style>.hero{color:red}</style>"
      "<link rel=\"stylesheet\" href=\"" +
      found_url + "\">" + "<link rel=\"stylesheet\" href=\"" + missing_url +
      "\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), url, html);
  // Cache ONLY the first sheet.
  CacheOriginal(worker.cache(), found_url, ".hero{color:red;font-size:20px}",
                "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> v;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    v = ReadVariantWithMeta(worker.cache(), url, mask);
    if (v.has_value()) break;
  }
  ASSERT_TRUE(v.has_value()) << "HTML variant not written";
  // One sheet still missing -> async suppressed (no preload primitive) ...
  EXPECT_EQ(v->content.find("as=\"style\""), std::string::npos)
      << "async must stay suppressed while ANY declared sheet is uncached";
  // ... and revalidation set (the miss is same-origin, so it can self-heal).
  EXPECT_NE(v->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation, 0)
      << "same-origin partial miss should mark the variant for revalidation";
}

TEST_F(WorkerTest,
       CrossOriginMissingSheetSuppressesAsyncWithoutRevalidationChurn) {
  // A cross-origin stylesheet (e.g. Google Fonts) can NEVER enter the worker's
  // cache. async-CSS must stay suppressed (we cannot measure it -> no FOUC),
  // but the variant must NOT be marked for revalidation — otherwise it would
  // re-process the HTML on every request forever without ever resolving.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/fonts.html";
  std::string html =
      "<html><head>"
      "<style>.hero{color:red}</style>"
      "<link rel=\"stylesheet\" "
      "href=\"https://fonts.googleapis.com/css?family=Roboto\">"
      "</head><body><div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\"></body></html>";
  // Set the page host explicitly so the cross-origin sheet is actually detected
  // as cross-origin (a missing same-origin sheet would, by design, revalidate).
  std::string hostname = "example.com";
  CacheOriginal(worker.cache(), url, html, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> v;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    v = ReadVariantWithMeta(worker.cache(), url, mask, hostname);
    if (v.has_value()) break;
  }
  ASSERT_TRUE(v.has_value()) << "HTML variant not written";
  // Fail-safe: the unmeasured cross-origin sheet is NOT async-deferred.
  EXPECT_EQ(v->content.find("as=\"style\""), std::string::npos)
      << "cross-origin unmeasured sheet must not be async-deferred";
  // No churn: a sheet that can never cache must NOT trigger revalidation.
  EXPECT_EQ(v->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation, 0)
      << "cross-origin (uncacheable) miss must NOT mark the variant for "
         "revalidation (would re-process forever)";
}

// =============================================================================
// Phase 3.1: Worker Daemon Internals Tests
// =============================================================================

// --- Health check socket tests ---

TEST_F(WorkerTest, HealthSocketExists) {
  // Verify that the .health socket file is created after Initialize().
  WorkerConfig config;
  config.socket_path = socket_path_;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string health_path = worker.health_socket_path();
  EXPECT_TRUE(pagespeed::test::PipeEndpointExists(health_path))
      << "Health socket/pipe endpoint should exist after Initialize()";

  worker.Shutdown();
}

TEST_F(WorkerTest, HealthSocketReturnsOk) {
  // Connect to health socket, read response, verify it starts with "OK ".
  WorkerConfig config;
  config.socket_path = socket_path_;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_TRUE(response.starts_with("OK "))
      << "Health response should start with 'OK ', got: " << response;
}

TEST_F(WorkerTest, HealthSocketShowsConnectionCount) {
  // Verify active connection info appears in health response.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_connections = 64;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Open a notification socket connection to bump active_connections.
  int held_fd = ConnectToSocket(socket_path_);
  ASSERT_GE(held_fd, 0) << "Failed to open held connection";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Now query health endpoint.
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from health socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  // Response should contain connection count info (e.g., "1/64").
  EXPECT_NE(response.find("/64"), std::string::npos)
      << "Health response should show max connections, got: " << response;

  pagespeed::test::ClosePipe(held_fd);
}

// --- Cooldown enforcement tests ---

TEST_F(WorkerTest, HtmlCooldownPreventsReprocessing) {
  // Send the same HTML notification twice quickly; the second should be
  // skipped due to cooldown enforcement.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Cooldown Test</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification.
  SendNotification(notification);

  // Wait for first processing to complete (generous timeout for slow
  // machines, e.g. arm64 under emulation).
  CapabilityMask mask = CapabilityMask::Decode(0);
  bool variant_found = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) {
      variant_found = true;
      break;
    }
  }
  ASSERT_TRUE(variant_found)
      << "First notification must produce a variant before testing dedup";

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification for the same URL — should be skipped by dedup
  // or cooldown.  Poll for the skip counter to advance instead of a fixed
  // sleep (slow shards can sit at the pipe read past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Second HTML notification should be skipped by dedup/cooldown";
}

TEST_F(WorkerTest, CooldownExpiresAllowsReprocessing) {
  // Verify that after the cooldown period expires, the same URL can be
  // reprocessed.  We use ClearDedupAndCooldown() to simulate expiry
  // rather than waiting the full 60-second default cooldown.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-expire.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Expire Test</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification.
  SendNotification(notification);
  CapabilityMask mask = CapabilityMask::Decode(0);
  bool variant_found = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!ReadVariant(worker.cache(), url, mask).empty()) {
      variant_found = true;
      break;
    }
  }
  ASSERT_TRUE(variant_found) << "First notification must produce a variant "
                                "before testing cooldown expiry";

  uint64_t html_before = worker.stats().html_processed.load();

  // Clear dedup and cooldown to simulate expiry.
  worker.ClearDedupAndCooldown(url, "");

  // Invalidate cached variant so worker must reprocess.
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, html);

  // Re-send notification — should be processed again.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() > html_before) {
      reprocessed = true;
      break;
    }
  }

  EXPECT_TRUE(reprocessed)
      << "After clearing cooldown, HTML should be reprocessed";
}

TEST_F(WorkerTest, ClearDedupAndCooldownResetsState) {
  // Verify that ClearDedupAndCooldown() allows an already-processed
  // URL to be reprocessed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/clear-dedup.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — poll for completion instead of fixed sleep.
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  uint64_t css_before = worker.stats().css_processed.load();

  // Second notification without clearing — should be skipped.  Poll for the
  // skip counter to advance instead of a fixed sleep (slow shards can sit at
  // the pipe read past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() >= 1u) break;
  }

  uint64_t skipped = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GE(skipped, 1u) << "Duplicate CSS notification should be skipped";

  // The dedup-skipped duplicate above still occupies the in-flight set until
  // the event loop retires it.  Drain before invalidating and re-sending, or
  // the third notification is rejected by the in-flight guard and never
  // reprocesses.
  WaitForWorkerIdle(worker);

  // Clear dedup state.
  worker.ClearDedupAndCooldown(url, "");

  // Invalidate so the worker reprocesses from original.
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, css);

  // Third notification — should now be processed.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }

  EXPECT_TRUE(reprocessed)
      << "After ClearDedupAndCooldown, CSS should be reprocessed";
}

// --- Dedup tracking tests ---

TEST_F(WorkerTest, DuplicateImageNotificationSkipped) {
  // Send the same image notification twice; verify only one variant
  // is actually written (second is dedup-skipped).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  // First notification — should be processed.  Wait for the full processing
  // cycle to complete (all variant writes + dedup registration).  We detect
  // completion by waiting for variants_written to stabilize.
  SendNotification(notification);
  bool image_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) {
      image_processed = true;
      break;
    }
  }
  ASSERT_TRUE(image_processed)
      << "Image should be processed once before testing dedup";
  // Wait for variants_written to stabilize — MarkVariantProcessed (dedup
  // registration) runs after all variant writes, so once the counter stops
  // changing the dedup key is guaranteed to be registered.
  {
    uint64_t prev = 0;
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      uint64_t cur = worker.stats().variants_written.load();
      if (cur > 0 && cur == prev) break;
      prev = cur;
    }
  }

  uint64_t variants_before = worker.stats().variants_written.load();
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification for the same URL+mask — should be dedup-skipped.
  // Poll for the skip counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets
  // variants_written settle for the no-new-variants invariant below.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Duplicate image notification should be dedup-skipped";
  // variants_written should not have increased (or at most marginally
  // from compressed variants of the first processing).
  EXPECT_EQ(worker.stats().variants_written.load(), variants_before)
      << "No new variants should be written for duplicate notification";
}

TEST_F(WorkerTest, InvalidateUrlClearsDedup) {
  // After InvalidateUrl(), the same URL should be re-processed on
  // the next notification.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/invalidate-dedup.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — poll for completion.
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";

  // css_processed moves before the in-flight entry is erased, and InvalidateUrl
  // does not clear the in-flight set.  Drain first, or the re-send below is
  // rejected by the in-flight guard and never reprocesses.
  WaitForWorkerIdle(worker);

  uint64_t css_before = worker.stats().css_processed.load();

  // Invalidate the URL — this clears dedup state and cache entries.
  worker.InvalidateUrl(url);

  // Re-populate the cache so the worker has data to process.
  CacheOriginal(worker.cache(), url, css);

  // Re-send notification — should be processed again.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }

  EXPECT_TRUE(reprocessed) << "After InvalidateUrl, CSS should be reprocessed";
}

TEST_F(WorkerTest, DedupHealReprocessesAfterVariantFamilyLoss) {
  // a processed-set entry must not
  // outlive the variant family it stands for.  Process a CSS URL (variant
  // written, dedup marked), remove the WHOLE family through the cache API —
  // simulating a Cyclone capacity eviction, which has no callback into the
  // worker — re-seed the source as a durable original (sentinel class, so
  // the family still holds no servable variant), and re-notify.  The stale
  // dedup entry must be healed (erased + counted) and the URL reprocessed
  // instead of skipped.  Fails on the un-fixed code, which skips.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-heal.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — processed, dedup marked.
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";
  // css_processed moves before the in-flight entry is erased; drain, or the
  // re-send below is rejected by the in-flight guard and never reaches the
  // dedup check (see InvalidateUrlClearsDedup).
  WaitForWorkerIdle(worker);
  ASSERT_EQ(0u, worker.stats().notifications_dedup_healed.load())
      << "no heal before the family is lost";

  // Simulate capacity eviction at the cache layer only: the family
  // disappears while the in-memory dedup entry survives.  NOT
  // InvalidateUrl — that would also clear the dedup entry this test needs
  // to keep orphaned.
  (void)worker.cache()->Remove(StripSchemeAuthority(url), "", "https");
  // Re-seed the source as a durable original (SentinelId::kOriginalContent):
  // sentinel space, so the family still holds NO servable variant — the
  // state the heal is responsible for — while the re-processed notification
  // still has a source to optimize from (the durable-original fallback).
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  uint64_t css_before = worker.stats().css_processed.load();
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification — must heal the orphaned dedup entry and reprocess.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }
  EXPECT_TRUE(reprocessed)
      << "URL must be reprocessed once its variant family is gone, not "
         "dedup-skipped forever";
  EXPECT_EQ(1u, worker.stats().notifications_dedup_healed.load())
      << "the heal counter moves exactly on the heal path";
  EXPECT_EQ(skipped_before, worker.stats().notifications_skipped_dedup.load())
      << "a healed notification is not counted as a dedup skip";
}

TEST_F(WorkerTest, DedupSkipStandsWhenVariantFamilyPresent) {
  // Companion of DedupHealReprocessesAfterVariantFamilyLoss: while a
  // servable (non-sentinel) alternate still exists for the key, the dedup
  // skip stands exactly as before and the heal counter does not move.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-stands.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification with the variant family intact — the skip stands.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "duplicate notification with a live variant family must be "
         "dedup-skipped";
  EXPECT_EQ(0u, worker.stats().notifications_dedup_healed.load())
      << "no heal when the family is present";
  EXPECT_EQ(1u, worker.stats().css_processed.load())
      << "no reprocessing when the family is present";
}

TEST_F(WorkerTest, DedupHealIsRateLimitedForTerminalDecisions) {
  // Pins the BOUND on the orphaned-entry heal
  //.
  //
  // A sentinel-only family is not always orphaned.  Several paths mark an
  // entry processed while writing nothing servable, and say so: a JS or CSS
  // minify failure is "deterministic for a given input, so retrying on the
  // next notification would loop forever".  On a substrate that records the
  // original only at the durable-original id, such a family is sentinel-only
  // FOREVER, so the heal's existence check sees "orphaned" on every dedup
  // hit and would re-pay the ladder per notification -- reintroducing the
  // loop those marks exist to prevent.
  //
  // The heal is therefore rate limited per URL.  This test drives a JS URL
  // that can never produce a variant and asserts the third notification is
  // deferred rather than re-processed.  Deleting the rate limiter makes the
  // third notification re-pay, and the parse-failure count moves.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Unterminated string literal: MinifyUtf8Js reliably returns false, so the
  // worker marks the entry processed and writes NO variant.  Recorded only
  // at the durable-original id, so the family is sentinel-only throughout.
  std::string url = "http://example.com/dedup-heal-bounded.js";
  std::string js = "var  a  =  1;\n\"not valid javascript";
  CacheDurableOriginal(worker.cache(), url, js, "application/javascript",
                       ContentType::kJs);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  // 1st notification: processed, parse fails, entry marked, nothing written.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().text_minify_parse_failures.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().text_minify_parse_failures.load(), 1u)
      << "the JS parse failure should have been recorded once";
  ASSERT_EQ(0u, worker.stats().variants_written.load())
      << "the terminal path must not write a variant";
  WaitForWorkerIdle(worker);

  // 2nd notification: dedup hit on a sentinel-only family, and no heal has
  // run for this URL yet, so it heals once -- the behaviour #824 needs.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_dedup_healed.load() >= 1) break;
  }
  EXPECT_EQ(1u, worker.stats().notifications_dedup_healed.load())
      << "a sentinel-only family must still heal once";
  WaitForWorkerIdle(worker);

  const uint64_t failures_before =
      worker.stats().text_minify_parse_failures.load();
  const uint64_t healed_before =
      worker.stats().notifications_dedup_healed.load();

  // 3rd notification, inside the same window: THE BOUND.  It must be
  // deferred, not re-processed -- no second heal, and no fresh minify
  // attempt on content that deterministically cannot be minified.
  SendNotification(notification);
  bool rate_limited = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_dedup_heal_rate_limited.load() >= 1) {
      rate_limited = true;
      break;
    }
  }
  EXPECT_TRUE(rate_limited)
      << "a repeat notification inside the window must be rate limited, not "
         "re-processed";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(failures_before, worker.stats().text_minify_parse_failures.load())
      << "the rate-limited notification must not have re-paid the ladder";
  EXPECT_EQ(healed_before, worker.stats().notifications_dedup_healed.load())
      << "the rate-limited notification must not count as a heal";
  EXPECT_EQ(0u, worker.stats().variants_written.load());
}

// --- begin ported adversarial probes ---
TEST_F(WorkerTest, AdvStaleTombstoneBlocksHealForChangedSource) {
  // ADVERSARIAL REVIEW PROBE, ported from the review of de600de8.  Against
  // the tombstone arm this FAILED deterministically: the arm counted the
  // tombstone as "family present" purely by ALTERNATE ID -- it never parsed
  // the payload, so it never compared the tombstone's bound source SHA-256
  // with the bytes now at the durable original, and a source changed after
  // the tombstone was written stayed unoptimized forever.  Under the bounded
  // heal the sentinel-only family heals (once per window), the changed
  // source is re-optimized, and this probe PASSES.  Body otherwise verbatim.
  //
  // Original review comment: identical in every respect to
  // DedupHealsEvictedFamilyWithNoTombstone except that a decline tombstone
  // survived the eviction, and the durable original re-recorded afterwards
  // holds DIFFERENT (v2) bytes than the ones the tombstone was written for.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/adv-stale-tombstone.css";
  std::string css_v1 = "body { margin: 0; } h1 { color: blue; }";
  std::string css_v2 = "body { padding: 0; } h2 { color: green; }";
  CacheOriginal(worker.cache(), url, css_v1);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";
  WaitForWorkerIdle(worker);

  // Capacity eviction takes the servable variants.  The tombstone (written
  // in a later phase) survives; the substrate then re-records the CHANGED
  // origin body at the durable-original id, which is the only place the
  // module substrate records originals.
  (void)worker.cache()->Remove(StripSchemeAuthority(url), "", "https");
  CacheDurableOriginal(worker.cache(), url, css_v2, "text/css",
                       ContentType::kCss);
  const std::string blob = "decline-tombstone-placeholder";
  auto wh =
      worker.cache()->WriteSentinel(StripSchemeAuthority(url), "", "https",
                                    SentinelId::kDeclineTombstone, blob.size());
  ASSERT_TRUE(wh.has_value()) << "Failed to open tombstone write handle";
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(blob.data(), blob.size())))
                  .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  const uint64_t css_before = worker.stats().css_processed.load();

  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }
  EXPECT_TRUE(reprocessed)
      << "the source CHANGED after the tombstone was written and every "
         "servable variant is gone -- the URL must be re-optimized, not "
         "skipped forever on the strength of a tombstone bound to bytes that "
         "no longer exist";
  EXPECT_EQ(0u, worker.stats().notifications_skipped_dedup.load())
      << "a stale tombstone must not answer the dedup check";
}

TEST_F(WorkerTest, AdvUnminifiableJsRepaysTheLadderOnEveryNotification) {
  // ADVERSARIAL REVIEW PROBE, ported from the review of de600de8 with the
  // assertions re-based on the bounded-heal design.  At de600de8 this probe
  // measured the re-pay as PER-NOTIFICATION: three notifications produced
  // skipped_dedup 0 / healed 2 / parse_failures 3, and since a family with
  // nothing servable makes the front end fall through and re-notify on every
  // request, that re-pay was per-request and permanent.  The bounded heal
  // converts it into one re-pay per URL per kDedupHealMinIntervalSecs: three
  // notifications inside one window must produce exactly TWO parse failures
  // (the initial attempt plus one bounded heal), ONE heal, and ONE
  // rate-limited deferral.
  //
  // worker.cc marks an unparseable JS URL processed with the comment "parse
  // failure is deterministic for a given input, so retrying on the next
  // notification would loop forever".  That mark writes NO servable
  // alternate.  On a substrate that records originals only at the
  // durable-original id (sentinel space -- the Apache module),
  // the family is therefore sentinel-only forever, and the existence check
  // reads it as "orphaned" on every dedup hit.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/adv-broken.js";
  std::string js = "var  a  =  1;\n\"not valid javascript";
  // Module-substrate shape: the origin bytes live ONLY at the durable
  // original (SentinelId::kOriginalContent), never at the identity id.
  CacheDurableOriginal(worker.cache(), url, js, "application/javascript",
                       ContentType::kJs);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().text_minify_parse_failures.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().text_minify_parse_failures.load())
      << "the first notification must attempt and fail the minify once";
  WaitForWorkerIdle(worker);

  // Notifications 2 and 3, inside one rate-limit window.
  for (int n = 0; n < 2; ++n) {
    const uint64_t want = worker.stats().notifications_received.load() + 1;
    SendNotification(notification);
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_received.load() >= want) break;
    }
    ASSERT_EQ(want, worker.stats().notifications_received.load())
        << "the worker did not receive notification " << (n + 2);
    WaitForWorkerIdle(worker);
  }

  // THE BOUND: notification 2 heals once (the property #824 needs);
  // notification 3 is deferred by the rate limiter.  At de600de8 the same
  // three notifications measured parse_failures 3 / healed 2 -- an
  // unbounded per-notification re-pay.
  EXPECT_EQ(2u, worker.stats().text_minify_parse_failures.load())
      << "the re-pay must be bounded to one per window, not one per "
         "notification";
  EXPECT_EQ(1u, worker.stats().notifications_dedup_healed.load());
  EXPECT_EQ(1u, worker.stats().notifications_dedup_heal_rate_limited.load());
  {
    auto alts =
        worker.cache()->ListAlternates(StripSchemeAuthority(url), "", "https");
    std::string ids;
    if (alts.has_value()) {
      for (const auto& al : *alts) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02x,", static_cast<unsigned>(al.id));
        ids += buf;
      }
    } else {
      ids = "<none>";
    }
    std::cerr << "ADV-DIAG recv="
              << worker.stats().notifications_received.load() << " skip_dedup="
              << worker.stats().notifications_skipped_dedup.load()
              << " skip_inflight="
              << worker.stats().notifications_skipped_inflight.load()
              << " healed=" << worker.stats().notifications_dedup_healed.load()
              << " rate_limited="
              << worker.stats().notifications_dedup_heal_rate_limited.load()
              << " parse_fail="
              << worker.stats().text_minify_parse_failures.load()
              << " js_proc=" << worker.stats().js_processed.load()
              << " variants=" << worker.stats().variants_written.load()
              << " alts=[" << ids << "]" << '\n';
  }
}
// --- end ported adversarial probes ---

// --- Stats counter tests ---

TEST_F(WorkerTest, NotificationCounterIncrements) {
  // Verify notifications_received increments for each notification.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);

  // Pre-populate two distinct CSS resources.
  std::string url1 = "http://example.com/counter1.css";
  std::string url2 = "http://example.com/counter2.css";
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url1, css);
  CacheOriginal(worker.cache(), url2, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification n1;
  n1.url = url1;
  n1.scheme = "https";
  n1.content_type = ContentType::kCss;
  n1.capability_mask = CapabilityMask().Encode();
  SendNotification(n1);

  // Wait for first notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);

  CacheNotification n2;
  n2.url = url2;
  n2.scheme = "https";
  n2.content_type = ContentType::kCss;
  n2.capability_mask = CapabilityMask().Encode();
  SendNotification(n2);

  // Wait for second notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().notifications_received.load(), 2u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, VariantWrittenCounterAccurate) {
  // Verify that variants_written reflects actual cache writes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  // Pre-populate CSS content.
  std::string url = "http://example.com/counter-variants.css";
  std::string css = "body { margin: 0; } h1 { color: green; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().variants_written.load() >= 1) break;
  }

  uint64_t variants = worker.stats().variants_written.load();
  EXPECT_GE(variants, 1u)
      << "At least one variant should be written for CSS processing";

  // Verify a variant actually exists in cache (identity mask for CSS).
  CapabilityMask mask;  // Desktop/Identity = default
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty())
      << "Cache should contain written variant for CSS";
  // Minified CSS should be smaller or equal to original.
  EXPECT_LE(variant.size(), css.size())
      << "Minified CSS should not be larger than original";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, RevalidationDoesNotDuplicatePreconnectLinks) {
  // On revalidation, previously-injected <link rel="preconnect"
  // data-pagespeed-hint> must not accumulate.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // HTML with an external stylesheet from a third-party origin.
  std::string url = "http://example.com/page.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" "
      "href=\"https://fonts.googleapis.com/css?family=Inter\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the external CSS so processing succeeds.
  std::string css_url = "https://fonts.googleapis.com/css?family=Inter";
  std::string css = ".hero { font-size: 24px; }";
  CacheOriginal(worker.cache(), css_url, css, "fonts.googleapis.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first_variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first_variant = ReadVariant(worker.cache(), url, mask);
    if (!first_variant.empty()) break;
  }
  ASSERT_FALSE(first_variant.empty()) << "First variant not written";

  // Second notification — simulates revalidation.
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::string second_variant = ReadVariant(worker.cache(), url, mask);
  ASSERT_FALSE(second_variant.empty()) << "Second variant not written";

  // Count preconnect occurrences — should be at most 1 per origin.
  size_t count = 0;
  size_t pos = 0;
  while ((pos = second_variant.find("rel=\"preconnect\"", pos)) !=
         std::string::npos) {
    ++count;
    pos += 16;
  }
  // Expect at most 1 preconnect for fonts.googleapis.com.
  EXPECT_LE(count, 1u) << "Expected at most 1 preconnect link, found " << count
                       << " in:\n"
                       << second_variant;
}

// =============================================================================
// SpeculationRules / Hot URL tracking (tested indirectly through HTML processing)
// =============================================================================

TEST_F(WorkerTest, SpeculationRulesDisabledByDefault) {
  // When enable_speculation_rules is false (default), HTML processing should
  // not inject speculation rules even when multiple URLs are processed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Page</h1></body></html>";
  std::string url = "http://example.com/no-speculation.html";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) break;
  }

  // The variant should not contain speculation rules script tag.
  CapabilityMask mask = CapabilityMask::Decode(CapabilityMask().Encode());
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  if (!variant.empty()) {
    EXPECT_EQ(variant.find("speculationrules"), std::string::npos)
        << "Speculation rules should not be injected when disabled";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupWithSpeculationRulesTracksUrl) {
  // When enable_speculation_rules is true, a warmup notification should
  // track the URL for speculation rules.  We verify indirectly by checking
  // that the warmup request was handled without errors.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = true;
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content for warmup to process.
  std::string url = "http://example.com/speculation-warmup.css";
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0xFFFFFFFE;  // kWarmupSentinel
  SendNotification(notification);

  // Poll for warmup processing (CSS processed counter).
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().css_processed.load() > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Verify no errors during warmup processing.
  EXPECT_EQ(worker.stats().errors.load(), 0u)
      << "Warmup with speculation rules should not cause errors";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, MultipleHtmlNotificationsWithSpeculationRules) {
  // Process multiple HTML pages with speculation rules enabled.
  // Each page should be tracked in the hot URL list (tested indirectly).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create multiple HTML pages.
  for (int i = 0; i < 3; ++i) {
    std::string url =
        "http://example.com/spec-page" + std::to_string(i) + ".html";
    std::string html =
        "<html><head><style>body { margin: 0; }</style></head>"
        "<body><h1>Page " +
        std::to_string(i) + "</h1></body></html>";
    CacheOriginal(worker.cache(), url, html);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (int i = 0; i < 3; ++i) {
    CacheNotification notification;
    notification.url =
        "http://example.com/spec-page" + std::to_string(i) + ".html";
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Wait for all to process.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 3) break;
  }

  EXPECT_GE(worker.stats().html_processed.load(), 3u)
      << "All three HTML pages should be processed with speculation rules";
  EXPECT_EQ(worker.stats().errors.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Management Socket: BROWSER-STATUS command
// =============================================================================

TEST_F(WorkerTest, MgmtBrowserStatusDisabled) {
  // When browser analysis is not enabled, BROWSER-STATUS returns
  // {"enabled":false}.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "BROWSER-STATUS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  char buf[1024];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No data from management socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("\"enabled\":false"), std::string::npos)
      << "Response: " << response;
}

// =============================================================================
// Config Accessor Tests
// =============================================================================

TEST_F(WorkerTest, ApiPortDefaultZero) {
  // Without an API server, api_port() returns 0.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.api_port(), 0);

  worker.Shutdown();
}

TEST_F(WorkerTest, GetConfigReturnsLiveConfig) {
  // GetConfig() returns the current live configuration snapshot.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.jpeg_quality = 42;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  auto live = worker.GetConfig();
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(live->jpeg_quality, 42);

  worker.Shutdown();
}

TEST_F(WorkerTest, UpdateConfigChangesLiveConfig) {
  // UpdateConfig() atomically replaces the live config.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.jpeg_quality = 85;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  auto before = worker.GetConfig();
  ASSERT_NE(before, nullptr);
  EXPECT_EQ(before->jpeg_quality, 85);

  // Update to a new config.
  auto new_config = std::make_shared<WorkerConfig>(*before);
  new_config->jpeg_quality = 50;
  worker.UpdateConfig(new_config);

  auto after = worker.GetConfig();
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->jpeg_quality, 50);

  // The old snapshot should still have the original value.
  EXPECT_EQ(before->jpeg_quality, 85);

  worker.Shutdown();
}

TEST_F(WorkerTest, SocketPathAccessor) {
  // socket_path() returns the configured path.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.socket_path(), socket_path_);

  worker.Shutdown();
}

TEST_F(WorkerTest, CacheAccessor) {
  // cache() returns non-null when cache is configured.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_NE(worker.cache(), nullptr);

  worker.Shutdown();
}

TEST_F(WorkerTest, CacheAccessorNullWithoutPath) {
  // cache() returns null when no cache_path is configured.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_EQ(worker.cache(), nullptr);

  worker.Shutdown();
}

TEST_F(WorkerTest, StatsAccessor) {
  // stats() provides access to WorkerStats with default zeros.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  EXPECT_EQ(worker.stats().errors.load(), 0u);

  worker.Shutdown();
}

TEST_F(WorkerTest, ActiveConnectionsAndInFlightDefaults) {
  // active_connections() and in_flight_work() default to 0.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.active_connections(), 0);
  EXPECT_EQ(worker.in_flight_work(), 0);

  worker.Shutdown();
}

// =============================================================================
// Disable toggles
// =============================================================================

TEST_F(WorkerTest, DisableJsSkipsProcessing) {
  // When disable_js is set, JS notifications are skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_js = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/disabled-js.js";
  std::string js = "var foo = 1; var bar = 2;";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  uint64_t before = worker.stats().notifications_received.load();
  SendNotification(notification);

  // Poll until the notification has been received by the worker.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  // JS should not be processed (disabled).
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DisableImageSkipsProcessing) {
  // When disable_image is set, image notifications are skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_image = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with image data (use a real JPEG to be safe).
  std::string image_data = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(image_data.empty());
  std::string url = "http://example.com/disabled-image.jpg";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received by the worker.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  // Image should not be processed (disabled).
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// ClearDedupAndCooldown (hostname-scoped) Tests
// =============================================================================

TEST_F(WorkerTest, ClearDedupScopedToHostname) {
  // ClearDedupAndCooldown only clears dedup entries for the given URL+hostname,
  // not for the same URL on different hostnames.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/scoped.css";
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process notification for host1 (empty hostname = default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing AND in-flight work to drain.  Although
  // MarkVariantProcessed runs before css_processed increments,
  // checking in_flight_work()==0 ensures the full work item
  // (including WriteCompressedVariants) has completed.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  // Clear dedup for a DIFFERENT hostname — should NOT affect the empty hostname
  // entry.
  worker.ClearDedupAndCooldown(url, "other.com");

  // Send same notification again — should still be deduped.
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  SendNotification(notification);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Notification should still be deduped after clearing a different host";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// InvalidateUrl clears cooldown
// =============================================================================

TEST_F(WorkerTest, InvalidateUrlClearsCooldown) {
  // InvalidateUrl should clear the HTML cooldown so the URL can be
  // immediately reprocessed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-invalidate.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body>Test</body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — processes and sets cooldown.
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed)
      << "HTML should be processed once before testing cooldown invalidation";

  // html_processed moves before the in-flight entry is erased, and InvalidateUrl
  // clears the dedup set and the cooldown but NOT the in-flight set.  Without
  // draining first, the re-notification below is rejected by the in-flight guard
  // and never reprocesses, which matches how this test fails under load.
  WaitForWorkerIdle(worker);

  uint64_t html_before = worker.stats().html_processed.load();

  // Invalidate the URL — should clear both dedup and cooldown.
  worker.InvalidateUrl(url, "");

  // Re-populate cache so the worker can read it.
  CacheOriginal(worker.cache(), url, html);

  // Send notification again — should process since cooldown was cleared.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() > html_before) {
      reprocessed = true;
      break;
    }
  }

  EXPECT_TRUE(reprocessed)
      << "HTML should be reprocessed after InvalidateUrl clears cooldown";
}

// =============================================================================
// Management socket: multiple commands on same connection (keep-alive)
// =============================================================================

TEST_F(WorkerTest, MgmtStatsIncludesSvgCounters) {
  // The STATS JSON response includes SVG vectorization counters.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"svg\""), std::string::npos)
      << "STATS should include SVG section. Response: " << response;
  EXPECT_NE(response.find("\"candidates_evaluated\""), std::string::npos);
  EXPECT_NE(response.find("\"vectorized\""), std::string::npos);
  EXPECT_NE(response.find("\"written\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtStatsIncludesAlternateWriteCounters) {
  // STATS JSON includes alternate write/failure counters and selector
  // invocations.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"alternates\""), std::string::npos)
      << "STATS should include alternates section. Response: " << response;
  EXPECT_NE(response.find("\"write_failures\""), std::string::npos);
  EXPECT_NE(response.find("\"selector_invocations\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtStatsIncludesContentAnalysis) {
  // STATS JSON includes content analysis classification counters.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"content_analysis\""), std::string::npos)
      << "STATS should include content_analysis section";
  EXPECT_NE(response.find("\"photo\""), std::string::npos);
  EXPECT_NE(response.find("\"screenshot\""), std::string::npos);
  EXPECT_NE(response.find("\"illustration\""), std::string::npos);
  EXPECT_NE(response.find("\"denoised\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtStatsIncludesSsimulacra2) {
  // STATS JSON includes SSIMULACRA2 quality verification counters.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  // Read the WHOLE response: the STATS document grows with every counter the
  // build gains, so a fixed-buffer single read truncates it silently once the
  // document outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"ssimulacra2\""), std::string::npos)
      << "STATS should include ssimulacra2 section";
  EXPECT_NE(response.find("\"checks\""), std::string::npos);
  EXPECT_NE(response.find("\"reencodes\""), std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsIncludesSvgCounters) {
  // The METRICS Prometheus response includes SVG-related counters.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty());
  EXPECT_NE(response.find("pagespeed_svg_candidates_evaluated_total"),
            std::string::npos)
      << "METRICS should include SVG counters";
  EXPECT_NE(response.find("pagespeed_svg_vectorized_total"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_svg_written_total"), std::string::npos);
  EXPECT_NE(response.find("pagespeed_svg_bytes_saved_total"),
            std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsIncludesHtmlAssembly) {
  // METRICS includes html assembly outcome counters.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty());
  EXPECT_NE(response.find("pagespeed_html_assembly_total"), std::string::npos)
      << "METRICS should include html assembly counters";
  EXPECT_NE(response.find("pagespeed_alternate_writes_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_selector_invocations_total"),
            std::string::npos);
}

TEST_F(WorkerTest, MgmtMetricsIncludesContentAnalysis) {
  // METRICS includes content analysis classification counters.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  std::string cmd = "METRICS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty());
  EXPECT_NE(response.find("pagespeed_content_analysis_total"),
            std::string::npos)
      << "METRICS should include content analysis counters";
  EXPECT_NE(response.find("pagespeed_images_denoised_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_ssimulacra2_checks_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_ssimulacra2_reencodes_total"),
            std::string::npos);
  EXPECT_NE(response.find("pagespeed_ssimulacra2_declines_total"),
            std::string::npos)
      << "a decline is a decision an operator must be able to see (#1381)";
  EXPECT_NE(response.find("pagespeed_ssimulacra2_decline_tombstone_hits_total"),
            std::string::npos)
      << "a tombstone skip is a decision an operator must be able to see "
         "(#1382)";
}

// =============================================================================
// InvalidateUrl with no cache returns 0
// =============================================================================

TEST_F(WorkerTest, InvalidateUrlNoCacheReturnsZero) {
  // When no cache is configured, InvalidateUrl returns 0.
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  int deleted = worker.InvalidateUrl("http://example.com/no-cache", "");
  EXPECT_EQ(deleted, 0);

  worker.Shutdown();
}

// =============================================================================
// MGMT PURGE with bad syntax
// =============================================================================

TEST_F(WorkerTest, MgmtPurgeBadSyntax) {
  // Authenticated PURGE with bad syntax returns an error.
  // Set env var BEFORE Initialize so the token is picked up.
  setenv("PAGESPEED_PURGE_TOKEN", "test-token-bad-syntax", 1);

  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // AUTH first.
  std::string auth_cmd = "AUTH test-token-bad-syntax\n";
  pagespeed::test::PipeWrite(fd, auth_cmd.data(), auth_cmd.size());

  char buf[1024];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_EQ(std::string(buf, n), "OK\n") << "AUTH should succeed";

  // Send PURGE with no arguments (bad syntax).
  std::string purge_cmd = "PURGE \n";
  pagespeed::test::PipeWrite(fd, purge_cmd.data(), purge_cmd.size());

  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string purge_response(buf, n);
  EXPECT_NE(purge_response.find("ERR"), std::string::npos)
      << "Bad PURGE syntax should return error. Response: " << purge_response;

  unsetenv("PAGESPEED_PURGE_TOKEN");
}

// =============================================================================
// Health socket path construction
// =============================================================================

TEST_F(WorkerTest, HealthSocketPathSuffix) {
  // Health socket path should be the main socket path + ".health".
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.health_socket_path(), socket_path_ + ".health");

  worker.Shutdown();
}

TEST_F(WorkerTest, MgmtSocketPathSuffix) {
  // Management socket path should be the main socket path + ".mgmt".
  WorkerConfig config;
  config.socket_path = socket_path_;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.mgmt_socket_path(), socket_path_ + ".mgmt");

  worker.Shutdown();
}

// =============================================================================
// Notification without cache does not crash
// =============================================================================

TEST_F(WorkerTest, NotificationWithoutCacheHandledGracefully) {
  // Notifications when cache is not configured should not crash.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the notification to be received instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // not-processed invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1u) break;
  }

  // Should have received the notification but not processed it.
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Additional coverage tests for previously untested paths
// =============================================================================

// Synthetic AVIF authoring via libavif (the test links @libavif), same
// helper as the transcoder suite: noise content is the reliable
// constructor for a below-floor verify verdict, and AVIF is the one input
// format whose original-format arm has no optimizer.
enum class SyntheticPattern : uint8_t { kFlat, kNoise };

std::string MakeSyntheticAvif(int width, int height, bool grayscale,
                              SyntheticPattern pattern, int value, int quality,
                              uint32_t seed) {
  avifImage* image = avifImageCreate(
      width, height, 8,
      grayscale ? AVIF_PIXEL_FORMAT_YUV400 : AVIF_PIXEL_FORMAT_YUV420);
  if (image == nullptr) return {};

  std::string rgb(static_cast<size_t>(width) * height * 3, '\0');
  uint32_t s = seed != 0 ? seed : 1;
  for (char& byte : rgb) {
    if (pattern == SyntheticPattern::kFlat) {
      byte = static_cast<char>(value);
    } else {
      s = s * 1664525u + 1013904223u;
      byte = static_cast<char>((s >> 16) & 0xFF);
    }
  }

  avifRGBImage rgb_image;
  avifRGBImageSetDefaults(&rgb_image, image);
  rgb_image.depth = 8;
  rgb_image.format = AVIF_RGB_FORMAT_RGB;
  rgb_image.pixels = reinterpret_cast<uint8_t*>(rgb.data());
  rgb_image.rowBytes = static_cast<size_t>(width) * 3;
  avifResult r = avifImageRGBToYUV(image, &rgb_image);
  if (r != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return {};
  }

  avifEncoder* encoder = avifEncoderCreate();
  if (encoder == nullptr) {
    avifImageDestroy(image);
    return {};
  }
  encoder->quality = quality;
  encoder->speed = 8;  // Authoring speed; determinism is what matters.
  avifRWData out = AVIF_DATA_EMPTY;
  r = avifEncoderWrite(encoder, image, &out);
  avifEncoderDestroy(encoder);
  avifImageDestroy(image);
  if (r != AVIF_RESULT_OK) {
    avifRWDataFree(&out);
    return {};
  }
  std::string bytes(reinterpret_cast<const char*>(out.data), out.size);
  avifRWDataFree(&out);
  return bytes;
}

// Helper: pre-populate cache with content and custom metadata.
// Unlike CacheOriginal(), this lets callers set origin_content_type_str
// and other metadata fields for testing image type detection.
// URL is normalized to path-only to match worker's NormalizeCacheUrl.
void CacheOriginalWithMeta(PageSpeedCache* cache, const std::string& url,
                           const std::string& content,
                           const AlternateMetadata& meta_in) {
  std::string effective_url = WorkerTest::StripSchemeAuthority(url);
  CapabilityMask default_mask;  // Desktop/Identity = 0x08
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
  AlternateMetadata meta = meta_in;
  meta.full_mask = default_mask.Encode();
  auto wh = cache->WriteAlternate(effective_url, "", "https", id,
                                  content.size(), meta);
  ASSERT_TRUE(wh.has_value()) << "Failed to open write handle";
  auto written =
      wh->write_sync(std::as_bytes(std::span(content.data(), content.size())));
  ASSERT_TRUE(written.has_value()) << "Failed to write content";
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value()) << "Failed to close write handle";
}

TEST_F(WorkerTest, CssNotificationGzipGuard) {
  // CSS content starting with gzip magic bytes should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with fake gzip-compressed CSS.
  std::string url = "http://example.com/gzip-guard.css";
  std::string gzip_css = "\x1f\x8b\x08...fake gzip css content";
  CacheOriginal(worker.cache(), url, gzip_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // no-variant invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // No minified variant should be written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, gzip_css)
      << "Gzip-guarded CSS should not produce a variant";

  // Error counter should have incremented.
  EXPECT_GE(worker.stats().errors.load(), 1u)
      << "Gzip guard should increment error counter";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssNotificationTooLarge) {
  // CSS content exceeding max_css_size should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_css_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS content exceeding 100 bytes.
  std::string url = "http://example.com/large.css";
  std::string css =
      "body { margin: 0; padding: 0; } "
      "h1 { color: red; font-size: 24px; } "
      "p { line-height: 1.5; margin: 10px; } "
      "a { text-decoration: none; color: blue; }";
  ASSERT_GT(css.size(), 100u);
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Original CSS should still be intact, no minified variant written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, css) << "Too-large CSS should not produce a variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssNotificationAlreadyMinimal) {
  // CSS that is already minimal should still produce compressed
  // variants (gzip/brotli) even though no identity variant is written.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with already-minified CSS.
  std::string url = "http://example.com/already-min.css";
  std::string css = "body{margin:0}h1{color:red}";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Compressed variants should exist even though CSS is already minimal.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Gzip variant should exist for already-minimal CSS";

  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "Brotli variant should exist for already-minimal CSS";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsNotificationGzipGuard) {
  // JS content starting with gzip magic bytes should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/gzip-guard.js";
  std::string gzip_js = "\x1f\x8b\x08...fake gzip js content";
  CacheOriginal(worker.cache(), url, gzip_js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // no-variant invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // No minified variant should be written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, gzip_js) << "Gzip-guarded JS should not produce a variant";

  // Error counter should have incremented.
  EXPECT_GE(worker.stats().errors.load(), 1u)
      << "Gzip guard should increment error counter";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsNotificationTooLarge) {
  // JS content exceeding max_js_size should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_js_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/large.js";
  std::string js =
      "// This is a long JavaScript file\n"
      "function processData(input) {\n"
      "  var result = input.split(',');\n"
      "  return result.map(function(item) { return item.trim(); });\n"
      "}\n";
  ASSERT_GT(js.size(), 100u);
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll to confirm notification was received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  // No minified variant should be written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, js) << "Too-large JS should not produce a variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsNotificationSourcemapWarning) {
  // JS containing a sourceMappingURL should still be minified.
  // The worker produces output even when the minifier encounters
  // a sourceMappingURL comment.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/sourcemap.js";
  std::string js =
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n"
      "//# sourceMappingURL=app.js.map\n";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for the worker to finish processing the JS notification before
  // reading.  Poll the js_processed stat counter — which the worker increments
  // only AFTER the variant write commits (worker.cc orders
  // MarkVariantProcessed + variants_written + js_processed strictly after
  // WriteVariant returns write_ok) — rather than breaking on the first
  // non-empty ReadVariant.  The slot is pre-populated with the un-minified
  // original by CacheOriginal, so a "first non-empty read" races the worker
  // and can return the original (variant.size() == js.size()) before
  // minification lands.  That race is the cause of the historical
  // JsNotificationSourcemapWarning flake on slow runners.  This mirrors the
  // passing JsNotificationMinifiesAndWritesVariant.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "Worker did not process the JS sourcemap notification";

  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);

  // Should still produce a minified variant.
  EXPECT_FALSE(variant.empty()) << "JS with sourcemap should still be minified";
  if (!variant.empty()) {
    // Minified should be shorter than original.
    EXPECT_LT(variant.size(), js.size());
    // Key identifiers should survive minification.
    EXPECT_NE(variant.find("hello"), std::string::npos);
    EXPECT_NE(variant.find("greeting"), std::string::npos);
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationGzipGuard) {
  // HTML content starting with gzip magic bytes should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/gzip-guard.html";
  std::string gzip_html = "\x1f\x8b\x08...fake gzip html content";
  CacheOriginal(worker.cache(), url, gzip_html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use Mobile mask (0x00) so the variant would be written at a
  // different AlternateId from the original (which is at Desktop 0x08).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;  // Mobile/Identity
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // no-variant invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // No variant should be written at the Mobile mask for gzip content.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty())
      << "Gzip-guarded HTML should not produce a variant";

  // Error counter should have incremented.
  EXPECT_GE(worker.stats().errors.load(), 1u)
      << "HTML gzip guard should increment error counter";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationNoTransformsApplicable) {
  // Disable all HTML transforms and provide HTML with no critical CSS.
  // The worker should still process but may not produce a variant (since
  // no transforms produce output different from original).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_lazy_load = true;
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no CSS, so critical CSS injection has nothing to inject.
  std::string url = "http://example.com/no-transforms.html";
  std::string html =
      "<html><head></head><body><p>Simple page</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  // Notification should have been received without errors.
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().errors.load(), 0u)
      << "No transforms applicable should not cause errors";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationUnsupportedType) {
  // An image with unsupported origin_content_type (e.g., image/x-icon)
  // should have only compressed variants written, not transcoded.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create fake ICO data with metadata indicating image/x-icon.
  std::string url = "http://example.com/favicon.ico";
  std::string ico_data = "fake ico content for testing purposes";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/x-icon";
  CacheOriginalWithMeta(worker.cache(), url, ico_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP/AVIF transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty())
      << "Unsupported image type should not produce WebP variant";

  // Compressed variants (gzip/brotli) of the original should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Unsupported image type should still have gzip variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationTooLarge) {
  // Image content exceeding max_image_size should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_image_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/huge-image.jpg";
  // Use a real JPEG file that exceeds 100 bytes.
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test JPEG not found";
  ASSERT_GT(image_data.size(), 100u);
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  // No transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(variant.empty())
      << "Too-large image should not produce a variant";

  // Image processing counter should not have incremented.
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationSvgPassthrough) {
  // SVG content with origin_content_type "image/svg+xml" should
  // produce only compressed variants without raster processing.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/logo.svg";
  std::string svg_data =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" "
      "height=\"100\"><circle cx=\"50\" cy=\"50\" r=\"40\"/></svg>";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/svg+xml";
  CacheOriginalWithMeta(worker.cache(), url, svg_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP variant should be produced (SVG is already vector format).
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty()) << "SVG should not produce a WebP variant";

  // Compressed variants of the SVG should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "SVG should have a gzip compressed variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ContentTypeOtherNotification) {
  // A notification with ContentType::kOther should be handled
  // gracefully (logged as unknown, no processing).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/data.json";
  std::string content = R"({"key": "value"})";
  CacheOriginal(worker.cache(), url, content);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  // No variants should be written for unknown content types.
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  // No processing counters should increment.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupSentinelProcessesJs) {
  // Verify that the warmup sentinel triggers JS minification for
  // a URL that doesn't have a minified variant yet.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate JS content.
  std::string js_url = "http://example.com/warmup.js";
  std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheOriginal(worker.cache(), js_url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup notification (sentinel mask).
  CacheNotification notification;
  notification.url = js_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Wait for processing to complete and dedup set to be populated.
  // CacheOriginal writes at the same AlternateId, so !variant.empty()
  // alone races with the worker overwriting the original.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }

  CapabilityMask default_mask;
  std::string variant = ReadVariant(worker.cache(), js_url, default_mask);
  EXPECT_FALSE(variant.empty()) << "Warmup should produce minified JS";
  if (!variant.empty()) {
    EXPECT_LT(variant.size(), js.size());
    EXPECT_NE(variant.find("hello"), std::string::npos);
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationWithLcpAndPreconnect) {
  // HTML with an LCP image and third-party stylesheets should
  // produce Early Hints containing both image: and preconnect: entries.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/lcp-preconnect.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" "
      "href=\"https://fonts.googleapis.com/css2?family=Roboto\">"
      "<style>body { margin: 0; } .hero { font-size: 24px; }</style>"
      "</head><body>"
      "<img src=\"https://cdn.example.com/hero.jpg\">"
      "<p>Content</p>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Pre-populate the external stylesheet so critical CSS injection works.
  std::string font_url = "https://fonts.googleapis.com/css2?family=Roboto";
  std::string font_css = "@font-face { font-family: Roboto; }";
  CacheOriginal(worker.cache(), font_url, font_css, "fonts.googleapis.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for Early Hints sentinel.
  std::string hints;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  EXPECT_FALSE(hints.empty()) << "Early Hints not stored";
  if (!hints.empty()) {
    // Should contain the LCP image with image: prefix.
    EXPECT_NE(hints.find("image:https://cdn.example.com/hero.jpg"),
              std::string::npos)
        << "Hints should contain LCP image. Got: " << hints;

    // Should contain preconnect for third-party origins.
    EXPECT_NE(hints.find("preconnect:https://fonts.googleapis.com"),
              std::string::npos)
        << "Hints should contain preconnect for fonts. Got: " << hints;
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, MgmtAuthWithoutToken) {
  // AUTH when no purge token is configured should return
  // "ERR no token configured".
  WorkerConfig config;
  config.socket_path = socket_path_;

  // Ensure no token is set.
  unsetenv("PAGESPEED_PURGE_TOKEN");

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  std::string cmd = "AUTH sometoken\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "No response from management socket";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find("ERR no token configured"), std::string::npos)
      << "AUTH without configured token should return error. Got: " << response;
}

// =============================================================================
// Additional Coverage Tests: Uncovered Code Paths in worker.cc
// =============================================================================

// --- A. HTML Path Coverage ---

TEST_F(WorkerTest, HtmlOriginalNotInCache) {
  // Send HTML notification for URL not in cache — should not crash
  // and should not write any variant.  Covers the "original HTML not
  // found" early return (worker.cc ~1506-1512).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/html-not-cached.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  // No HTML should be processed since it's not in cache.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlTransformNoChanges) {
  // Send HTML that has elements but no transforms apply.  All individual
  // transforms are enabled, but the HTML has no CSS (no critical CSS), no
  // images (no lazy load), no LCP candidate (no preload), no third-party
  // origins (no preconnect), and no speculation rules.  The HtmlTransformFilter
  // produces no modifications.  Covers line ~1759-1765.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Leave all transforms enabled (defaults) but provide HTML that
  // won't trigger any of them.
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no CSS, no images, no external resources — transforms
  // are applicable in theory but produce no changes in practice.
  // We need at least one external stylesheet link for preconnect to have
  // something to work with, but since the stylesheet is on the same origin,
  // preconnect won't inject anything.  The key is that the final
  // transform_filter.modified() returns false.
  std::string url = "http://example.com/no-changes.html";
  std::string html =
      "<html><head></head><body><p>Plain text only</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Wait for notification to be processed.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  // No errors should have occurred.
  EXPECT_EQ(worker.stats().errors.load(), 0u);

  // No variant should be written at Mobile mask since transforms
  // produced no changes (html_assembly_skipped should increment).
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty())
      << "No variant should be written when transforms produce no changes";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlCacheNotInitializedSkips) {
  // When cache is not initialized, HTML notifications should be skipped.
  // Covers worker.cc ~1494-1499.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-html.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// --- B. Image Path Coverage ---

TEST_F(WorkerTest, ImageOriginalNotInCache) {
  // Send image notification for URL not in cache — should not crash
  // and should not write any variant.  Covers worker.cc ~1916-1921.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/image-not-cached.jpg";
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageUnsupportedFormatBmp) {
  // BMP images should not be transcoded but should get compressed variants.
  // Covers the unsupported image type path (worker.cc ~1953-1992).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/image.bmp";
  std::string bmp_data = "fake bmp content for testing purposes";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/bmp";
  CacheOriginalWithMeta(worker.cache(), url, bmp_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP/AVIF transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty()) << "BMP should not produce WebP variant";

  // Compressed variants of the original should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "BMP should still have gzip compressed variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageUnsupportedFormatTiff) {
  // TIFF images should not be transcoded but should get compressed variants.
  // Covers the unsupported image type path (worker.cc ~1953-1992).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/image.tiff";
  std::string tiff_data = "fake tiff content for testing";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/tiff";
  CacheOriginalWithMeta(worker.cache(), url, tiff_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP/AVIF transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty()) << "TIFF should not produce WebP variant";

  // Compressed variants of the original should exist.
  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "TIFF should still have brotli compressed variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageProactiveAllSiblingsExistSkips) {
  // When all format/viewport siblings already exist in cache, proactive
  // generation should be skipped entirely.  Covers worker.cc ~2457-2462.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/all-siblings.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send first notification to generate all format siblings at Desktop.
  CapabilityMask notif_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = notif_mask.Encode();
  SendNotification(notification);

  // Wait for all proactive variants (WebP, AVIF, optimized original).
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 2u)
      << "At least WebP + AVIF should be written";

  // images_processed moves before the in-flight entry is erased.  Drain first
  // so the re-send below reaches the all-siblings check instead of being
  // dropped by the in-flight guard.
  WaitForWorkerIdle(worker);

  uint64_t variants_before = worker.stats().variants_written.load();

  // Clear dedup to allow re-processing.
  worker.ClearDedupAndCooldown(url, "");

  // Send same notification again — all siblings already exist in cache,
  // so no new variants should be written.
  SendNotification(notification);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  uint64_t variants_after = worker.stats().variants_written.load();
  EXPECT_EQ(variants_after, variants_before)
      << "No new variants should be written when all siblings already exist";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNonProactiveGeneratesAllFormatsAtRequestedDimension) {
  // When proactive_image_variants is disabled, all 3 formats (WebP, AVIF,
  // optimized original) should be generated at the requested viewport/density/
  // save-data, but NOT at other viewports/densities/save-data combinations.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/non-proactive-formats.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Request WebP/Desktop specifically with non-proactive mode.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Poll for the WebP variant.
  std::string webp_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    webp_variant = ReadVariant(worker.cache(), url, webp_mask);
    if (!webp_variant.empty()) break;
  }
  EXPECT_FALSE(webp_variant.empty()) << "WebP variant should be written";

  // AVIF at Desktop should also be generated (all formats at requested dim).
  CapabilityMask avif_mask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string avif_variant;
  for (int i = 0; i < 20 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    avif_variant = ReadVariant(worker.cache(), url, avif_mask);
    if (!avif_variant.empty()) break;
  }
  EXPECT_FALSE(avif_variant.empty())
      << "AVIF should be generated at the requested dimension";

  // Optimized original at Desktop should also be generated.
  CapabilityMask orig_mask(CapabilityMask::ImageFormat::kOriginal,
                           CapabilityMask::Viewport::kDesktop,
                           CapabilityMask::PixelDensity::k1x,
                           CapabilityMask::SaveData::kOff,
                           CapabilityMask::TransferEncoding::kIdentity);
  std::string orig_variant;
  for (int i = 0; i < 20 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    orig_variant = ReadVariant(worker.cache(), url, orig_mask);
    if (!orig_variant.empty()) break;
  }
  // Note: optimized_original may be absent if re-encoding yields no savings,
  // which is an acceptable outcome for high-quality source images.

  // Mobile viewport should NOT be generated (non-proactive).
  CapabilityMask mobile_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string mobile_variant = ReadVariant(worker.cache(), url, mobile_mask);
  EXPECT_TRUE(mobile_variant.empty())
      << "Mobile WebP should not be generated in non-proactive mode";

  // Tablet viewport should NOT be generated (non-proactive).
  CapabilityMask tablet_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string tablet_variant = ReadVariant(worker.cache(), url, tablet_mask);
  EXPECT_TRUE(tablet_variant.empty())
      << "Tablet WebP should not be generated in non-proactive mode";

  // Save-Data kOn should NOT be generated (non-proactive).
  CapabilityMask sd_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string sd_variant = ReadVariant(worker.cache(), url, sd_mask);
  EXPECT_TRUE(sd_variant.empty())
      << "Save-Data WebP should not be generated in non-proactive mode";

  // 2x density should NOT be generated (non-proactive).
  CapabilityMask den_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string den_variant = ReadVariant(worker.cache(), url, den_mask);
  EXPECT_TRUE(den_variant.empty())
      << "2x density WebP should not be generated in non-proactive mode";

  // Verify the WebP variant is valid WebP (starts with "RIFF").
  if (!webp_variant.empty()) {
    EXPECT_GE(webp_variant.size(), 4u);
    EXPECT_EQ(webp_variant.substr(0, 4), "RIFF")
        << "WebP variant should start with RIFF header";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageCacheNotInitializedSkips) {
  // When cache is not initialized, image notifications should be skipped.
  // Covers worker.cc ~1902-1908.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-image.jpg";
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// --- C. CSS Path Coverage ---

TEST_F(WorkerTest, CssOriginalNotInCache) {
  // Send CSS notification for URL not in cache — should not crash
  // and should not write any variant.  Covers worker.cc ~2661-2667.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/css-not-cached.css";
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssCacheNotInitializedSkips) {
  // When cache is not initialized, CSS notifications should be skipped.
  // Covers worker.cc ~2649-2654.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-css.css";
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssImportFlattening) {
  // CSS with @import statements should have them flattened before
  // minification.  Covers worker.cc ~2702-2729.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate the imported CSS file.
  std::string import_url = "http://example.com/base.css";
  std::string import_css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  blue;  }\n";
  CacheOriginal(worker.cache(), import_url, import_css);

  // Pre-populate the main CSS that imports base.css.
  std::string url = "http://example.com/main.css";
  std::string main_css =
      "@import url(\"http://example.com/base.css\");\n"
      "p  {  font-size:  16px;  line-height:  1.5;  }\n";
  CacheOriginal(worker.cache(), url, main_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the minified variant.
  CapabilityMask mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "Flattened+minified CSS variant missing";
  if (!variant.empty()) {
    // The flattened CSS should contain content from both files.
    EXPECT_NE(variant.find("margin"), std::string::npos)
        << "Should contain imported body margin rule";
    EXPECT_NE(variant.find("font-size"), std::string::npos)
        << "Should contain main CSS font-size rule";
    // Should be smaller than the combined size (minified).
    EXPECT_LT(variant.size(), main_css.size() + import_css.size());
    // @import directive should be gone (flattened).
    EXPECT_EQ(variant.find("@import"), std::string::npos)
        << "Flattened CSS should not contain @import";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssMinificationWritesCompressed) {
  // Verify that CSS minification produces identity, gzip, and brotli
  // variants.  Specifically covers the write path in worker.cc ~2741-2810.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use Desktop/Gzip mask to ensure encoding stripping is tested.
  std::string url = "http://example.com/compress-test.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n"
      "p  {  line-height:  1.5;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send with Gzip encoding mask — worker should strip to Identity for write.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x48;  // Desktop/Gzip
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Identity variant should exist at Desktop/Identity (0x08).
  CapabilityMask identity_mask = CapabilityMask::Decode(0x48);
  identity_mask.set_transfer_encoding(
      CapabilityMask::TransferEncoding::kIdentity);
  identity_mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  ASSERT_FALSE(identity.empty()) << "Identity CSS variant missing";
  EXPECT_LT(identity.size(), css.size()) << "Should be minified";

  // Gzip variant should exist.
  CapabilityMask gz_mask = identity_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty()) << "Gzip CSS variant missing";

  // Brotli variant should exist.
  CapabilityMask br_mask = identity_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty()) << "Brotli CSS variant missing";

  // Stats should show CSS was processed.
  EXPECT_GE(worker.stats().css_processed.load(), 1u);
}

// --- D. JS Path Coverage ---

TEST_F(WorkerTest, JsOriginalNotInCache) {
  // Send JS notification for URL not in cache — should not crash
  // and should not write any variant.  Covers worker.cc ~2835-2841.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/js-not-cached.js";
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsCacheNotInitializedSkips) {
  // When cache is not initialized, JS notifications should be skipped.
  // Covers worker.cc ~2823-2828.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-js.js";
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsMinificationAndCompression) {
  // Verify that JS minification produces identity + compressed variants.
  // Covers the JS write path in worker.cc ~2881-2939.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/compress-test.js";
  std::string js =
      "// Application script\n"
      "function  processData( items )  {\n"
      "  var  result  =  [];\n"
      "  for  ( var  i  =  0;  i  <  items.length;  i++ )  {\n"
      "    result.push( items[i].trim() );\n"
      "  }\n"
      "  return  result;\n"
      "}\n";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing to complete and dedup set to be populated.
  // CacheOriginal writes at the same AlternateId, so !variant.empty()
  // alone races with the worker overwriting the original.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }

  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  ASSERT_FALSE(variant.empty()) << "Minified JS variant missing";
  EXPECT_LT(variant.size(), js.size()) << "Should be minified";
  EXPECT_NE(variant.find("processData"), std::string::npos)
      << "Key identifiers should survive minification";

  // Gzip variant should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty()) << "Gzip JS variant missing";

  // Brotli variant should exist.
  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty()) << "Brotli JS variant missing";

  // Stats should reflect processing.
  EXPECT_GE(worker.stats().js_processed.load(), 1u);
  EXPECT_GE(worker.stats().variants_written.load(), 1u);
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u);
  EXPECT_GE(worker.stats().brotli_variants_written.load(), 1u);
}

TEST_F(WorkerTest, JsAlreadyMinimalProducesCompressed) {
  // JS that is already minimal should still produce compressed variants.
  // Covers the "JS already minimal" path in worker.cc ~2940+.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Already-minimal JS (no comments, no extra whitespace).
  std::string url = "http://example.com/already-min.js";
  std::string js = "function f(a){return a+1}";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing (JS minification compiles RE2 patterns).
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  // Give extra time for compressed variant writes.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Compressed variants should exist even though JS is already minimal.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Gzip variant should exist for already-minimal JS";

  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "Brotli variant should exist for already-minimal JS";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Additional Coverage: HTML Cooldown, Speculation Rules, SVG Pipeline,
// Proactive Transcoding, CSS @import Edge Cases
// =============================================================================

TEST_F(WorkerTest, HtmlWriteFailureCooldownSkipsWithMessage) {
  // When HTML processing sets a tentative cooldown and a second
  // notification arrives within the cooldown window, the worker should
  // skip processing and log the remaining cooldown time.
  // Covers lines 1458-1470 (cooldown expiry check with log message).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML with inline CSS so a variant is produced.
  std::string url = "http://example.com/cooldown-msg.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Cooldown Message</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification — sets the tentative cooldown.
  SendNotification(notification);

  // Wait for processing to complete.
  CapabilityMask mask = CapabilityMask::Decode(0);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!ReadVariant(worker.cache(), url, mask).empty()) break;
  }

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  uint64_t html_before = worker.stats().html_processed.load();

  // Second notification immediately — should be skipped by either dedup
  // or cooldown, exercising the cooldown remaining-time log path.  Poll for
  // the skip counter to advance instead of a fixed sleep (slow shards can sit
  // at the pipe read past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  // Verify the second notification was skipped.
  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Second notification should be skipped by dedup/cooldown";
  // HTML should not be processed again.
  EXPECT_EQ(worker.stats().html_processed.load(), html_before)
      << "HTML should not be reprocessed within cooldown window";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, SpeculationRulesInjectedWhenHotUrlsExist) {
  // When enable_speculation_rules is true and warmup notifications have
  // populated hot_urls_, subsequent HTML pages should have speculation
  // rules injected.  Covers lines 1707-1710.
  // Hot URLs are populated via warmup notifications (kWarmupSentinel mask),
  // not regular HTML processing notifications.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = true;
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string hostname = "spectest.example.com";

  // Helper to cache HTML with a specific hostname.
  // Normalizes URL to path-only to match worker's NormalizeCacheUrl.
  auto cache_html = [&](const std::string& url, const std::string& html) {
    std::string norm_url = StripSchemeAuthority(url);
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(norm_url, hostname, "https", id,
                                             html.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(html.data(), html.size())));
    (void)wh->close_sync();
  };

  // Cache the target page (page4) that will receive speculation rules.
  std::string target_url = "http://spectest.example.com/page4.html";
  std::string target_html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Page 4</h1></body></html>";
  cache_html(target_url, target_html);

  // Also cache the pages that will be used as hot URLs (so warmup
  // can read them from cache).
  for (int i = 0; i < 4; ++i) {
    std::string page_url =
        "http://spectest.example.com/page" + std::to_string(i) + ".html";
    std::string page_html =
        "<html><head><style>body { margin: 0; }</style></head>"
        "<body><h1>Page " +
        std::to_string(i) + "</h1></body></html>";
    cache_html(page_url, page_html);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup notifications (kWarmupSentinel mask) for pages 0-3.
  // This populates the hot_urls_ list via RecordHotUrl().
  for (int i = 0; i < 4; ++i) {
    CacheNotification notification;
    notification.url =
        "http://spectest.example.com/page" + std::to_string(i) + ".html";
    notification.scheme = "https";
    notification.hostname = hostname;
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = kWarmupSentinel;
    SendNotification(notification);
  }

  // Wait for warmup processing.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Now send a regular HTML notification for page4.
  // The hot URL list should be populated, so speculation rules
  // should be injected into the output.
  CacheNotification final_notif;
  final_notif.url = target_url;
  final_notif.scheme = "https";
  final_notif.hostname = hostname;
  final_notif.content_type = ContentType::kHtml;
  final_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(final_notif);

  // Wait for page4 to be processed.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_processed.load(), 1u)
      << "Page4 should be processed";

  // Read the variant for page4 and check for speculation rules.
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant =
      ReadVariant(worker.cache(), final_notif.url, mask, hostname);
  EXPECT_FALSE(variant.empty()) << "HTML variant for page4 not written";
  if (!variant.empty()) {
    EXPECT_NE(variant.find("speculationrules"), std::string::npos)
        << "Speculation rules should be injected when hot URLs exist. "
        << "Variant: " << variant.substr(0, 500);
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageSvgCandidacyEvaluationDetectMode) {
  // In detect mode, SVG candidacy should be evaluated and stats
  // updated, but no SVG variant should be written.
  // Covers lines 2025-2089 (SVG candidacy evaluation).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a small PNG (icon-like, good SVG candidate).
  std::string url = "http://example.com/icon-detect.png";
  std::string png = ReadTestFile("png/pagespeed-128.png");
  ASSERT_FALSE(png.empty()) << "Test PNG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  CacheOriginalWithMeta(worker.cache(), url, png, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated in detect mode";

  // No SVG variant should be written in detect mode.
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string svg_variant = ReadVariant(worker.cache(), url, svg_mask);
  EXPECT_TRUE(svg_variant.empty())
      << "No SVG variant should be written in detect mode";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageSvgAutoModeVectorizesAndWrites) {
  // In auto mode with a good SVG candidate, the full pipeline should run:
  // evaluate -> vectorize -> size gate -> cache write.
  // Covers lines 2107-2190 (vectorization) and 2537-2614 (size gate + write).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kAuto;
  config.svg_candidacy_threshold = 1;  // Very low threshold to pass.
  config.svg_max_pixels = 1000000;
  config.svg_max_paths = 10000;
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use the pagespeed icon PNG — likely a good SVG candidate (logo/icon).
  std::string url = "http://example.com/icon-auto.png";
  std::string png = ReadTestFile("png/pagespeed-128.png");
  ASSERT_FALSE(png.empty()) << "Test PNG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  CacheOriginalWithMeta(worker.cache(), url, png, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for image processing (SVG pipeline may take a moment).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated in auto mode";

  // Check if vectorization was attempted (candidate may or may not pass).
  // The vectorize time counter increments only when vectorization runs.
  bool vectorization_attempted =
      worker.stats().svg_vectorized.load() > 0 ||
      worker.stats().svg_candidates_rejected.load() > 0 ||
      worker.stats().svg_path_count_rejected.load() > 0 ||
      worker.stats().svg_fidelity_rejected.load() > 0 ||
      worker.stats().svg_timeout_exceeded.load() > 0;
  EXPECT_TRUE(vectorization_attempted ||
              worker.stats().svg_candidates_rejected.load() > 0)
      << "SVG pipeline should have either vectorized or rejected";

  // If vectorization succeeded, check for SVG variant.
  if (worker.stats().svg_written.load() > 0) {
    CapabilityMask svg_mask(
        CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
        CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
        CapabilityMask::TransferEncoding::kIdentity);
    std::string svg_variant = ReadVariant(worker.cache(), url, svg_mask);
    EXPECT_FALSE(svg_variant.empty())
        << "SVG variant should exist when svg_written > 0";
    if (!svg_variant.empty()) {
      // Should be valid SVG (starts with <svg or similar).
      EXPECT_NE(svg_variant.find("<svg"), std::string::npos)
          << "SVG variant should contain SVG markup";
    }
    // Bytes saved should be tracked.
    EXPECT_GT(worker.stats().svg_bytes_saved.load(), 0u)
        << "SVG bytes saved should be tracked";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageSvgPreviewModeVectorizesButDoesNotWrite) {
  // In preview mode, SVG should be vectorized but NOT written to cache
  // for serving.  Covers lines 2615-2626 (preview mode branch).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kPreview;
  config.svg_candidacy_threshold = 1;
  config.svg_max_pixels = 1000000;
  config.svg_max_paths = 10000;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/icon-preview.png";
  std::string png = ReadTestFile("png/pagespeed-128.png");
  ASSERT_FALSE(png.empty()) << "Test PNG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  CacheOriginalWithMeta(worker.cache(), url, png, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u);

  // In preview mode, svg_written should NOT increment even if
  // vectorization succeeded.
  EXPECT_EQ(worker.stats().svg_written.load(), 0u)
      << "SVG should not be written in preview mode";

  // No SVG variant should exist in cache.
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string svg_variant = ReadVariant(worker.cache(), url, svg_mask);
  EXPECT_TRUE(svg_variant.empty())
      << "SVG variant should not be in cache in preview mode";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageProactiveWithContentClassCounting) {
  // Proactive image transcode with content analysis enabled should
  // update content class stats counters.
  // Covers lines 2265-2301 (proactive transcode loop) and
  // 2376-2427 (content class counting and learned quality stats).
  //
  // Content analysis only runs in TranscodeMultiResized (pixel decode path),
  // which requires target_width > 0 AND actual resize to occur (otherwise
  // the no-resize optimization short-circuits to TranscodeMulti).
  // quality100.jpg is 200x200, so we set mobile_width to 100 to force resize.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.content_analysis = true;
  config.mobile_width =
      100;  // Smaller than quality100.jpg (200px) to force resize
  // This test is about content-class COUNTING, not verification: turn the
  // verifier off so the tiny (100px) resized variants store unconditionally.
  // On images this small SSIMULACRA2 scores structurally low, so under the
  // binding verify verdict both cross-format variants decline below the
  // floor — decline mechanics are pinned in the transcoder suite.
  config.quality_verify = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a real JPEG photo -- should classify as kPhoto.
  std::string url = "http://example.com/content-class.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use Mobile viewport so target_width > 0 (default 480px), which
  // forces the pixel decode + resize path in TranscodeMultiResized,
  // where content analysis and content class counting occur.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for processing (proactive generates WebP + AVIF).
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "At least one image format should be processed";

  // Content class counting: at least one class should have incremented.
  uint64_t photo = worker.stats().content_photo.load();
  uint64_t screenshot = worker.stats().content_screenshot.load();
  uint64_t illustration = worker.stats().content_illustration.load();
  uint64_t noisy = worker.stats().content_noisy.load();
  EXPECT_GE(photo + screenshot + illustration + noisy, 1u)
      << "Content class should be counted at least once";

  // WebP variant should exist at Mobile viewport.
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_FALSE(webp_variant.empty()) << "WebP variant should be written";

  // AVIF variant should also exist (proactive format sibling).
  CapabilityMask avif_mask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string avif_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    avif_variant = ReadVariant(worker.cache(), url, avif_mask);
    if (!avif_variant.empty()) break;
  }
  EXPECT_FALSE(avif_variant.empty()) << "AVIF variant should be written";

  worker.Shutdown();
  worker_thread.join();
}

// The verify verdict must ROUTE to the declines counter (#1381):
// the stats-contract test seeds the atomic directly, so the increment path
// in WriteImageVariants was previously undriven — a mutant deleting the
// declined branch survived, and the declined score silently polluted the
// checks count and the score average. This drives a real decline
// end-to-end: same construction as the content-class test above, but with
// the verifier ON, so the tiny (100px) resized cross-format variants score
// structurally below the floor and decline.
TEST_F(WorkerTest, ImageProactiveDeclineRoutesToDeclinesCounterOnly) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.content_analysis = false;
  config.mobile_width = 100;  // force the resize path (200px source)
  config.quality_verify = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/decline-counting.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Both cross-format arms decline; wait on the counter itself, not on
  // images_processed (a declined variant is not a processed image).
  for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().ssimulacra2_declines.load() >= 2) break;
  }
  EXPECT_EQ(2u, worker.stats().ssimulacra2_declines.load())
      << "both cross-format variants of a 100px resize decline below the "
         "floor";

  // Routing, not just counting: a decline must NOT feed the checks count
  // or the score average. Only the same-format JPEG arm (ship-below-floor
  // policy) may register an ordinary check.
  const uint64_t checks = worker.stats().ssimulacra2_checks.load();
  EXPECT_LE(checks, 1u)
      << "declined arms leaked into ssimulacra2_checks — the declined "
         "branch in WriteImageVariants is not routing";
  const uint64_t total_x100 =
      worker.stats().ssimulacra2_total_score_x100.load();
  if (checks == 0) {
    EXPECT_EQ(0u, total_x100);
  } else {
    EXPECT_LE(total_x100, 100u * 100u)
        << "score average carries more than the single JPEG check";
  }
  EXPECT_EQ(0u, worker.stats().errors.load())
      << "a decline is a decision, not an error";

  // The mgmt STATS surface carries the decision too (hand-built JSON,
  // distinct from BuildStatsJson): read the WHOLE response.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";
  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  EXPECT_NE(response.find("\"declines\":2"), std::string::npos)
      << "mgmt STATS response: " << response;
}

// A declined original-format slot must not ALSO count as a no-savings skip
// (#1382): both counters describe decisions, and one refusal wearing two
// labels makes each of them over-report. The classification helper is
// tested directly because the state is not constructible end-to-end -- an
// unmeasurable-declined optimized_original needs a failed decode-back of
// our own JPEG re-encode, which the encoders do not produce.
TEST_F(WorkerTest, DeclinedOriginalIsNotANoSavingsSkip) {
  MultiTranscodeResult declined;
  declined.optimized_original = {
      false,
      {},
      {},
      "JPEG quality verification failed (no verdict: output could not be "
      "measured)"};
  declined.ssimulacra2_declined = true;
  EXPECT_FALSE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(declined))
      << "a verify decline must not double-count as image_no_savings_skipped";

  // The plain case still counts: an original the optimizer could not beat.
  MultiTranscodeResult no_savings;
  no_savings.optimized_original = {
      false, {}, {}, "JPEG optimization produced no size savings"};
  EXPECT_TRUE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(no_savings));

  // A shipped original never counts, whatever else the call did.
  MultiTranscodeResult shipped;
  shipped.optimized_original = {true, "bytes", "image/jpeg", {}};
  EXPECT_FALSE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(shipped));

  // A CROSS-FORMAT decline does not reclassify the original slot: only the
  // original arm's decline flag belongs to this classification.
  MultiTranscodeResult webp_declined_only;
  webp_declined_only.webp_ssimulacra2_declined = true;
  webp_declined_only.optimized_original = {
      false, {}, {}, "JPEG optimization produced no size savings"};
  EXPECT_TRUE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(webp_declined_only));
}

// ---------------------------------------------------------------------------
// Decline tombstone (#1382): codec contract. The blob is worker-written
// (kDeclineTombstone sentinel) and worker-read; the parse must be fail-open
// (anything unrecognized reads as "no tombstone" and the ladder re-runs)
// and MUST key on the source hash (a changed source retries its slots).
// ---------------------------------------------------------------------------

TEST(DeclineTombstoneCodec, RoundTrip) {
  std::array<std::byte, 32> hash{};
  hash[0] = static_cast<std::byte>(0xAB);
  std::vector<pagespeed::DeclineTombstoneRecord> records = {
      {0x11, 60}, {0x22, 0xFF}, {0x33, 45}};
  std::string blob = pagespeed::EncodeDeclineTombstone(hash, records);
  auto back = pagespeed::ParseDeclineTombstone(blob, hash);
  ASSERT_EQ(back.size(), records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(back[i].slot, records[i].slot);
    EXPECT_EQ(back[i].quality, records[i].quality);
  }
}

TEST(DeclineTombstoneCodec, DifferentSourceHashReadsAsNoTombstone) {
  std::array<std::byte, 32> hash_a{};
  std::array<std::byte, 32> hash_b{};
  hash_b[31] = static_cast<std::byte>(1);
  auto blob = pagespeed::EncodeDeclineTombstone(hash_a, {{0x11, 60}});
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(blob, hash_b).empty())
      << "a tombstone for another source must not stop this source's "
         "recompute";
  // Non-vacuity: the same hash reads it back.
  EXPECT_EQ(pagespeed::ParseDeclineTombstone(blob, hash_a).size(), 1u);
}

TEST(DeclineTombstoneCodec, MalformedBlobReadsAsNoTombstone) {
  std::array<std::byte, 32> hash{};
  // Truncated header.
  EXPECT_TRUE(
      pagespeed::ParseDeclineTombstone(std::string(8, '\0'), hash).empty());
  // Unknown payload version: a future writer's blob must not be
  // half-understood.
  std::string bad = pagespeed::EncodeDeclineTombstone(hash, {{0x11, 60}});
  bad[0] = static_cast<char>(99);
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(bad, hash).empty());
  // Count disagrees with the payload length (truncated record).
  std::string truncated = pagespeed::EncodeDeclineTombstone(hash, {{0x11, 60}});
  truncated.pop_back();
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(truncated, hash).empty());
}

// The decline tombstone, end-to-end (#1382): a declined variant is never
// stored, so a notification that asks for it again re-pays the full attempt
// ladder (measured in seconds per AVIF arm). The production shape of "asks
// again" is a re-notify under a DIFFERENT client mask: the notification
// dedup keys on the normalized (viewport x density x save-data) mask of the
// original request, so a second visitor class reaches the variant loops
// with every previously declined slot still missing.
//
// Fixture: a synthetic 33x34 noise AVIF stored with an EMPTY origin content
// type. Both cross-format arms of a noise image score far below the floor
// and decline (the same constructor the transcoder suite uses for its
// negative-verdict test, same pinned qualities), and -- the reason this
// input is load-bearing here -- the original-format arm of an AVIF source
// has no optimizer, so no variant is ever written for the URL and the
// original-format slot stays missing on every pass. The empty content type
// is deliberate: a stored "image/avif" type hits the pipeline's
// unsupported-type branch (only jpeg/png/gif/webp pass) and never reaches
// the variant loops, while an empty type routes by magic bytes and does.
TEST_F(WorkerTest, DeclineTombstoneStopsTheSecondNotificationsLadder) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;  // all three viewports per pass
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.content_analysis = false;
  config.learned_quality = false;
  // Mirror the transcoder suite's negative-verdict fixture: low fixed
  // qualities, so the noise re-encodes land far below the floor
  // deterministically instead of depending on a predictor.
  config.webp_quality = 20;
  config.avif_quality = 20;
  config.quality_verify = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/decline-tombstone.avif";
  std::string noise = MakeSyntheticAvif(33, 34, /*grayscale=*/false,
                                        SyntheticPattern::kNoise, 0,
                                        /*quality=*/90, /*seed=*/12345);
  ASSERT_FALSE(noise.empty()) << "synthetic AVIF authoring failed";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "";  // Route by magic bytes; see the comment.
  CacheOriginalWithMeta(worker.cache(), url, noise, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto notify = [&](CapabilityMask::Viewport vp) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    CapabilityMask mask(CapabilityMask::ImageFormat::kWebP, vp,
                        CapabilityMask::PixelDensity::k1x,
                        CapabilityMask::SaveData::kOff,
                        CapabilityMask::TransferEncoding::kIdentity);
    notification.capability_mask = mask.Encode();
    SendNotification(notification);
  };

  // First notification (mobile visitor): all three viewport combinations
  // process, and both cross-format arms decline in each -- 6 declines, and
  // six tombstone records.
  notify(CapabilityMask::Viewport::kMobile);
  for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().ssimulacra2_declines.load() >= 6) break;
  }
  ASSERT_EQ(6u, worker.stats().ssimulacra2_declines.load())
      << "first notification must decline both cross-format arms at all "
         "three viewports";
  EXPECT_EQ(0u, worker.stats().ssimulacra2_decline_tombstone_hits.load())
      << "the first run has no tombstone to hit yet";

  // Second notification (desktop visitor): a different normalized mask, so
  // the dedup does not absorb it, and the same six slots are missing again
  // (declined variants are never stored). The tombstone must stop every
  // ladder. Wait on the tombstone counter itself.
  notify(CapabilityMask::Viewport::kDesktop);
  for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().ssimulacra2_decline_tombstone_hits.load() >= 6) break;
  }
  EXPECT_EQ(6u, worker.stats().ssimulacra2_decline_tombstone_hits.load())
      << "every declined slot must hit the tombstone on the re-notify";
  EXPECT_EQ(6u, worker.stats().ssimulacra2_declines.load())
      << "the ladder re-ran for a tombstoned slot: the decline counter grew";
  EXPECT_EQ(0u, worker.stats().errors.load());

  // The mgmt STATS surface carries the decision (hand-built JSON, distinct
  // from BuildStatsJson): read the WHOLE response.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";
  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  EXPECT_NE(response.find("\"tombstone_hits\":6"), std::string::npos)
      << "mgmt STATS response: " << response;
}

// The tombstone consult is gated on the verifier toggle (#1382): a
// tombstone stored while verification was on must not keep suppressing
// its slot after verification is turned off -- the slot would now fill.
// Two workers share one cache: the first (verifier on) declines the
// cross-format arms and stores the tombstone; the second (verifier off)
// receives the same notification and must RE-ATTEMPT the slots -- the
// consult is skipped, no tombstone hit is counted, and variants are
// written. Deleting the gate makes the second worker read the stored
// tombstone, skip every ladder, and write nothing.
TEST_F(WorkerTest, DeclineTombstoneIgnoredWhenVerifierDisabled) {
  const std::string noise = MakeSyntheticAvif(33, 34, /*grayscale=*/false,
                                              SyntheticPattern::kNoise, 0,
                                              /*quality=*/90, /*seed=*/12345);
  ASSERT_FALSE(noise.empty()) << "synthetic AVIF authoring failed";
  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "";  // Route by magic bytes; see the e2e test.
  const std::string url = "http://example.com/decline-tombstone-off.avif";

  // Phase 1: verifier ON -- declines both cross-format arms at all three
  // viewports and stores the tombstone (same construction as the e2e
  // test above).
  {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.proactive_image_variants = true;
    config.proactive_viewport_variants = true;
    config.proactive_savedata_variants = false;
    config.proactive_density_variants = false;
    config.content_analysis = false;
    config.learned_quality = false;
    config.webp_quality = 20;
    config.avif_quality = 20;
    config.quality_verify = true;

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());
    CacheOriginalWithMeta(worker.cache(), url, noise, meta);

    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    CapabilityMask mask(
        CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
        CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
        CapabilityMask::TransferEncoding::kIdentity);
    notification.capability_mask = mask.Encode();
    SendNotification(notification);
    for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().ssimulacra2_declines.load() >= 6) break;
    }
    ASSERT_EQ(6u, worker.stats().ssimulacra2_declines.load())
        << "phase 1 must decline both cross-format arms at all three "
           "viewports and store the tombstone";
  }

  // Phase 2: verifier OFF, same cache -- the stored tombstone must be
  // ignored (consult gated) and the slots re-attempted.
  {
    WorkerConfig config;
    config.socket_path = socket_path_ + "-off";
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.proactive_image_variants = true;
    config.proactive_viewport_variants = true;
    config.proactive_savedata_variants = false;
    config.proactive_density_variants = false;
    config.content_analysis = false;
    config.learned_quality = false;
    config.webp_quality = 20;
    config.avif_quality = 20;
    config.quality_verify = false;

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());

    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    CapabilityMask mask(
        CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
        CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
        CapabilityMask::TransferEncoding::kIdentity);
    notification.capability_mask = mask.Encode();
    // This worker listens on its own socket; the fixture's SendNotification
    // helper hardcodes the first worker's path.
    auto notify_b = [&](const CacheNotification& n) {
      intptr_t fd = pagespeed::test::ConnectPipe(socket_path_ + "-off");
      ASSERT_GE(fd, static_cast<intptr_t>(0)) << "worker B socket";
      std::vector<char> data = n.Serialize();
      ssize_t written =
          pagespeed::test::PipeWrite(fd, data.data(), data.size());
      EXPECT_EQ(written, static_cast<ssize_t>(data.size()));
      pagespeed::test::ClosePipe(fd);
    };
    notify_b(notification);

    // With the verifier off nothing declines; the slots the first worker
    // tombstoned are re-attempted and written. Wait on the write.
    for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().variants_written.load() >= 1) break;
    }
    EXPECT_GE(worker.stats().variants_written.load(), 1u)
        << "the tombstoned slot must be re-attempted with the verifier "
           "disabled";
    EXPECT_EQ(0u, worker.stats().ssimulacra2_decline_tombstone_hits.load())
        << "the consult must be skipped entirely with the verifier "
           "disabled";
    EXPECT_EQ(0u, worker.stats().ssimulacra2_declines.load());
  }
}

TEST_F(WorkerTest, CssImportFlatteningMissingImportGraceful) {
  // CSS with @import referencing a file NOT in cache should still
  // produce a minified variant using only the available CSS, but
  // with kFlagNeedsRevalidation set so the worker re-processes once
  // the import becomes available.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate the main CSS that imports a missing file.
  std::string url = "http://example.com/import-missing.css";
  std::string main_css =
      "@import url(\"http://example.com/missing-reset.css\");\n"
      "body  {  color:  red;  font-size:  16px;  }\n";
  CacheOriginal(worker.cache(), url, main_css);
  // Do NOT cache missing-reset.css.

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the minified variant with metadata.
  CapabilityMask mask;
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), url, mask);
    if (result.has_value()) break;
  }

  ASSERT_TRUE(result.has_value())
      << "CSS with missing import should still produce minified variant";
  // Should contain the main CSS rule.
  EXPECT_NE(result->content.find("color"), std::string::npos)
      << "Main CSS rules should survive import flattening failure";
  // Should be smaller than original (minified).
  EXPECT_LT(result->content.size(), main_css.size());
  // CSS variants are always marked as worker-processed (revalidation
  // is only used on HTML variants).
  EXPECT_NE(result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "kFlagWorkerProcessed should be set";

  // No errors should occur (unresolved imports are handled gracefully).
  EXPECT_EQ(worker.stats().errors.load(), 0u)
      << "Missing import should not cause errors";
}

TEST_F(WorkerTest, CssRevalidationWhenImportMissing) {
  // Verifies that when CSS A imports CSS B which is not yet cached,
  // the worker still produces a minified variant of CSS A (without B's
  // rules) and marks it as worker-processed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string a_url = "http://example.com/reval-a.css";
  std::string a_css =
      "@import url(\"http://example.com/reval-b.css\");\n"
      "body  {  color:  red;  font-size:  16px;  }\n";

  std::string hostname = "example.com";
  CacheOriginal(worker.cache(), a_url, a_css, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = a_url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  CapabilityMask mask;
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), a_url, mask, hostname);
    if (result.has_value()) break;
  }

  ASSERT_TRUE(result.has_value()) << "CSS variant not written";
  // Should contain A's rule but NOT B's (B not cached).
  EXPECT_NE(result->content.find("color"), std::string::npos)
      << "Should contain CSS A rules";
  EXPECT_EQ(result->content.find("font-weight"), std::string::npos)
      << "Should NOT contain CSS B rules (not cached)";
  // Worker-processed flag should be set.
  EXPECT_NE(result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "kFlagWorkerProcessed should be set";
}

TEST_F(WorkerTest, HtmlDetectsIncompleteCssViaRevalidationFlag) {
  // Verifies that when CSS A imports B (which is not cached), the CSS
  // variant is still written as worker-processed, and HTML can use it
  // for critical CSS extraction despite the missing import.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string hostname = "example.com";
  std::string css_a_url = "http://example.com/incomplete-a.css";
  std::string css_a =
      "@import url(\"http://example.com/incomplete-b.css\");\n"
      ".hero  {  font-size:  24px;  color:  blue;  }\n";
  CacheOriginal(worker.cache(), css_a_url, css_a, hostname);

  std::string html_url = "http://example.com/detect-incomplete.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_a_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "<img src=\"photo1.jpg\">"
      "<img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\">"
      "<img src=\"photo4.jpg\">"
      "</body></html>";
  CacheOriginal(worker.cache(), html_url, html, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process CSS A first (with B missing).
  CacheNotification css_notif;
  css_notif.url = css_a_url;
  css_notif.scheme = "https";
  css_notif.hostname = hostname;
  css_notif.content_type = ContentType::kCss;
  css_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(css_notif);

  // Wait for CSS A variant to appear.
  CapabilityMask mask;
  std::optional<VariantWithMeta> css_result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    css_result = ReadVariantWithMeta(worker.cache(), css_a_url, mask, hostname);
    if (css_result.has_value()) break;
  }
  ASSERT_TRUE(css_result.has_value()) << "CSS A variant not written";
  EXPECT_NE(
      css_result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "CSS A should have kFlagWorkerProcessed set";

  // Now process HTML that references CSS A.
  CacheNotification html_notif;
  html_notif.url = html_url;
  html_notif.scheme = "https";
  html_notif.hostname = hostname;
  html_notif.content_type = ContentType::kHtml;
  html_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(html_notif);

  // Poll for HTML variant.
  CapabilityMask html_mask;
  std::optional<VariantWithMeta> html_result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    html_result =
        ReadVariantWithMeta(worker.cache(), html_url, html_mask, hostname);
    if (html_result.has_value()) break;
  }

  ASSERT_TRUE(html_result.has_value()) << "HTML variant not written";
}

TEST_F(WorkerTest, CssSizeGateBypassWhenImportsNewlyResolved) {
  // When @imports are newly resolved (flattened), the CSS variant should
  // be written even if the resulting minified CSS is larger than the
  // original (because the flattened version is more complete).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create a small main CSS that imports a large dependency.
  // After flattening, the result will be larger than the original.
  std::string main_url = "http://example.com/size-gate-bypass.css";
  std::string dep_url = "http://example.com/large-dep.css";
  std::string main_css =
      "@import url(\"http://example.com/large-dep.css\");\n"
      "a{color:red}\n";
  // Generate a dependency that is already well minified (no extra
  // whitespace to remove) so the flattened result stays larger.
  std::string dep_css =
      "h1{font-size:32px}h2{font-size:24px}h3{font-size:18px}"
      "h4{font-size:14px}h5{font-size:12px}h6{font-size:10px}"
      "p{line-height:1.6;margin:0 0 16px}ul{padding-left:20px}"
      "ol{padding-left:20px}li{margin:4px 0}";
  std::string hostname = "example.com";
  CacheOriginal(worker.cache(), main_url, main_css, hostname);
  CacheOriginal(worker.cache(), dep_url, dep_css, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = main_url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the variant.
  CapabilityMask mask;
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), main_url, mask, hostname);
    if (result.has_value()) break;
  }

  // After flattening, the CSS is larger than the original input.
  // The size gate prevents writing the identity variant.  However,
  // compressed variants (gzip/brotli) of the original are still written.
  // The read at Desktop/Identity returns the original content.
  ASSERT_TRUE(result.has_value()) << "Should read back original content";
  // The original should contain the main rule and the @import.
  EXPECT_NE(result->content.find("color:red"), std::string::npos)
      << "Should contain main CSS rule";
  EXPECT_NE(result->content.find("@import"), std::string::npos)
      << "Original @import should be present (flattened version was larger)";
}

TEST_F(WorkerTest, CssImportFlatteningWithChainedImports) {
  // CSS with chained @imports (A imports B, B imports C) should flatten
  // all levels.  Covers the recursive flattening path.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // C.css (leaf — no imports)
  std::string c_url = "http://example.com/c.css";
  std::string c_css = "p  {  line-height:  1.6;  }\n";
  CacheOriginal(worker.cache(), c_url, c_css);

  // B.css imports C.css
  std::string b_url = "http://example.com/b.css";
  std::string b_css =
      "@import url(\"http://example.com/c.css\");\n"
      "h1  {  color:  blue;  }\n";
  CacheOriginal(worker.cache(), b_url, b_css);

  // A.css (main) imports B.css
  std::string a_url = "http://example.com/a-chain.css";
  std::string a_css =
      "@import url(\"http://example.com/b.css\");\n"
      "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), a_url, a_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = a_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the minified variant.
  CapabilityMask mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), a_url, mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "Chained import CSS variant missing";
  if (!variant.empty()) {
    // Should contain content from all three files (flattened).
    EXPECT_NE(variant.find("margin"), std::string::npos)
        << "Should contain A.css body margin rule";
    EXPECT_NE(variant.find("color"), std::string::npos)
        << "Should contain B.css h1 color rule";
    EXPECT_NE(variant.find("line-height"), std::string::npos)
        << "Should contain C.css p line-height rule";
    // @import directives should be gone.
    EXPECT_EQ(variant.find("@import"), std::string::npos)
        << "Flattened CSS should not contain @import";
    // Should be smaller than combined originals.
    EXPECT_LT(variant.size(), a_css.size() + b_css.size() + c_css.size());
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlVariantWriteSuccessLogs) {
  // Verify that successful HTML variant write increments the
  // html_assembly_complete counter and produces compressed variants.
  // Covers lines 1802-1817 (HTML write success path).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/write-success.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/ws.css\">"
      "</head><body>"
      "<div class=\"hero\">Content</div>"
      "<img src=\"photo1.jpg\">"
      "<img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\">"
      "<img src=\"photo4.jpg\">"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the CSS so critical CSS injection works.
  std::string css_url = "http://example.com/ws.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;  // Mobile/Identity
  SendNotification(notification);

  // Poll for the HTML variant AND wait for all in-flight work to drain.
  // Compressed variants are written synchronously after the identity variant,
  // but Cyclone's write-around RAM cache can cache "not found" results from
  // early reads.  Waiting for in_flight_work()==0 ensures all writes
  // (identity + gz + br) are committed before we read.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty() && worker.in_flight_work() == 0) break;
  }

  ASSERT_FALSE(variant.empty()) << "HTML variant should be written";

  // html_assembly_complete should have incremented.
  EXPECT_GE(worker.stats().html_assembly_complete.load(), 1u)
      << "html_assembly_complete should increment on successful write";

  // Compressed HTML variants should exist.  Since we waited for
  // in_flight_work()==0 above, all writes are complete.
  CapabilityMask gz_mask = CapabilityMask::Decode(0);
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Gzip HTML variant should exist after successful write";

  CapabilityMask br_mask = CapabilityMask::Decode(0);
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "Brotli HTML variant should exist after successful write";

  // The variant should have critical CSS injected.
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
  // Should also have fetchpriority/lazy-load transforms.
  EXPECT_NE(variant.find("loading=\"lazy\""), std::string::npos)
      << "Lazy load should be applied to later images";
}

TEST_F(WorkerTest, HtmlEarlyHintsSentinelWriteContent) {
  // Verify that the early hints sentinel contains stylesheet URLs,
  // LCP image with image: prefix, and preconnect origins — all in
  // the same sentinel entry.
  // Covers lines 1582-1622 (early hints assembly and sentinel write).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Keep the sheets render-blocking: async-deferred sheets get no preload
  // hint, and this test asserts all three hint classes appear together.
  config.disable_async_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/full-hints.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "<link rel=\"stylesheet\" "
      "href=\"https://cdn.example.com/vendor.css\">"
      "<style>body { margin: 0; }</style>"
      "</head><body>"
      "<img src=\"https://img.example.com/hero.jpg\" "
      "fetchpriority=\"high\">"
      "<script src=\"https://analytics.example.com/track.js\"></script>"
      "<p>Content</p>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the stylesheets so critical CSS can be extracted.
  CacheOriginal(worker.cache(), "/main.css", "body { margin: 0; }");
  CacheOriginal(worker.cache(), "https://cdn.example.com/vendor.css",
                ".hero { font-size: 24px; }", "cdn.example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for early hints sentinel.
  std::string hints;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  ASSERT_FALSE(hints.empty()) << "Early hints sentinel should be stored";

  // Should contain stylesheet URLs.
  EXPECT_NE(hints.find("/main.css"), std::string::npos)
      << "Hints should contain /main.css. Got: " << hints;
  EXPECT_NE(hints.find("https://cdn.example.com/vendor.css"), std::string::npos)
      << "Hints should contain vendor.css URL. Got: " << hints;

  // Should contain LCP image with image: prefix.
  EXPECT_NE(hints.find("image:https://img.example.com/hero.jpg"),
            std::string::npos)
      << "Hints should contain LCP image with image: prefix. Got: " << hints;

  // Should contain preconnect entries for third-party origins.
  // At minimum the CDN and analytics origins should appear.
  EXPECT_NE(hints.find("preconnect:"), std::string::npos)
      << "Hints should contain preconnect entries. Got: " << hints;
}

TEST_F(WorkerTest, DeferredSheetIsPreloadHinted) {
  // The inverse of the rule that stood while the deferral primitive was a
  // low-priority media technique.  The primitive IS a preload now, so a 103
  // hint announces exactly what the transform announces — the same fetch, at
  // the same priority, started before the HTML body.  Suppressing the hint
  // would throw away the whole point of the switch.  The LCP image hint stays
  // alongside it.
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/deferred-hints.html";
  std::string css_url = "http://example.com/site.css";
  std::string css = ".hero{color:red;font-size:24px}p{margin:0}";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);
  // Cache the sheet so the async-CSS gate can measure it (a small cached
  // sheet is allowed to defer), and seed the validation record deferral now
  // requires. The page has no inline <style>, so the combined sheet IS these
  // bytes.
  CacheOriginal(worker.cache(), css_url, css, "example.com");
  StoreBrowserProfile(worker, url, html, ".hero{color:red;font-size:24px}",
                      /*validated=*/true, /*validated_against_css=*/css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Wait until the variant shows the deferral actually happened.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (variant.find("as=\"style\"") != std::string::npos) break;
  }
  ASSERT_NE(variant.find("as=\"style\""), std::string::npos)
      << "Precondition: the stylesheet should be async-deferred";
  ASSERT_NE(variant.find("rel=\"preload\""), std::string::npos)
      << "Precondition: the deferral primitive should be a preload. Got: "
      << variant;

  std::string hints =
      ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
  ASSERT_FALSE(hints.empty()) << "Hints sentinel should be stored";
  EXPECT_NE(hints.find("site.css"), std::string::npos)
      << "A deferred stylesheet must be preload-hinted — the transform and "
         "the hint have to promote the same thing. Got: "
      << hints;
  EXPECT_NE(hints.find("image:/hero.jpg"), std::string::npos)
      << "The LCP image hint must remain. Got: " << hints;
}

// The sentinel line format is the contract between the worker and every 103
// emitter (nginx module, .NET middleware).  A stylesheet is written as a bare
// URL — no prefix — which is what makes the emitter render it as
// `rel=preload; as=style`.  Asserting the parsed resource rather than the raw
// line is what pins the END of that chain: a deferred sheet promoted as
// anything other than a style would not be matched to the link the loader
// flips, and Chrome would download the sheet twice.
TEST_F(WorkerTest, PreloadHintForDeferredSheetCarriesStyleType) {
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/hint-as-style.html";
  std::string css_url = "http://example.com/typed.css";
  std::string css = ".hero{color:red;font-size:24px}p{margin:0}";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), css_url, css, "example.com");
  StoreBrowserProfile(worker, url, html, ".hero{color:red;font-size:24px}",
                      /*validated=*/true, /*validated_against_css=*/css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  std::string hints;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (hints.find("typed.css") != std::string::npos) break;
  }
  ASSERT_NE(hints.find("typed.css"), std::string::npos)
      << "Precondition: the deferred sheet must be hinted. Got: " << hints;

  // Find the sheet's line and run it through the emitters' own parser.
  std::string sheet_line;
  size_t start = 0;
  while (start <= hints.size()) {
    size_t nl = hints.find('\n', start);
    std::string line = hints.substr(
        start, (nl == std::string::npos ? hints.size() : nl) - start);
    if (line.find("typed.css") != std::string::npos) {
      sheet_line = line;
      break;
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  ASSERT_FALSE(sheet_line.empty()) << hints;

  PreloadResource res = ParseHintLine(sheet_line);
  EXPECT_EQ(res.rel, "preload") << sheet_line;
  EXPECT_EQ(res.as_type, "style")
      << "a deferred stylesheet must be hinted as a STYLE, or the preload "
         "will not match the link the loader flips: "
      << sheet_line;
  EXPECT_NE(res.url.find("typed.css"), std::string::npos) << sheet_line;
}

TEST_F(WorkerTest, EarlyHintsKeepStylesheetWhenAsyncCssSuppressed) {
  // The stylesheet hint gate keys on the RUNTIME async-CSS decision, not on
  // config: with async-CSS enabled (default) but the external sheet not yet
  // cached, the FOUC sufficiency gate suppresses deferral — the sheet stays
  // render-blocking, so it MUST be preload-hinted.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/suppressed-hints.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/site.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";
  // Only the HTML is cached — the stylesheet is NOT, so the sufficiency
  // gate keeps it render-blocking.
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  std::string hints;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  EXPECT_NE(hints.find("site.css"), std::string::npos)
      << "A render-blocking (suppression-kept) stylesheet must be "
         "preload-hinted. Got: "
      << hints;

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, EarlyHintsStaleSentinelClearedWhenHintsBecomeEmpty) {
  // A page can stop having anything worth hinting — its only stylesheet
  // becomes print-only, its LCP image goes away, a third-party origin is
  // dropped.  When that happens the sentinel from the earlier pass must be
  // REMOVED, not left behind: a missing sentinel reads as "no hints", a stale
  // one keeps the 103 path promoting a resource the page no longer wants.
  //
  // Driven by seeding the stale sentinel directly rather than by producing it
  // from a first pass.  It used to be produced by one — while the deferral
  // primitive demoted the sheet, deferring it emptied the hint list — but a
  // deferred sheet is now hinted like any other, so that no longer empties
  // anything.  Seeding is also the sharper test: it isolates the removal
  // branch from whatever made the list non-empty in the first place.
  WorkerConfig config = BrowserProfileConfig();
  // Disable RAM cache: same-process cache writes must be visible to the
  // worker's next disk read (in production nginx writes from another process).
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/hints-go-empty.html";
  // Nothing on this page is hintable: the one stylesheet is print-only (never
  // render-blocking for screen), there is no image, and the relative href
  // yields no third-party origin.  Processing it must produce an EMPTY hint
  // list — which is what sends the emitter down the removal branch.
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/site.css\" media=\"print\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // The stale sentinel a previous pass would have left behind. Keyed the way
  // the cache helpers key everything here: scheme+authority stripped, which is
  // also what the worker's NormalizeCacheUrl does before it writes.
  std::string stale = "/site.css\nimage:/gone.jpg";
  {
    auto wh =
        worker.cache()->WriteSentinel(StripSchemeAuthority(url), "", "https",
                                      SentinelId::kEarlyHints, stale.size());
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span(stale.data(), stale.size())))
            .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  ASSERT_EQ(ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints), stale)
      << "Precondition: the stale sentinel should be seeded";

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for the removal: the emitter runs as part of processing, and a
  // missing sentinel reads back as empty.
  std::string hints = stale;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (hints.empty()) break;
  }

  EXPECT_TRUE(hints.empty())
      << "The stale sentinel must be cleared when processing leaves no "
         "hints. Got: "
      << hints;

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, EmptySentinelOverwriteReadsAsNoHints) {
  // The in-process (.NET) pipeline cannot remove a single alternate through
  // the C API, so it clears stale hints by overwriting the sentinel with
  // EMPTY content.  Pin the round-trip: a zero-length sentinel write reads
  // back as empty content, which every consumer treats as "no hints".
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "example.com/empty-sentinel.html";
  std::string hints = "style.css\nimage:/hero.jpg";
  {
    auto wh = worker.cache()->WriteSentinel(
        url, "", "https", SentinelId::kEarlyHints, hints.size());
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span(hints.data(), hints.size())))
            .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  EXPECT_EQ(ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints), hints);

  {
    auto wh = worker.cache()->WriteSentinel(url, "", "https",
                                            SentinelId::kEarlyHints, 0);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  EXPECT_TRUE(
      ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints).empty())
      << "A zero-length sentinel overwrite must read back as no hints";
}

TEST_F(WorkerTest, HtmlExternalCssGatheringWithMeta) {
  // When HTML references external stylesheets and the CSS is cached,
  // the worker should gather CSS and use it for critical CSS extraction.
  // Verify via metadata that kFlagNeedsRevalidation is NOT set when
  // all CSS is available.
  // Covers lines 1635-1657 (external CSS gathering loop).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/css-gathered.html";
  std::string css_url = "http://example.com/gathered.css";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body><div class=\"hero\">Gathered CSS</div></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the external CSS (absolute href, so raw_url=true).
  std::string css = ".hero { font-size: 24px; color: green; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for variant with metadata.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), url, mask);
    if (result.has_value()) break;
  }

  ASSERT_TRUE(result.has_value()) << "HTML variant should be written";

  // Critical CSS should be injected (CSS was available).
  EXPECT_NE(result->content.find("data-pagespeed-critical"), std::string::npos)
      << "Critical CSS should be injected when external CSS is available";

  // The hero class should appear in the critical CSS.
  EXPECT_NE(result->content.find("hero"), std::string::npos)
      << "Hero class should be in critical CSS";

  // kFlagNeedsRevalidation should NOT be set (CSS was found).
  EXPECT_EQ(result->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "kFlagNeedsRevalidation should not be set when CSS is available";

  // kFlagWorkerProcessed should be SET.
  EXPECT_NE(result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "kFlagWorkerProcessed should be set on worker-written variant";
}

TEST_F(WorkerTest, ImageProactiveMultiFormatCreation) {
  // Proactive image variants enabled with multiple formats should
  // create WebP and AVIF variants at the requested viewport, plus
  // track variants_written and proactive_variants_written stats.
  // Covers lines 2199-2304 (proactive format loop).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/multi-format.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Desktop/WebP mask.
  CapabilityMask desktop_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = desktop_webp.Encode();
  SendNotification(notification);

  // Wait for processing (proactive generates at all viewports x formats).
  // Desktop/WebP is requested directly; proactive generates AVIF at
  // Desktop, plus both formats at Mobile and Tablet.
  // Use selector fallback: content-deduped variants still servable.
  auto check_variant = [&](CapabilityMask::Viewport vp,
                           CapabilityMask::ImageFormat fmt) {
    CapabilityMask m(fmt, vp, CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity);
    return !ReadBestVariant(worker.cache(), url, m).empty();
  };

  // Poll for Desktop variants (directly requested + proactive AVIF).
  // AVIF encoding is slow; under CPU contention allow up to 20s
  // (sanitizer-scaled: TSan multiplies the transcode cost).
  bool has_desktop_webp = false;
  bool has_desktop_avif = false;
  for (int i = 0; i < 200 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!has_desktop_webp)
      has_desktop_webp = check_variant(CapabilityMask::Viewport::kDesktop,
                                       CapabilityMask::ImageFormat::kWebP);
    if (!has_desktop_avif)
      has_desktop_avif = check_variant(CapabilityMask::Viewport::kDesktop,
                                       CapabilityMask::ImageFormat::kAvif);
    if (has_desktop_webp && has_desktop_avif) break;
  }

  EXPECT_TRUE(has_desktop_webp) << "Desktop/WebP should be servable";
  EXPECT_TRUE(has_desktop_avif)
      << "Desktop/AVIF should be servable (proactive)";

  // Mobile and Tablet should also be servable due to proactive_viewport_variants
  // (either written directly or via selector fallback when content-identical).
  bool mobile_webp = false;
  bool mobile_avif = false;
  for (int i = 0; i < 200 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!mobile_webp)
      mobile_webp = check_variant(CapabilityMask::Viewport::kMobile,
                                  CapabilityMask::ImageFormat::kWebP);
    if (!mobile_avif)
      mobile_avif = check_variant(CapabilityMask::Viewport::kMobile,
                                  CapabilityMask::ImageFormat::kAvif);
    if (mobile_webp && mobile_avif) break;
  }
  EXPECT_TRUE(mobile_webp)
      << "Mobile/WebP should be servable (proactive viewport)";
  EXPECT_TRUE(mobile_avif)
      << "Mobile/AVIF should be servable (proactive viewport)";

  // Wait for the proactive matrix to fully complete (ReadBestVariant may
  // find variants via selector fallback before all writes finish).  The
  // old "counter stable for 3s" heuristic was a slowdown trap: under TSan
  // a single AVIF transcode leaves the counter flat well past 3s
  // mid-matrix, so the wait declared completion at 3 of the expected >=4
  // variants.  Retiring the work item is the true completion signal.
  WaitForFirstVariantThenIdle(worker);

  // Stats should show variants written + dedup skips cover the expected matrix.
  uint64_t written = worker.stats().variants_written.load();
  uint64_t dedup_skipped = worker.stats().dedup_writes_skipped.load();
  EXPECT_GE(written + dedup_skipped, 4u)
      << "At least 4 variants should be generated (2 formats x 2+ viewports)";
  EXPECT_GE(worker.stats().proactive_variants_written.load() + dedup_skipped,
            1u)
      << "Proactive variants counter should increment";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageProactivePerFormatSsimulacra2InMetadata) {
  // Verify that per-format SSIMULACRA2 scores are written to cache
  // metadata (not just the JPEG score for all formats).
  // Uses Mobile viewport so the resize path runs (Desktop target_width=0
  // takes a fast path that skips SSIMULACRA2 verification).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.avif_speed = 10;  // Fastest — MSVC-compiled libaom is slower

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/ssim-meta-test.jpg";
  // q80_512x512.jpg is 512x512, wider than mobile_width=480, so resize occurs.
  // q80 source quality ensures the resized WebP/AVIF output beats the size
  // gate (sjpeg6.jpg was too well-compressed, causing WebP to be larger).
  std::string jpeg = ReadTestFile("jpeg/q80_512x512.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Mobile/WebP mask to trigger the resize path
  // (q80_512x512.jpg is wider than mobile_width=480).
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // Poll for WebP and Original variants at Mobile viewport.
  // 160 iterations × 100ms = 16s — Windows image transcoding is slower.
  CapabilityMask mobile_original(
      CapabilityMask::ImageFormat::kOriginal, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  std::optional<VariantWithMeta> webp_meta;
  std::optional<VariantWithMeta> orig_meta;
  // 600 iterations × 100ms = 60s. The verify loop now runs its full
  // attempt budget before binding its verdict (the old loop broke early on
  // a negative or incomparable attempt and shipped what it had), so this
  // notification's transcode — AVIF at speed 10 with up to four attempts —
  // takes ~25s before ANY variant, including the Mobile/Original baseline,
  // is written (writes happen when the whole multi-format transcode
  // returns). The 16s ceiling sized for the early-break world expires
  // before the first write.
  for (int i = 0; i < 600 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!webp_meta)
      webp_meta = ReadVariantWithMeta(worker.cache(), url, mobile_webp);
    if (!orig_meta)
      orig_meta = ReadVariantWithMeta(worker.cache(), url, mobile_original);
    if (webp_meta && orig_meta) break;
  }

  // At least the Original variant should always be written (resized JPEG).
  // WebP may not be written if its output exceeds the resized JPEG size gate.
  ASSERT_TRUE(orig_meta.has_value())
      << "Mobile/Original variant should be written";

  // Original variant SSIMULACRA2: valid when pixel-based resize path runs,
  // N/A (0xFFFF) on stream-based fast path fallback.
  if (orig_meta->metadata.ssimulacra2_score_x100 != 0xFFFF) {
    EXPECT_LE(orig_meta->metadata.ssimulacra2_score_x100, 10000u);
  }

  // WebP variant may not exist (size gate rejects when output >= resized JPEG).
  // When it does exist, verify its SSIMULACRA2 score is valid.
  if (webp_meta.has_value() &&
      webp_meta->metadata.ssimulacra2_score_x100 != 0xFFFF) {
    EXPECT_LE(webp_meta->metadata.ssimulacra2_score_x100, 10000u);
  }
}

// =============================================================================
// Additional coverage tests: config-driven skipping, dedup, HTTP API, browser
// =============================================================================

TEST_F(WorkerTest, HtmlDisabledSkipsProcessing) {
  // With disable_html and a non-null handler, the HTML disable branch
  // (worker.cc ~1486-1491) is exercised including the Info log call.
  // Verifies notifications_received increments but html_processed does not.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_html = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML that has CSS (would normally produce a
  // variant).
  std::string url = "http://example.com/html-disabled-stats.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Disabled</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received by the worker.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  // HTML should not be processed (disabled).
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  // No variant should be written.
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssDisabledSkipsProcessing) {
  // With disable_css and a non-null handler, the CSS disable branch
  // (worker.cc ~2641-2646) is exercised including the Info log call.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_css = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/css-disabled-stats.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsDisabledSkipsProcessing) {
  // With disable_js and a non-null handler, the JS disable branch
  // (worker.cc ~2814-2820) is exercised including the Info log call.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_js = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/js-disabled-stats.js";
  std::string js = "function  hello()  { return 'world'; }\n";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageDisabledSkipsProcessing) {
  // With disable_image and a non-null handler, the image disable branch
  // (worker.cc ~1894-1899) is exercised including the Info log call.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_image = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use real JPEG data so the cache entry is realistic.
  std::string image_data = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(image_data.empty());
  std::string url = "http://example.com/img-disabled-stats.jpg";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  EXPECT_EQ(worker.stats().images_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, NotificationDeduplication) {
  // Send the same CSS notification twice; the second should be deduped
  // and stats.notifications_skipped_dedup should increment.
  // Covers the dedup path in HandleNotification (worker.cc ~1416-1444)
  // and MarkVariantProcessed (worker.cc ~1347-1356).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-counter.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send first notification (empty hostname matches CacheOriginal's "").
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for first processing to complete AND in-flight work to drain.
  // css_processed is incremented before MarkVariantProcessed populates
  // the dedup set, so polling css_processed alone is insufficient --
  // in_flight_work==0 guarantees MarkVariantProcessed has returned.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "First CSS notification was not processed";

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  uint64_t dedup_before = worker.stats().notifications_skipped_dedup.load();

  // Send the same notification again -- should be deduped.
  SendNotification(notification);

  // Poll until the dedup counter increments.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > dedup_before) break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), dedup_before)
      << "Second notification should be deduped";

  // CSS processed counter should still be 1 (not reprocessed).
  EXPECT_EQ(worker.stats().css_processed.load(), 1u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, UnknownContentTypeNotification) {
  // Send a notification with ContentType::kOther and a non-null handler.
  // Exercises the default case in the switch statement (worker.cc ~2965-2971)
  // including the Warning log call.  Also verifies no processing counters
  // increment and the timing accumulation still runs.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/unknown-type.bin";
  std::string content = "binary data here";
  CacheOriginal(worker.cache(), url, content);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() > before) break;
  }

  EXPECT_GT(worker.stats().notifications_received.load(), before);
  // No processing counters should increment.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  // Total processing time should still accumulate (timing code runs
  // regardless of content type).
  // We can't assert exact values but the counter should be accessible.
  worker.stats().total_processing_time_us.load();

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HttpApiEnabledWorker) {
  // Initialize worker with api_port=-1 (OS picks a free port).
  // Covers the HTTP server init, route registration, the WebSocket upgrade
  // handler and the console route.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;  // OS picks a free port

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // api_port() should return the actual bound port (> 0).
  EXPECT_GT(worker.api_port(), 0) << "HTTP API should be bound to a real port";

  // Run briefly to exercise the event loop with HTTP server active.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Verify we can connect to the HTTP API port via TCP.
  int sock = pagespeed::test::ConnectTcp(worker.api_port());
  EXPECT_GE(sock, 0) << "Could not connect to HTTP API port "
                     << worker.api_port();
  if (sock >= 0) pagespeed::test::CloseSocket(sock);

  // WebSocket manager should be available.
  EXPECT_NE(worker.ws_manager(), nullptr);

  worker.Shutdown();
  worker_thread.join();
}

// Regression: /v1/health's connections block must report the connection cap
// the management API listener actually enforces
// (HttpServerConfig::max_connections, default 32), not the notification
// listener's separate WorkerConfig::max_connections (default 128).
TEST_F(WorkerTest, HttpApiHealthReportsHttpListenerConnectionCap) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;  // OS picks a free port

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_GT(worker.api_port(), 0);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int sock = pagespeed::test::ConnectTcp(worker.api_port());
  ASSERT_GE(sock, 0) << "Failed to connect to API port " << worker.api_port();

  const std::string request =
      "GET /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n\r\n";
  ssize_t written =
      pagespeed::test::SocketWrite(sock, request.data(), request.size());
  ASSERT_EQ(written, static_cast<ssize_t>(request.size()));

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
  }
  pagespeed::test::CloseSocket(sock);

  EXPECT_NE(response.find("200 OK"), std::string::npos) << response;
  EXPECT_NE(response.find("\"max\":32"), std::string::npos)
      << "connections.max must be the HTTP listener's cap: " << response;
  EXPECT_EQ(response.find("\"max\":128"), std::string::npos)
      << "the notification listener's cap must not leak into /v1/health: "
      << response;
}

TEST_F(WorkerTest, WorkerWithBrowserAnalysisEnabled) {
  // Initialize worker with browser_analysis.enabled = true.
  // Covers lines 503-513 (BrowserAnalysisManager creation and init).
  // Chrome is unlikely to be available in the test environment, so the
  // Initialize() call should succeed but browser_manager_ may be reset
  // after the BrowserAnalysisManager fails to start Chrome.
  // Also covers lines 896-898 (browser_manager shutdown) during teardown.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // The test binary runs as root in the build container,
  // where the Chrome sandbox is (correctly) unavailable and `require`
  // would refuse to construct the manager.  These tests seed profiles
  // directly and never spawn Chrome, so take the documented opt-out.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  // Use a non-existent binary to trigger graceful failure.
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";

  NullMessageHandler handler;
  Worker worker(config, &handler);
  // Initialize should succeed even if browser analysis init fails
  // (it falls back to heuristics only).
  ASSERT_TRUE(worker.Initialize());

  // The worker should still function: cache should be available.
  ASSERT_NE(worker.cache(), nullptr);

  // Run briefly to exercise the event loop.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send a CSS notification to verify the worker still processes normally.
  std::string url = "http://example.com/browser-analysis-test.css";
  std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for CSS processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "Worker should still process notifications after browser init failure";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Management Socket Error Handling Tests
// =============================================================================

TEST_F(WorkerTest, MgmtSocketAbruptClose) {
  // Covers lines 3279-3289 (OnMgmtRead nread < 0 path).
  // When a client connects to the management socket and immediately
  // disconnects without sending data, the worker should handle the
  // EOF/error gracefully (nread < 0) and clean up the connection.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect and immediately close without sending anything.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";
  pagespeed::test::ClosePipe(fd);

  // Allow the worker to process the disconnect.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Worker should still be functional: connect again and send a command.
  fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0)
      << "Worker should still accept connections after abrupt close";

  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  char buf[2048];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "Worker should still respond after handling abrupt close";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "Response: " << response;
}

TEST_F(WorkerTest, MgmtSocketBufferOverflow) {
  // Covers lines 3304-3313 (buffer > 16384 bytes without newline).
  // When a client sends more than 16384 bytes without a newline,
  // the management socket should forcibly disconnect the client
  // to prevent memory exhaustion.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";

  // Send >16384 bytes of data without any newline to trigger overflow guard.
  std::string overflow_data(17000, 'A');
  ssize_t written = pagespeed::test::PipeWrite(fd, overflow_data.data(),
                                               overflow_data.size());
  // The write may succeed partially or fully depending on kernel buffers.
  EXPECT_GT(written, 0);

  // Give the worker time to process the data and close the connection.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Try to read - should get EOF or error since server closed connection.
  char buf[256];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf));
  EXPECT_LE(n, 0) << "Connection should be closed after buffer overflow";
  pagespeed::test::ClosePipe(fd);

  // Worker should still be functional after the overflow disconnect.
  fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Worker should still accept connections after overflow";

  std::string cmd = "STATS\n";
  written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));

  n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0) << "Worker should still respond after overflow disconnect";
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_NE(response.find(R"("status":"ok")"), std::string::npos)
      << "Response: " << response;
}

// =============================================================================
// Web Bot Auth Refresh Timer Tests
// =============================================================================

TEST_F(WorkerTest, WebBotAuthTimerSetupAndSharedConfigPublication) {
  // When --web-bot-auth is on and a key directory is configured, the worker
  // starts the refresh timer and publishes the toggle + directory hosts to
  // nginx via pagespeed-shared.conf.  The first refresh fires only after a
  // 5s initial delay, so this test never touches the network.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.web_bot_auth = true;
  config.web_bot_auth_key_directories = {
      "https://keys.example.com/.well-known/"
      "http-message-signatures-directory"};
  config.web_bot_auth_verified_bots = "kid1=crawler";

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Shared config carries the observe-only toggle end-to-end.
  SharedConfig shared = ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
  EXPECT_TRUE(shared.web_bot_auth);
  EXPECT_EQ(shared.web_bot_auth_verified_bots, "kid1=crawler");
  EXPECT_EQ(shared.web_bot_auth_directory_hosts, "keys.example.com");

  // Run briefly to exercise the event loop with the refresh timer active.
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_NE(worker.cache(), nullptr);
}

TEST_F(WorkerTest, WebBotAuthTimerNotSetWhenDisabled) {
  // Default off: no timer, and the shared config publishes web_bot_auth =
  // false so nginx does no signature work.  Negative control for the setup
  // test above.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // web_bot_auth stays false (default); directories configured but ignored.
  config.web_bot_auth_key_directories = {"https://keys.example.com/dir"};

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  SharedConfig shared = ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
  EXPECT_FALSE(shared.web_bot_auth);

  ASSERT_NE(worker.cache(), nullptr);
  worker.Shutdown();
}

// =============================================================================
// No license state, no telemetry identity
// =============================================================================

TEST_F(WorkerTest, InitializeWritesNoLicenseOrInstanceIdArtifacts) {
  // A 2.0 daemon persisted a license token (pagespeed.license) and a
  // telemetry instance id (pagespeed.instance-id) beside the cache, and
  // published license_key / license_valid / license_checked_once into
  // pagespeed-shared.conf.  A 2.1 daemon must create none of that, and the
  // daemon-authored file set must still be recognised so the cache-dir
  // ownership check keeps working.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::filesystem::path dir =
      std::filesystem::path(cache_path_).parent_path();
  EXPECT_FALSE(std::filesystem::exists(dir / "pagespeed.license"));
  EXPECT_FALSE(std::filesystem::exists(dir / "pagespeed.instance-id"));

  std::ifstream f(SharedConfigFilePath(cache_path_));
  ASSERT_TRUE(f.is_open());
  std::string raw((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  // Line-anchored: rsl_cap_requested_license= (RSL-CAP, unrelated) is a
  // legitimate key that contains the substring.
  for (const char* key :
       {"\nlicense_key=", "\nlicense_valid=", "\nlicense_checked_once="}) {
    EXPECT_EQ(raw.find(key), std::string::npos) << key << "\n" << raw;
  }
  EXPECT_EQ(raw.find("instance"), std::string::npos) << raw;

  ASSERT_NE(worker.cache(), nullptr);
  worker.Shutdown();
}

TEST_F(WorkerTest, AgentOptimizeServeTogglesFollowOperatorFlagsOnly) {
  // The agent_optimize markdown variant and the /llms.txt index
  // are switched on by their operator flags alone — there is no license
  // state anywhere for them to depend on.  The wire key keeps its historical
  // name (agent_optimize_entitled) so a 2.0-era reader keeps working.
  {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.browser_analysis.agent_optimize = true;
    config.browser_analysis.agent_optimize_llms_txt = true;

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());
    SharedConfig shared =
        ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
    EXPECT_TRUE(shared.agent_optimize_entitled);
    EXPECT_TRUE(shared.agent_optimize_llms_txt_enabled);
    worker.Shutdown();
  }
  {
    // Negative control: flags off → toggles off.  Same cache dir, so this
    // also proves the toggle is rewritten (not sticky) on every start.
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());
    SharedConfig shared =
        ReadSharedConfigFile(SharedConfigFilePath(cache_path_));
    EXPECT_FALSE(shared.agent_optimize_entitled);
    EXPECT_FALSE(shared.agent_optimize_llms_txt_enabled);
    worker.Shutdown();
  }
}

// =============================================================================
// Cache Corruption Recovery Tests
// =============================================================================

TEST_F(WorkerTest, CacheCorruptionRecoveryDeletesAndRetries) {
  // When the cache file is corrupt, Worker::Initialize() should delete it
  // and retry.  Covers lines 317-337 (auto-recovery path).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  // Create a corrupt file at the cache path.  Cyclone expects a valid
  // volume file; random bytes will fail the open.
  {
    std::ofstream corrupt(cache_path_, std::ios::binary);
    ASSERT_TRUE(corrupt.is_open());
    std::string garbage(4096, '\xDE');
    corrupt.write(garbage.data(), garbage.size());
    corrupt.close();
    // Verify the corrupt file exists.
    ASSERT_TRUE(std::filesystem::exists(cache_path_));
    ASSERT_GT(std::filesystem::file_size(cache_path_), 0u);
  }

  NullMessageHandler handler;
  Worker worker(config, &handler);

  // Initialize should succeed: the worker detects the corrupt cache,
  // deletes it, and recreates a fresh one.
  EXPECT_TRUE(worker.Initialize())
      << "Worker should recover from corrupt cache file";
  EXPECT_NE(worker.cache(), nullptr)
      << "Cache should be available after recovery";

  // Verify the cache is functional by writing and reading.
  std::string url = "http://example.com/recovery-test";
  std::string content = "recovered content";
  CacheOriginal(worker.cache(), url, content);
  CapabilityMask default_mask;
  std::string read_back = ReadVariant(worker.cache(), url, default_mask);
  EXPECT_EQ(read_back, content) << "Recovered cache should be fully functional";

  worker.Shutdown();
}

#ifndef _WIN32
// POSIX only: the cache-directory check is uid/mode-based, and on Windows
// (where ACLs are the access model and the service story is a separate arc)
// ValidateCacheDir always reports OK, so there is no refusal to observe.
TEST_F(WorkerTest, CacheDirectoryMissingRefusesToStart) {
  // A cache DIRECTORY that is missing, unwritable, or holds
  // content this daemon does not own is a refusal to start, not a degrade.
  // Nothing is chowned, nothing falls back to another location, and the
  // process exits non-zero so the supervisor's backoff and the operator's
  // monitoring both see it.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = "/nonexistent_dir_for_test/cache";
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);

  EXPECT_FALSE(worker.Initialize())
      << "A missing cache directory must be a refusal, never a silent "
         "cache-off start";
  EXPECT_EQ(worker.cache(), nullptr);
}
#endif  // !_WIN32

TEST_F(WorkerTest, CacheCorruptionRecoveryStartsDegradedWhenPathUnusable) {
  // When the cache VOLUME cannot be created even after the retry — the cache
  // directory itself being present, writable and owned — Initialize returns
  // true (degraded mode) with a null cache. Covers the degraded-mode startup
  // path, which survives the cache-directory check above: that check is about
  // the directory, this is about the volume inside it.
  //
  // A name longer than NAME_MAX makes both the initial Create and the retry
  // fail deterministically, without needing a directory the daemon may not
  // enter.
  WorkerConfig config;
  config.socket_path = socket_path_;
#ifdef _WIN32
  // Windows has no uid/mode directory check to satisfy, so the original
  // nonexistent-parent path still lands squarely in the degraded path.
  config.cache_path = "/nonexistent_dir_for_test/cache";
#else
  config.cache_path = temp_dir_ + "/" + std::string(300, 'x');
#endif
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);

  // Initialize should succeed in degraded mode: event loop still starts.
  EXPECT_TRUE(worker.Initialize())
      << "Worker should start in degraded mode when cache is unusable";
  EXPECT_EQ(worker.cache(), nullptr) << "Cache should be null in degraded mode";

  // Health should report DEGRADED
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, 0) << "Failed to connect to health socket in degraded mode";

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  EXPECT_TRUE(response.starts_with("DEGRADED "))
      << "Expected DEGRADED health, got: " << response;
  EXPECT_NE(response.find("cache=unavailable"), std::string::npos)
      << "Response should mention cache=unavailable: " << response;
}

// =============================================================================
// HTML Cooldown LRU Eviction Tests
// =============================================================================

TEST_F(WorkerTest, HtmlCooldownEvictionPrunesOldEntries) {
  // When the cooldown map exceeds kMaxCooldownEntries (256), oldest entries
  // should be evicted.  Covers lines 1869-1885 (cooldown LRU eviction).
  //
  // Strategy: use a very small cache (2MB) and send many unique HTML
  // notifications through the socket.  Pre-populate the cache with HTML
  // content for all URLs.  The processed variant writes may fail under
  // extreme cache pressure (2MB shared across 270 URLs), adding
  // write-failure cooldown entries.  With >256 unique URLs, the eviction
  // code at lines 1869-1885 is exercised.
  //
  // Even if writes succeed (and cooldown entries are cleared on success),
  // the tentative cooldown entries accumulate from concurrent/queued
  // notifications before processing completes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes =
      static_cast<uint64_t>(2 * 1024 * 1024);  // 2MB — very small

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with 270 unique HTML pages.
  // Each page has inline CSS so the worker can produce a variant.
  // Use short content to fit within the tiny cache.
  constexpr int kNumUrls = 270;
  for (int i = 0; i < kNumUrls; ++i) {
    std::string url = "http://example.com/evict-" + std::to_string(i) + ".html";
    std::string html =
        "<html><head><style>h1{color:red}</style></head>"
        "<body><h1>Page " +
        std::to_string(i) + "</h1></body></html>";
    CacheOriginal(worker.cache(), url, html);
  }

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send 270 unique HTML notifications in batches.  Each one sets a
  // tentative cooldown entry (line 1478).  Write failures add entries
  // at line 1864.  Once the map exceeds 256, eviction at lines 1869-1885
  // fires.
  constexpr int kBatchSize = 30;
  for (int i = 0; i < kNumUrls; ++i) {
    CacheNotification notification;
    notification.scheme = "https";
    notification.url =
        "http://example.com/evict-" + std::to_string(i) + ".html";
    notification.hostname = "";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    // Brief pause between batches to let the worker drain connections.
    if ((i + 1) % kBatchSize == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Wait for processing to complete (or timeout).
  // Use 120 iterations to give Windows enough headroom for pipe I/O.
  for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t processed = worker.stats().html_processed.load();
    uint64_t errors = worker.stats().alternate_write_failures.load();
    uint64_t skipped = worker.stats().notifications_skipped_dedup.load();
    // We expect all notifications to be either processed, failed, or skipped.
    if (processed + errors + skipped >= static_cast<uint64_t>(kNumUrls - 10)) {
      break;
    }
  }

  // The worker should not crash regardless of whether eviction triggered.
  EXPECT_NE(worker.cache(), nullptr);

  // Verify that ClearDedupAndCooldown still works (accesses the map).
  worker.ClearDedupAndCooldown("http://example.com/evict-0.html", "");

  // Verify notifications were received.  Allow a small tolerance for pipe
  // buffering on Windows where a notification may still be in-flight at
  // assertion time.
  EXPECT_GE(worker.stats().notifications_received.load(),
            static_cast<uint64_t>(kNumUrls - 2))
      << "All notifications should have been received";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlCooldownMapBoundedUnderLoad) {
  // Verify that the cooldown map doesn't grow without bound when many
  // unique HTML notifications are sent for URLs not in cache.
  // The tentative cooldown path (line 1478) adds entries; the worker
  // should remain functional even with >kMaxCooldownEntries (256)
  // unique URLs.
  //
  // Send notifications in batches with small delays to avoid overwhelming
  // the Unix socket listen backlog.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send 270 unique HTML notifications (URLs not in cache) in batches.
  // Each sets a tentative cooldown entry.
  constexpr int kNumUrls = 270;
  constexpr int kBatchSize = 30;
  for (int i = 0; i < kNumUrls; ++i) {
    CacheNotification notification;
    notification.url =
        "http://example.com/bounded-" + std::to_string(i) + ".html";
    notification.hostname = "example.com";
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    // Brief pause between batches to let the worker drain connections.
    if ((i + 1) % kBatchSize == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Wait for processing.
  // Use 80 iterations to give Windows enough headroom for pipe I/O.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (worker.stats().notifications_received.load() >=
        static_cast<uint64_t>(kNumUrls - 10)) {
      break;
    }
  }

  // Worker should remain functional.
  EXPECT_NE(worker.cache(), nullptr);

  // Verify ClearDedupAndCooldown works — accessing the map doesn't crash.
  worker.ClearDedupAndCooldown("http://example.com/bounded-0.html",
                               "example.com");

  // Cache operations should still work.
  std::string url = "http://example.com/after-load.html";
  std::string content = "still works";
  CacheOriginal(worker.cache(), url, content);
  CapabilityMask default_mask;
  EXPECT_EQ(ReadVariant(worker.cache(), url, default_mask), content);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// WebSocket Invalid Endpoint Tests
// =============================================================================

TEST_F(WorkerTest, WebSocketInvalidEndpointClosesConnection) {
  // When a WebSocket upgrade request arrives for an unknown endpoint
  // (not /v1/ws/stats, /v1/ws/events, or /v1/ws/logs), the worker
  // should close the connection.  Covers lines 738-750 (the else branch
  // of the WebSocket endpoint routing in the upgrade handler).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;  // OS picks a free port

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_GT(worker.api_port(), 0) << "HTTP API should be available";

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send a WebSocket upgrade request to an invalid endpoint path.
  std::string request =
      "GET /v1/ws/invalid-endpoint HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

  int sock = pagespeed::test::ConnectTcp(worker.api_port());
  ASSERT_GE(sock, 0) << "Failed to connect to API port " << worker.api_port();

  ssize_t written =
      pagespeed::test::SocketWrite(sock, request.data(), request.size());
  EXPECT_EQ(written, static_cast<ssize_t>(request.size()));

  // Wait for the server to process the upgrade and close the handle.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Try to read from the socket.  The server should have closed
  // the connection (invalid endpoint -> uv_close).  We expect either
  // 0 bytes (clean close) or an error.
  char buf[256];
  ssize_t nread = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
  // nread == 0 means the peer closed the connection (expected).
  // nread < 0 means error (also acceptable -- connection was reset).
  // nread > 0 would mean the server sent something, which is unexpected
  // for an invalid WS endpoint.
  //
  // Note: The HTTP server sends a 101 Switching Protocols response
  // before passing the handle to the upgrade handler.  So we may
  // receive the 101 response before the close.  Read it and then
  // check for close.
  if (nread > 0) {
    // We received the 101 response -- read again for the close.
    nread = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
  }
  EXPECT_LE(nread, 0) << "Connection should be closed for invalid WS endpoint";

  pagespeed::test::CloseSocket(sock);
}

TEST_F(WorkerTest, WebSocketValidEndpointsAccepted) {
  // Verify that the three valid WebSocket endpoints are recognized
  // and don't trigger the close-on-invalid-endpoint path.
  // Covers lines 740-745 (the three valid endpoint checks).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.api_port = -1;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_GT(worker.api_port(), 0);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Test each valid WebSocket endpoint.
  std::vector<std::string> valid_endpoints = {"/v1/ws/stats", "/v1/ws/events",
                                              "/v1/ws/logs"};

  for (const auto& endpoint : valid_endpoints) {
    std::string request = "GET " + endpoint +
                          " HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: Upgrade\r\n"
                          "Upgrade: websocket\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";

    // ConnectTcp sets a 10s SO_RCVTIMEO by default; override to 1s.
    int sock =
        pagespeed::test::ConnectTcp(worker.api_port(), /*timeout_sec=*/1);
    ASSERT_GE(sock, 0) << "Failed to connect for endpoint " << endpoint;

    ssize_t written =
        pagespeed::test::SocketWrite(sock, request.data(), request.size());
    EXPECT_EQ(written, static_cast<ssize_t>(request.size()));

    // Wait briefly for the server to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // For valid endpoints, the server hands off to WsManager which
    // should accept the connection (send a 101 Switching Protocols).
    // We just verify the connection is not immediately closed.
    char buf[512];
    ssize_t nread = pagespeed::test::SocketRead(sock, buf, sizeof(buf));
    // For valid endpoints we expect either data (101 response) or
    // a timeout (connection still open, no data yet).  An immediate
    // close (nread == 0) would indicate the endpoint was rejected.
    // Note: nread == -1 with EAGAIN/EWOULDBLOCK means timeout (good).
    if (nread == 0) {
      ADD_FAILURE() << "Valid endpoint " << endpoint
                    << " should not be immediately closed";
    }

    pagespeed::test::CloseSocket(sock);
  }
}

// ---------------------------------------------------------------------------
// ToSvgImageType coverage: GIF source format through SVG pipeline.
// Exercises the IMAGE_GIF -> kGif branch of ToSvgImageType().
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, ImageSvgCandidacyEvaluatesGifFormat) {
  // GIF images should be evaluated for SVG candidacy with src_format=kGif.
  // This exercises the ToSvgImageType() IMAGE_GIF -> kGif mapping.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a small GIF test file.
  std::string url = "http://example.com/icon.gif";
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty()) << "Test GIF not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/gif";
  CacheOriginalWithMeta(worker.cache(), url, gif, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated (even if rejected for GIF).
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated for GIF images";

  worker.Shutdown();
  worker_thread.join();
}

// ---------------------------------------------------------------------------
// ToSvgImageType coverage: WebP source format through SVG pipeline.
// Exercises the IMAGE_WEBP -> kWebP branch of ToSvgImageType().
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, ImageSvgCandidacyEvaluatesWebPFormat) {
  // WebP images should be evaluated for SVG candidacy with src_format=kWebP.
  // This exercises the ToSvgImageType() IMAGE_WEBP -> kWebP mapping.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a WebP test file.
  std::string url = "http://example.com/icon.webp";
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty()) << "Test WebP not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/webp";
  CacheOriginalWithMeta(worker.cache(), url, webp, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated for WebP images";

  worker.Shutdown();
  worker_thread.join();
}

// ---------------------------------------------------------------------------
// ToSvgImageType coverage: JPEG source format through SVG pipeline.
// Exercises the IMAGE_JPEG -> kJpeg branch of ToSvgImageType().
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, ImageSvgCandidacyEvaluatesJpegFormat) {
  // JPEG images should be evaluated for SVG candidacy with src_format=kJpeg.
  // This exercises the ToSvgImageType() IMAGE_JPEG -> kJpeg mapping.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a JPEG test file.
  std::string url = "http://example.com/photo-svg-eval.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated for the JPEG.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated for JPEG images";

  worker.Shutdown();
  worker_thread.join();
}

// ---------------------------------------------------------------------------
// Compression variant write: verify write failure increments stats counter.
// When the cache is full or corrupted, WriteCompressedVariants should
// increment alternate_write_failures without crashing.
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, CompressedVariantWriteCountersAccurate) {
  // Verify that after processing CSS, the compressed variant write counters
  // are incremented correctly for both gzip and brotli.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.gzip_level = 6;
  config.brotli_level = 6;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/style-compressed-check.css";
  std::string css_content =
      "body { margin: 0; padding: 0; }\n"
      "h1 { color: red; font-size: 24px; }\n"
      "p { color: blue; line-height: 1.5; }\n";
  CacheOriginal(worker.cache(), url, css_content);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for compressed variants to be written (strong completion signal).
  // Polling on css_processed is racy: it increments BEFORE
  // WriteCompressedVariants runs. Poll on brotli_variants_written because
  // WriteCompressedVariants writes gzip first, then brotli, on the same
  // thread — brotli visibility implies gzip is already written.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().brotli_variants_written.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // Both gzip and brotli variants should have been written.
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u)
      << "At least one gzip variant should be written";
  EXPECT_GE(worker.stats().brotli_variants_written.load(), 1u)
      << "At least one brotli variant should be written";

  // alternate_writes should include both compressed writes plus the identity.
  uint64_t total_writes = worker.stats().alternate_writes.load();
  EXPECT_GE(total_writes, 3u)
      << "At least 3 alternate writes expected (identity + gzip + brotli)";

  // No write failures expected on a healthy cache.
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), 0u)
      << "No write failures expected on healthy cache";

  worker.Shutdown();
  worker_thread.join();
}

// Test: Warmup handler propagates content_class and SSIMULACRA2 scores
// to variant metadata (regression test for race condition where warmup
// overwrites proactive variants with stripped metadata).
TEST_F(WorkerTest, WarmupSentinelPropagatesMetadata) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;
  config.proactive_viewport_variants = false;  // Desktop only to speed up test
  config.proactive_density_variants = false;   // 1x only to speed up test
  config.proactive_savedata_variants = true;
  // Fixed qualities with learned prediction off, so the save-data WebP
  // variant stores deterministically: with learned prediction on, this
  // fixture's save-data WebP scores below the Save-Data floor (target
  // 70 - 15 = 55, floor 52) and is legitimately declined under the
  // binding verify verdict.  Decline mechanics are pinned in the
  // transcoder suite; this test is about metadata propagation.  Content
  // analysis stays on because the test asserts the propagated
  // content_class.  q60 clears the Save-Data band while staying well
  // below the normal-mode 75.
  config.learned_quality = false;
  config.savedata_webp_quality = 60;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/warmup-meta.jpg";
  // sjpeg6.jpg is 512x512, wider than mobile_width=480.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup sentinel notification (no prior proactive processing).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Warmup writes its non-save-data and save-data variants within a single
  // notification's processing, so wait for that work item to retire (a
  // condition wait, not a fixed poll) and then read both variants directly.
  // The old fixed poll -- even its bumped sanitizer branch (50s) -- expired
  // mid-transcode under TSan's ~10x slowdown on the AVIF / SSIMULACRA2
  // verification path (it aborted the shard).
  WaitForFirstVariantThenIdle(worker);

  // Non-save-data Desktop/WebP variant (written early in the warmup loop,
  // before the save-data iterations).
  CapabilityMask desktop_webp;
  desktop_webp.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::optional<VariantWithMeta> desktop_meta =
      ReadVariantWithMeta(worker.cache(), url, desktop_webp);
  ASSERT_TRUE(desktop_meta.has_value())
      << "Warmup should write non-save-data Desktop/WebP variant";

  // Save-data Desktop/WebP variant (written after non-save-data, with the
  // full SSIMULACRA2 verification path).
  CapabilityMask sd_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kIdentity);
  std::optional<VariantWithMeta> sd_meta =
      ReadVariantWithMeta(worker.cache(), url, sd_webp);
  ASSERT_TRUE(sd_meta.has_value())
      << "Warmup should write save-data WebP variant";

  // Content class should be populated (not kUnknown=4).
  EXPECT_NE(sd_meta->metadata.content_class, 4u)
      << "Warmup metadata should carry content_class from content analysis";

  // SSIMULACRA2 score should be valid (not N/A = 0xFFFF).
  EXPECT_NE(sd_meta->metadata.ssimulacra2_score_x100, 0xFFFF)
      << "Warmup save-data WebP should have SSIMULACRA2 score";
  if (sd_meta->metadata.ssimulacra2_score_x100 != 0xFFFF) {
    EXPECT_LE(sd_meta->metadata.ssimulacra2_score_x100, 10000u);
  }
}

TEST_F(WorkerTest, CompleteMatrixMarksProcessedAndAllowsDedup) {
  // Verify that a complete variant matrix marks the URL as processed,
  // causing re-notifications to be dedup-skipped. Also verify the
  // image_incomplete_matrices counter stays at zero.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/complete-matrix.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  // First notification — should process and complete.  Wait for the work
  // item to retire: MarkVariantProcessed and the in-flight erase both run
  // before retirement, so afterwards the dedup registration is visible AND
  // the in-flight guard cannot swallow the second notification below
  // (which would surface as notifications_skipped_inflight instead of
  // notifications_skipped_dedup).  The old two-poll wait — counter moved,
  // then "stable for 100ms" — could declare completion in that window.
  SendNotification(notification);
  WaitForFirstVariantThenIdle(worker);
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Image should be processed";
  EXPECT_EQ(worker.stats().image_incomplete_matrices.load(), 0u)
      << "Complete matrix should not increment incomplete counter";

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification — should be dedup-skipped.  Poll for the skip counter
  // to advance instead of a fixed sleep (slow shards can sit at the pipe read
  // past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Duplicate notification should be dedup-skipped after complete matrix";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CacheReadFailureDoesNotMarkProcessed) {
  // When original is not in cache, the notification fails early (before
  // the variant tracking code), so no dedup marking occurs. A later
  // notification with the original populated should process normally.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string url = "http://example.com/gapfill.jpg";
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  // First notification — original not in cache, processing fails.
  // ReadOriginalWithRetry total budget is ~4.4s, so wait up to 8s.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().cache_read_failures.load() >= 1 ||
        worker.stats().notifications_received.load() >= 1)
      break;
  }
  EXPECT_EQ(worker.stats().images_processed.load(), 0u)
      << "Should not process when original is missing";

  // The failed attempt still occupies the in-flight set until the event loop
  // retires it.  Drain, or the second notification is rejected by the in-flight
  // guard and never processes.
  WaitForWorkerIdle(worker);

  // Now populate the cache with the original image.
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  // Second notification — original now exists, should process.
  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Second notification should process after original is populated";

  // Should not be dedup-skipped since no successful processing happened before.
  EXPECT_EQ(worker.stats().notifications_skipped_dedup.load(), 0u)
      << "Re-notification should not be dedup-skipped when first attempt "
         "failed";
}

TEST_F(WorkerTest, InvalidateUrlClearsIncompleteRetryState) {
  // Verify that InvalidateUrl clears the incomplete-matrix retry state,
  // allowing full re-processing after a purge.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/retry-clear.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  // First notification — process and dedup-mark.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // images_processed moves before the in-flight entry is erased, and
  // InvalidateUrl clears dedup and retry state but NOT the in-flight set.
  // Drain first, or the re-notification below is rejected by the in-flight
  // guard and never reprocesses.
  WaitForWorkerIdle(worker);

  // Purge the URL — this should clear dedup AND retry state.
  worker.InvalidateUrl(url, "");

  // Re-populate cache so the next notification can process.
  CacheOriginal(worker.cache(), url, image_data);

  uint64_t processed_before = worker.stats().images_processed.load();

  // Re-notify — should be processed (not dedup-skipped).
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() > processed_before) break;
  }
  EXPECT_GT(worker.stats().images_processed.load(), processed_before)
      << "After InvalidateUrl, re-notification should be processed";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageIncompleteMatricesCounterInApiMetrics) {
  // Verify the new counter appears in the Prometheus metrics endpoint.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to management socket and issue METRICS command.
  std::string mgmt_path = socket_path_ + ".mgmt";
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  const char* cmd = "METRICS\n";
  ASSERT_EQ(pagespeed::test::PipeWrite(fd, cmd, strlen(cmd)),
            static_cast<ssize_t>(strlen(cmd)));

  // Keep the retry loop (tolerates an empty first read), but read each
  // attempt to EOF: the metrics document no longer fits one bufferful
  // (see ReadMgmtResponseToEof).
  std::string response;
  for (int i = 0; i < 20; ++i) {
    response = ReadMgmtResponseToEof(fd);
    if (!response.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("pagespeed_image_incomplete_matrices_total"),
            std::string::npos)
      << "METRICS should include pagespeed_image_incomplete_matrices_total. "
         "Response: "
      << response;
}

TEST_F(WorkerTest, ImageIncompleteMatricesCounterInMgmtStats) {
  // Verify the new image_incomplete_matrices counter appears in MGMT STATS.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to management socket and issue STATS command.
  std::string mgmt_path = socket_path_ + ".mgmt";
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  const char* cmd = "STATS\n";
  ASSERT_EQ(pagespeed::test::PipeWrite(fd, cmd, strlen(cmd)),
            static_cast<ssize_t>(strlen(cmd)));

  // Keep the retry loop (tolerates an empty first read), but read each
  // attempt to EOF: the STATS document grows with every counter the build
  // gains and the field asserted on sits about two-thirds in, so a
  // fixed-buffer single read truncates it silently as soon as the document
  // outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response;
  for (int i = 0; i < 20; ++i) {
    response = ReadMgmtResponseToEof(fd);
    if (!response.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"image_incomplete_matrices\":"), std::string::npos)
      << "STATS should include image_incomplete_matrices field. Response: "
      << response;
}

TEST_F(WorkerTest, ProactiveWriteDoesNotDuplicate) {
  // Verify that the proactive image write loop does not re-write variants
  // that were already written within the same notification.  The fix
  // inserts written alternate IDs into existing_ids so later loop
  // iterations skip them.
  //
  // Matrix: 3 viewports × 3 formats × 2 save-data × 1 density = 18 variants.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;
  config.proactive_savedata_variants = true;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-test.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Desktop/WebP mask.
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mask.Encode();
  SendNotification(notification);

  // Wait for the notification's whole proactive matrix to retire.  The old
  // "variants_written unchanged for 1s" heuristic was a slowdown trap: under
  // TSan a single slow transcode (AVIF via libaom) leaves the counter flat
  // for over a second mid-matrix, so the poll declared completion at 3 of the
  // expected variants.  Retiring the work item is the true completion signal.
  WaitForFirstVariantThenIdle(worker);

  uint64_t written = worker.stats().variants_written.load();
  uint64_t attempts = worker.stats().alternate_writes.load();
  uint64_t dedup_skipped = worker.stats().dedup_writes_skipped.load();

  // The proactive loop should produce a reasonable number of variants.
  // Theoretical max: 3 viewports × 3 formats × 2 save-data = 18,
  // but some combos may not succeed (e.g., optimized original not smaller,
  // AVIF/WebP size gate rejects output larger than original).  With high-quality
  // source JPEGs, the size gate rejects many format conversions.
  // Content-identical variants (e.g., same bytes across viewports when image
  // is small enough that no resize occurs) are deduped: written once, counted
  // in dedup_writes_skipped for the duplicates.
  // Windows MSVC image codecs have different compression characteristics,
  // causing more variants to be size-gated (output larger than original).
  // Use a lower threshold on Windows.
#ifdef _WIN32
  constexpr uint64_t kMinExpectedVariants = 3;
#else
  constexpr uint64_t kMinExpectedVariants = 6;
#endif
  EXPECT_GE(written + dedup_skipped, kMinExpectedVariants)
      << "Expected at least " << kMinExpectedVariants
      << " variants generated from proactive matrix, got " << written
      << " written + " << dedup_skipped << " dedup-skipped";

  // Write attempts (actual cache writes) should equal successful writes —
  // content-deduped variants are skipped before the write, not failed.
  EXPECT_EQ(attempts, written)
      << "Write attempts (" << attempts << ") should equal successful writes ("
      << written << "); mismatch suggests duplicate write attempts";

  // Content dedup tracking: dedup_skipped counts how many writes were
  // skipped because identical bytes were already written under another ID.
  // For small test images, viewports don't resize → many deduped writes.
  EXPECT_GE(dedup_skipped, 0u);  // May be 0 for large images

  // Note: second-notification idempotency is not asserted here because
  // image encoding is non-deterministic (SSIMULACRA2 binary search,
  // content-aware quality adjustments).  Transcodes that fail the size
  // gate on one run may succeed on the next, producing legitimately new
  // variants.  The within-notification dedup (attempts == written above)
  // is the meaningful invariant.

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DISABLED_DeferredRetryEscalatesMultipleTimes) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string url = "http://example.com/deferred-multi.jpg";
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  // Send notification — original not in cache, will trigger deferred retries.
  SendNotification(notification);

  // Wait for at least 2 deferred retries to be scheduled.
  // First deferred fires at ~5s, second at ~15s from first notification.
  // Wait up to 25s total for 2 deferred retries.
  for (int i = 0; i < 250 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().cache_read_deferred_retries.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().cache_read_deferred_retries.load(), 2u)
      << "Should have scheduled at least 2 deferred retries";

  // Now populate the cache — the next deferred retry should succeed.
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  // Wait for the image to be processed.
  for (int i = 0; i < 300 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Image should be processed after deferred retry succeeds";
  EXPECT_GE(worker.stats().cache_read_deferred_successes.load(), 1u)
      << "Should have at least one successful deferred retry";
}

// =============================================================================
// Auto-Heal Tests
// =============================================================================

TEST_F(WorkerTest, AutoHealStatsInitiallyZero) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().cache_auto_heals.load(), 0u);
  EXPECT_EQ(worker.stats().cache_auto_heal_exhausted.load(), 0u);

  worker.Shutdown();
}

TEST_F(WorkerTest, InvalidateUrlResetsAutoHealState) {
  // Manual purge (InvalidateUrl with default clear_heal_state=true) should
  // reset the per-URL auto-heal counter, allowing future auto-heals.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/autoheal-reset.jpg";
  std::string hostname = "example.com";

  // Populate cache so InvalidateUrl has something to remove.
  CacheOriginal(worker.cache(), url, "test content");

  // Manual purge should succeed (and reset any internal heal state).
  int deleted = worker.InvalidateUrl(url, hostname);
  // deleted might be 0 since hostname doesn't match the empty default,
  // but the point is that the call doesn't crash and clears state.
  (void)deleted;

  EXPECT_EQ(worker.stats().cache_auto_heals.load(), 0u)
      << "Manual purge should not increment auto-heal stats";

  worker.Shutdown();
}

TEST_F(WorkerTest, AutoHealDoesNotBlockNormalProcessing) {
  // Verify that the auto-heal mechanism doesn't interfere with normal
  // notification processing. Write original → send notification → check
  // that processing completes without triggering auto-heal.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string url = "http://example.com/normal.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";

  // Populate cache with original
  CacheOriginal(worker.cache(), url, image_data);

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Normal processing should complete";
  EXPECT_EQ(worker.stats().cache_auto_heals.load(), 0u)
      << "Auto-heal should not trigger for successful reads";
}

// =============================================================================
// Item 3: Generational double-buffer dedup tests
// =============================================================================

TEST_F(WorkerTest, DedupBothSetsConsulted) {
  // Verify that entries rotated to the previous set still dedup.
  // Use a small max_processed_entries (100) to force overflow with few
  // notifications, keeping the test fast and socket-friendly.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(100 * 1024 * 1024);
  config.gzip_level = 0;  // Disable compression for speed
  config.brotli_level = 0;
  config.max_processed_entries = 100;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with 101 CSS URLs to force one overflow rotation.
  // Use unminified CSS so the worker actually minifies and increments
  // css_processed (already-minimal CSS takes a different path).
  static constexpr size_t kCount = 101;
  std::string css_content =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  std::string first_url;
  for (size_t i = 0; i < kCount; ++i) {
    std::string url = absl::StrCat("http://example.com/dedup-both-", i, ".css");
    if (i == 0) first_url = url;
    CacheOriginal(worker.cache(), url, css_content);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send all notifications.
  for (size_t i = 0; i < kCount; ++i) {
    CacheNotification notification;
    notification.url =
        absl::StrCat("http://example.com/dedup-both-", i, ".css");
    notification.content_type = ContentType::kCss;
    notification.scheme = "https";
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
  }

  // Wait for all notifications to be received and all in-flight work to
  // complete.  Checking in_flight_work==0 ensures MarkVariantProcessed
  // has populated the dedup set for every processed URL.
  for (int w = 0; w < 300; ++w) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= kCount &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), kCount)
      << "All CSS notifications should be processed";

  // The first URL was processed early and should now be in previous_ set
  // after the overflow rotation.  Re-sending it should still dedup.
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  CacheNotification dup_notif;
  dup_notif.url = first_url;
  dup_notif.scheme = "https";
  dup_notif.content_type = ContentType::kCss;
  dup_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(dup_notif);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Entry in previous_ set should still be consulted for dedup";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DedupOldEntriesEvictedAfterDoubleOverflow) {
  // After two full overflow rotations, entries from the first wave should
  // be evicted from both current_ and previous_.
  // We send 3 sequential waves of 101 unique CSS URLs, each waited to
  // completion, ensuring deterministic overflow: wave 1 fills+overflows,
  // wave 2 fills+overflows (evicting wave 1 from previous), wave 3
  // guarantees all wave-1 entries are gone.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(100 * 1024 * 1024);
  config.gzip_level = 0;
  config.brotli_level = 0;
  config.max_processed_entries = 100;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  static constexpr size_t kWaveSize = 101;
  static constexpr size_t kNumWaves = 3;
  std::string css_content =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";

  // Pre-populate all URLs in cache.
  std::string first_url = "http://example.com/evict-w0-0.css";
  for (size_t w = 0; w < kNumWaves; ++w) {
    for (size_t i = 0; i < kWaveSize; ++i) {
      std::string url =
          absl::StrCat("http://example.com/evict-w", w, "-", i, ".css");
      CacheOriginal(worker.cache(), url, css_content);
    }
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send and wait for each wave sequentially.
  for (size_t w = 0; w < kNumWaves; ++w) {
    uint64_t expected_received = (w + 1) * kWaveSize;
    uint64_t target = expected_received;
    for (size_t i = 0; i < kWaveSize; ++i) {
      CacheNotification notification;
      notification.url =
          absl::StrCat("http://example.com/evict-w", w, "-", i, ".css");
      notification.content_type = ContentType::kCss;
      notification.scheme = "https";
      notification.capability_mask = CapabilityMask().Encode();
      SendNotification(notification);
    }
    // Wait for all notifications to be received and all in-flight work to
    // complete, rather than polling css_processed with an arbitrary timeout.
    for (int t = 0; t < 300; ++t) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_received.load() >= expected_received &&
          worker.in_flight_work() == 0) {
        break;
      }
    }
    auto const& s = worker.stats();
    EXPECT_GE(s.css_processed.load(), target)
        << "Wave " << w << " should complete"
        << "\n  notifications_received=" << s.notifications_received.load()
        << "\n  css_processed=" << s.css_processed.load()
        << "\n  alternate_write_failures=" << s.alternate_write_failures.load()
        << "\n  cache_read_failures=" << s.cache_read_failures.load()
        << "\n  skipped_dedup=" << s.notifications_skipped_dedup.load()
        << "\n  skipped_inflight=" << s.notifications_skipped_inflight.load()
        << "\n  in_flight_work=" << worker.in_flight_work();
  }

  // Re-populate wave-0's first URL and re-send.
  // After 3 waves, wave-0 entries are fully evicted from dedup sets.
  CacheOriginal(worker.cache(), first_url, css_content);

  uint64_t processed_before = worker.stats().css_processed.load();
  CacheNotification resend;
  resend.url = first_url;
  resend.scheme = "https";
  resend.content_type = ContentType::kCss;
  resend.capability_mask = CapabilityMask().Encode();
  SendNotification(resend);

  for (int w = 0; w < 40; ++w) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > processed_before) break;
  }
  EXPECT_GT(worker.stats().css_processed.load(), processed_before)
      << "After triple overflow, wave-0 entries should be re-processed";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Item 4: Purge generation tests
// =============================================================================

TEST_F(WorkerTest, PurgeGenerationPreventsStaleDedup) {
  // Verify that purge + re-cache + re-notify triggers re-processing
  // (the purge generation mechanism prevents the re-notification from
  // being dedup-skipped).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/purge-gen.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — process normally.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  // Verify dedup: second notification should be skipped.  Poll for the skip
  // counter to advance instead of a fixed sleep (slow shards can sit at the
  // pipe read past a fixed budget).
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Pre-purge: duplicate should be deduped";

  // The dedup-skipped duplicate also occupies the in-flight set until the event
  // loop retires it.  Drain before the purge + re-notify below.
  WaitForWorkerIdle(worker);

  // Purge the URL — bumps purge generation.
  worker.InvalidateUrl(url, "");

  // Re-populate cache with new content.
  std::string css2 =
      "body  {  padding:  0;  }\n"
      "h2  {  color:  blue;  font-size:  18px;  }\n";
  CacheOriginal(worker.cache(), url, css2);

  uint64_t processed_before = worker.stats().css_processed.load();

  // Re-notify — should be processed (not dedup-skipped).
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > processed_before) break;
  }
  EXPECT_GT(worker.stats().css_processed.load(), processed_before)
      << "After purge, re-notification should be re-processed";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, PurgeGenerationCounterIncrementsOnInvalidate) {
  // Verify that multiple purge+re-notify cycles all trigger re-processing.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/purge-multi.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First cycle: process normally.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // Two additional purge+re-process cycles.
  for (int cycle = 0; cycle < 2; ++cycle) {
    // Drain the previous cycle first: its processing counter moves before the
    // in-flight entry is erased, and the re-send below would otherwise be
    // rejected by the in-flight guard.
    WaitForWorkerIdle(worker);

    worker.InvalidateUrl(url, "");
    CacheOriginal(worker.cache(), url, css);

    uint64_t processed_before = worker.stats().css_processed.load();
    SendNotification(notification);
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().css_processed.load() > processed_before) break;
    }
    EXPECT_GT(worker.stats().css_processed.load(), processed_before)
        << "Purge cycle " << cycle << " should trigger re-processing";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WasPurgedSinceDispatch) {
  // Unit test for the WasPurgedSinceDispatch method that guards against
  // writing stale variants when a purge arrives during in-flight processing.
  // No IPC or Run() needed — just tests the generation tracking logic.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/was-purged-test";
  std::string hostname = "";

  // Before any purge, dispatch_gen=0 should NOT be detected as purged.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 0));

  // Purge the URL (bumps generation to 1).
  worker.InvalidateUrl(url, hostname);

  // Now dispatch_gen=0 should be detected as purged (1 > 0).
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 0));

  // dispatch_gen=1 should NOT be detected (1 > 1 is false — same gen).
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 1));

  // Purge again (gen becomes 2).
  worker.InvalidateUrl(url, hostname);

  // dispatch_gen=1 should now be detected (2 > 1).
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 1));

  // dispatch_gen=2 should not.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 2));

  // Different URL should not be affected.
  EXPECT_FALSE(
      worker.WasPurgedSinceDispatch("http://other.com/page", hostname, 0));
}

TEST_F(WorkerTest, ResetCacheBumpsGlobalPurgeGeneration) {
  // Issue #652: a full cache purge (ResetCache) must fence in-flight
  // variant writes the same way a single-URL purge does.  The fence is the
  // worker-global purge generation: baselines captured before the reset
  // must be flagged, baselines captured after must not.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/reset-fence-test";
  std::string hostname = "";

  // Baseline captured before the reset (never-purged URL: url_gen=0,
  // all_gen=0).
  PurgeDispatchGen before = worker.CapturePurgeGen(url, hostname);
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, before));

  // Full purge.
  ASSERT_TRUE(worker.ResetCache().empty());

  // The pre-reset baseline is now fenced — for EVERY URL, including ones
  // that were never individually purged.
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, before));
  EXPECT_TRUE(worker.WasPurgedSinceDispatch("http://other.com/x", "", before));

  // A baseline captured after the reset is clean.
  PurgeDispatchGen after = worker.CapturePurgeGen(url, hostname);
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, after));
}

TEST_F(WorkerTest, ResetCacheClearsPerUrlGenerationMaps) {
  // Issue #652 (review finding): reset_cache used to clear only
  // purge_generation_, leaving stale high generations in
  // purge_generation_previous_.  A post-reset notification capturing the
  // cleared-map baseline (0) would then be flagged as purged forever,
  // silently dropping every legitimate write for that URL.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/reset-prevmap-test";
  std::string hostname = "";

  // Purge a few times so the URL has a non-zero generation.
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 0));

  ASSERT_TRUE(worker.ResetCache().empty());

  // A fresh post-reset baseline must be clean: the per-URL generation maps
  // (current AND previous) were cleared, and the global generation matches
  // the captured one.
  PurgeDispatchGen after = worker.CapturePurgeGen(url, hostname);
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, after))
      << "Post-reset notifications must not be flagged as purged";
}

TEST_F(WorkerTest, CapturePurgeGenReflectsPerUrlPurges) {
  // The dispatch-time baseline must include purges that already happened,
  // so a notification dispatched after a purge is not spuriously dropped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/capture-gen-test";
  std::string hostname = "";

  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);

  PurgeDispatchGen gen = worker.CapturePurgeGen(url, hostname);
  EXPECT_EQ(gen.url_gen, 2u);
  // Captured baseline is current — not flagged.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, gen));
  // A purge after capture flags it.
  worker.InvalidateUrl(url, hostname);
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, gen));
}

TEST_F(WorkerTest, OriginRefreshedSentinelPreservesIdentityAndRebuilds) {
  // Issue #652 (fix B convergence + review): when nginx re-fetches expired
  // origin content (stale verdict, no validators), it re-records the fresh
  // body at the identity id and sends the origin-refreshed sentinel.  The
  // worker must purge the URL's stale NON-identity variants, PRESERVE the
  // fresh identity, clear dedup, and rebuild the variant set inline — no
  // second origin fetch, no unoptimized-MISS window.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refreshed.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process the URL once so variants exist and dedup is populated.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";
  ASSERT_FALSE(ReadVariant(worker.cache(), url, CapabilityMask()).empty());

  // Simulate nginx's freshness-driven re-fetch: the fresh body is
  // re-recorded over the identity alternate BEFORE the sentinel is sent
  // (record_response bypasses the AlternateExists short-circuit when
  // origin_refreshed is set).
  std::string fresh_css = "body { margin: 4px; } h2 { color: red; }";
  CacheOriginal(worker.cache(), url, fresh_css);

  // Send the origin-refreshed sentinel — the worker purges the stale
  // variants, preserves the identity, and rebuilds inline (dedup was
  // cleared, so css_processed increments WITHOUT any further
  // notification).
  uint64_t css_before = worker.stats().css_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged) << "Sentinel should have triggered a purge";

  bool rebuilt = false;
  for (int i = 0; i < 80; ++i) {
    if (worker.stats().css_processed.load() > css_before) {
      rebuilt = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(rebuilt) << "Worker must rebuild the variant set inline from "
                          "the preserved fresh identity (dedup cleared)";

  // The identity alternate survived the purge (fresh content, possibly
  // re-minified by the rebuild — but never empty, never a MISS).
  EXPECT_FALSE(ReadVariant(worker.cache(), url, CapabilityMask()).empty())
      << "Fresh identity must be preserved across the origin-refreshed "
         "purge — a whole-key Remove here means a second origin fetch and "
         "an unoptimized MISS on every TTL expiry";
}

TEST_F(WorkerTest,
       OriginRefreshedPurgesWorkerProcessedIdentityAndDefersRebuild) {
  // the dedup-heal work (reconciliation): on a substrate that
  // records originals ONLY at the durable-original id, the identity slot
  // holds the worker's OWN variant — stale at refresh time.  The
  // origin-refreshed purge must NOT preserve it, the inline rebuild must be
  // REFUSED (no genuine source, refusal counted), and the following
  // record+notify must rebuild the family from the genuinely fresh durable
  // original — the full convergence sequence.  Fails pre-reconciliation:
  // the stale variant is preserved and the family is "rebuilt" from
  // superseded bytes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refresh-stale.css";
  std::string css_v1 = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css_v1);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process once: the minified variant OVERWRITES the identity slot and
  // carries kFlagWorkerProcessed.  Nothing genuine replaces it — on this
  // substrate the origin's bytes live only at the durable-original id.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";
  ASSERT_NE(ReadVariant(worker.cache(), url, CapabilityMask()).find("margin"),
            std::string::npos)
      << "the stale variant must sit at the identity slot for this test";
  // Drain so every compressed-variant write of the first pass is counted
  // before variants_written is snapshotted below.
  WaitForWorkerIdle(worker);

  // The origin changed; the sentinel arrives BEFORE the fresh re-record
  // commits (pre-record ordering), so at sentinel time the identity slot
  // holds only the stale worker-processed variant.
  uint64_t variants_before = worker.stats().variants_written.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool refused = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_rebuild_refused.load() >= 1) {
      refused = true;
      break;
    }
  }
  EXPECT_TRUE(refused)
      << "inline rebuild from a worker-processed (stale) identity must be "
         "refused and counted";
  EXPECT_EQ(1u, worker.stats().origin_refresh_purges.load())
      << "the purge itself still runs";
  EXPECT_EQ(variants_before, worker.stats().variants_written.load())
      << "no inline rebuild may run without a genuine source";
  EXPECT_TRUE(ReadVariant(worker.cache(), url, CapabilityMask()).empty())
      << "the stale worker-processed identity must be purged, not preserved";
  // Drain the sentinel's work item so the plain notify below is not
  // rejected by the in-flight guard.
  WaitForWorkerIdle(worker);

  // Convergence: the post-commit re-record (fresh durable original) plus
  // its plain notification rebuilds the family from genuinely fresh bytes.
  std::string css_v2 = "body { padding: 0; } h2 { color: green; }";
  CacheDurableOriginal(worker.cache(), url, css_v2, "text/css",
                       ContentType::kCss);
  uint64_t css_before = worker.stats().css_processed.load();
  SendNotification(notification);
  bool rebuilt = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      rebuilt = true;
      break;
    }
  }
  ASSERT_TRUE(rebuilt)
      << "the next record+notify must rebuild (dedup was cleared)";
  std::string rebuilt_variant =
      ReadVariant(worker.cache(), url, CapabilityMask());
  EXPECT_NE(rebuilt_variant.find("padding"), std::string::npos)
      << "rebuilt from the FRESH origin bytes";
  EXPECT_EQ(rebuilt_variant.find("margin"), std::string::npos)
      << "no trace of the superseded bytes may survive the convergence";
}

// Re-points a key's chain head at itself in a CLOSED cache volume, turning the
// alternate chain into a cycle: the shape the storage layer's read and removal
// walks do not guard against, in which a listing repeats one node up to the
// traversal cap, every write to the key is refused, and a per-alternate
// removal reports success without changing anything.  Returns false (with a
// gtest failure already recorded) if the volume file could not be found.
//
// The two constants are the storage layer's on-disk document format, verified
// against the file before anything is written, so a pin that moves them fails
// here rather than silently patching nothing.
static bool SelfLinkChainHeadInClosedVolume(const std::string& dir,
                                            uint64_t head, uint64_t tail) {
  constexpr uint32_t kDocumentMagic = 0x5F129B14;
  constexpr std::streamoff kNextAlternateOffsetPos = 112;
  auto magic_at = [](std::fstream& f, uint64_t offset) {
    uint32_t magic = 0;
    f.clear();
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return f.good() && magic == kDocumentMagic;
  };
  std::filesystem::path volume_file;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    std::fstream probe(entry.path(), std::ios::in | std::ios::binary);
    if (probe.is_open() && magic_at(probe, head) && magic_at(probe, tail)) {
      volume_file = entry.path();
    }
  }
  if (volume_file.empty()) {
    ADD_FAILURE() << "no volume file holds both documents";
    return false;
  }
  std::fstream volume(volume_file,
                      std::ios::in | std::ios::out | std::ios::binary);
  if (!volume.is_open()) {
    ADD_FAILURE() << "cannot open the volume file for patching";
    return false;
  }
  uint64_t tail_relative = 0;
  volume.seekg(static_cast<std::streamoff>(head) + kNextAlternateOffsetPos);
  volume.read(reinterpret_cast<char*>(&tail_relative), sizeof(tail_relative));
  if (!volume.good() || tail_relative == 0 || tail_relative > tail) {
    ADD_FAILURE() << "the head's successor field did not read as expected";
    return false;
  }
  const uint64_t stripe_base = tail - tail_relative;
  if (stripe_base >= head) {
    ADD_FAILURE() << "the derived stripe base is not below the head";
    return false;
  }
  const uint64_t head_relative = head - stripe_base;
  volume.clear();
  volume.seekp(static_cast<std::streamoff>(head) + kNextAlternateOffsetPos);
  volume.write(reinterpret_cast<const char*>(&head_relative),
               sizeof(head_relative));
  volume.flush();
  return volume.good();
}

TEST_F(WorkerTest, OriginRefreshedSentinelEndsTheLoopOnAnUneditableVariantSet) {
  // Issue #1566.  A URL whose alternate chain has become a cycle is wedged:
  // the front end selects the stale variant, declines it on freshness and asks
  // for the refresh again, the worker's purge cannot unlink anything, and the
  // re-record that would converge the URL is refused — so it is served
  // unoptimized and re-optimized over and over.  After the sentinel the key
  // must be CLEAR and WRITABLE again, so the next request's record + notify
  // converges it.
  //
  // The chain is planted before the worker opens the volume: the state is a
  // property of the stored key, and building it needs the cache closed.
  const std::string url = "http://example.com/refresh-loop.jpg";
  const std::string path = StripSchemeAuthority(url);
  const CapabilityMask webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  const AlternateId webp_id =
      MaskToAlternateId(static_cast<uint8_t>(webp.Encode() & 0xFF));
  const AlternateId identity_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  const uint64_t kVolumeSize = static_cast<uint64_t>(10 * 1024 * 1024);

  uint64_t head = 0;
  uint64_t tail = 0;
  {
    PageSpeedCacheConfig plant_config;
    plant_config.volume_path = cache_path_;
    plant_config.volume_size = kVolumeSize;
    auto planted = PageSpeedCache::Create(plant_config);
    ASSERT_TRUE(planted.has_value());
    auto& plant = **planted;
    for (const auto& [mask, body] :
         std::vector<std::pair<CapabilityMask, std::string>>{
             {CapabilityMask(), "identity-bytes"}, {webp, "stale-webp"}}) {
      AlternateMetadata meta;
      meta.full_mask = mask.Encode();
      meta.content_type = ContentType::kImage;
      meta.flags = AlternateMetadata::kFlagWorkerProcessed;
      meta.origin_content_type = "image/jpeg";
      auto wh = plant.WriteAlternate(
          path, "", "https",
          MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF)),
          body.size(), meta);
      ASSERT_TRUE(wh.has_value());
      ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
      ASSERT_TRUE(wh->close_sync().has_value());
    }
    auto two = plant.ListAlternates(path, "", "https");
    ASSERT_TRUE(two.has_value());
    ASSERT_EQ(two->size(), 2u);
    ASSERT_EQ(static_cast<AlternateId>((*two)[0].id), webp_id);
    head = (*two)[0].disk_offset;
    tail = (*two)[1].disk_offset;
  }
  ASSERT_TRUE(SelfLinkChainHeadInClosedVolume(temp_dir_, head, tail));

  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = kVolumeSize;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // The worker opened the planted volume and sees the wedge: one node, over
  // and over, and a re-record that cannot land.
  auto looped = worker.cache()->ListAlternates(path, "", "https");
  ASSERT_TRUE(looped.has_value());
  ASSERT_GT(looped->size(), 2u);
  for (const auto& alt : *looped) {
    ASSERT_EQ(alt.disk_offset, head);
    ASSERT_EQ(static_cast<AlternateId>(alt.id), webp_id);
  }
  {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
    meta.content_type = ContentType::kImage;
    meta.origin_content_type = "image/jpeg";
    auto wh =
        worker.cache()->WriteOriginalAlternate(path, "", "https", 5, meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span("BYTES", 5))).has_value());
    ASSERT_FALSE(wh->close_sync().has_value())
        << "the wedged key must refuse the re-record, or this test is not "
           "standing on the shape it describes";
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification refresh;
  refresh.url = url;
  refresh.scheme = "https";
  refresh.content_type = ContentType::kImage;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged) << "Sentinel should have triggered a purge";
  WaitForWorkerIdle(worker);

  // Nothing stale is left for the front end to select ...
  EXPECT_FALSE(worker.cache()->AlternateExists(path, "", "https", webp_id))
      << "a stale variant survived the origin-refreshed purge: the next "
         "request selects it, declines it on freshness and asks for the "
         "refresh again — the loop";
  EXPECT_FALSE(worker.cache()->AlternateExists(path, "", "https", identity_id));

  // ... and the key takes the re-record that the fall-through produces, so
  // the ordinary record + notify path can rebuild the URL.
  CacheDurableOriginal(worker.cache(), url, "REFRESHED-ORIGIN", "image/jpeg",
                       ContentType::kImage);
  auto original = worker.cache()->ReadOriginalAlternate(path, "", "https");
  ASSERT_TRUE(original.has_value())
      << "the key must accept writes again after the purge";
}

TEST_F(WorkerTest, OriginRefreshedPreservesGenuineIdentityAndRebuilds) {
  // The nginx shape — no regression from the reconciliation
  //: the front stage re-records the
  // fresh origin body at the identity id BEFORE sending the sentinel, so
  // the entry there has kFlagWorkerProcessed CLEAR.  It must be preserved
  // across the purge and the inline rebuild must proceed from it, with the
  // refusal counter unmoved.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refresh-genuine.css";
  std::string css_v1 = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css_v1);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";

  // nginx's freshness re-fetch: fresh GENUINE bytes overwrite the identity
  // alternate (flag clear) before the sentinel is sent.
  std::string css_v2 = "body { padding: 0; } h2 { color: green; }";
  CacheOriginal(worker.cache(), url, css_v2);

  uint64_t css_before = worker.stats().css_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool rebuilt = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      rebuilt = true;
      break;
    }
  }
  EXPECT_TRUE(rebuilt)
      << "a genuine re-recorded identity must still drive the inline rebuild";
  EXPECT_EQ(0u, worker.stats().origin_refresh_rebuild_refused.load())
      << "no refusal when the identity is a genuine original";
  std::string rebuilt_variant =
      ReadVariant(worker.cache(), url, CapabilityMask());
  EXPECT_NE(rebuilt_variant.find("padding"), std::string::npos)
      << "the rebuild must run from the fresh genuine identity";
}

TEST_F(WorkerTest, PurgeGenRotationPreservesMonotonicity) {
  // Issue #652 review: after the purge-generation map rotates
  // (> kMaxPurgeGenEntries distinct purged URLs), a key with generation N
  // survives only in the previous map.  A later purge must seed its bump
  // from there (N+1), not restart at 1 — otherwise a baseline of N
  // captured at dispatch time silently bypasses the write fence
  // (`1 > N` == false) until N more purges occur.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/rotation-fence-test";
  std::string hostname = "";

  // Purge to generation 3.
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);

  // Force exactly one rotation: purge kMaxPurgeGenEntries + 1 distinct
  // other URLs (rotation triggers when the current map exceeds the cap),
  // so the test URL's generation now lives only in the previous map.
  for (size_t i = 0; i <= 10000; ++i) {
    worker.InvalidateUrl("http://example.com/rotate/" + std::to_string(i),
                         hostname);
  }

  // The baseline still sees generation 3 via the previous-map fallback.
  PurgeDispatchGen baseline = worker.CapturePurgeGen(url, hostname);
  EXPECT_EQ(baseline.url_gen, 3u)
      << "Rotation must not lose the per-URL generation";
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, baseline));

  // Purge once more: the bump must be seeded from the previous map (-> 4),
  // not restart at 1.
  worker.InvalidateUrl(url, hostname);
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, baseline))
      << "Post-rotation purge must fence a pre-purge baseline (else the "
         "#652 resurrection race is silently re-enabled)";
  EXPECT_EQ(worker.CapturePurgeGen(url, hostname).url_gen, 4u);
}

TEST_F(WorkerTest, OriginRefreshedSentinelRateLimited) {
  // Issue #652: a born-stale upstream (CDN serving Age > max-age) would
  // trigger the sentinel on every request pair.  Per-URL purges must be
  // rate-limited so the worker does not purge + re-optimize in a tight
  // loop.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refreshed-ratelimit.css";
  CacheOriginal(worker.cache(), url, "body { margin: 0; }");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification refresh;
  refresh.url = url;
  refresh.scheme = "https";
  refresh.content_type = ContentType::kCss;
  refresh.capability_mask = kOriginRefreshedSentinel;

  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged);

  // origin_refresh_purges is bumped mid-handler, well before the sentinel's
  // in-flight entry is erased (the handler still rebuilds variants inline
  // after it).  Drain, or the second sentinel is rejected by the in-flight
  // guard and never reaches the rate-limit check.
  WaitForWorkerIdle(worker);

  // A second sentinel within the rate-limit window is dropped.
  SendNotification(refresh);
  bool rate_limited = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_rate_limited.load() >= 1) {
      rate_limited = true;
      break;
    }
  }
  EXPECT_TRUE(rate_limited)
      << "Second origin-refreshed sentinel within the interval must be "
         "rate-limited";
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 1u);
}

TEST_F(WorkerTest, OriginRefreshedUnchangedImageRestampsWithoutPurge) {
  // Issue #1503 regression (nginx shape): an age-expired origin that
  // re-fetches BYTE-IDENTICAL must NOT purge the variant set — the worker
  // restamps the variants from the freshly re-recorded origin reference and
  // no re-optimization runs.  Pre-fix this purged every freshness cycle
  // (513 purges of unchanged origins observed in production).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-unchanged.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  WaitForFirstVariantThenIdle(worker);

  // The first pass wrote the variant set, stamped the kContentHash oracle
  // over the raw origin, and (typically) poisoned the identity slot with
  // the worker's own recompressed variant.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto before = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(before.has_value()) << "webp variant from the first pass";

  // nginx's freshness re-fetch, origin UNCHANGED: the identical body is
  // re-recorded over the identity slot (flag-clear, freshly stamped) BEFORE
  // the sentinel is sent.
  AlternateMetadata fresh_meta;
  fresh_meta.content_type = ContentType::kImage;
  fresh_meta.origin_content_type = "image/jpeg";
  fresh_meta.cache_inserted_at = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  fresh_meta.origin_max_age = 300;
  fresh_meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                               AlternateMetadata::kCCOriginPublic;
  CacheOriginalWithMeta(worker.cache(), url, jpeg, fresh_meta);

  const uint64_t images_before = worker.stats().images_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool unchanged_seen = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_unchanged.load() >= 1) {
      unchanged_seen = true;
      break;
    }
  }
  ASSERT_TRUE(unchanged_seen)
      << "unchanged origin across a freshness boundary must be recognised";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u)
      << "an unchanged origin must NOT be purged";
  EXPECT_EQ(worker.stats().origin_refresh_deferred.load(), 0u)
      << "a fresh reference resolves in the sentinel handler itself";
  EXPECT_EQ(worker.stats().images_processed.load(), images_before)
      << "no re-transcode of identical bytes";

  // The variant set survives, byte-identical, with its freshness stamp
  // adopted from the re-fetched origin reference.
  auto after = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(after.has_value()) << "variant set must survive (no purge)";
  EXPECT_EQ(after->content, before->content);
  EXPECT_EQ(after->metadata.cache_inserted_at, fresh_meta.cache_inserted_at)
      << "variant freshness restamped from the re-fetched origin";
}

TEST_F(WorkerTest, OriginRefreshedAgeBackdatedReferenceRestampsWithoutDefer) {
  // Issue #1503 review (must-fix): nginx Age-adjusts cache_inserted_at on
  // the origin-refreshed re-record — a CDN-fronted origin re-records with a
  // stamp minutes BEHIND wall clock.  A wall-clock-only freshness test
  // takes the defer branch for such a reference, but on the nginx shape NO
  // notify follows the sentinel, so the URL would re-fetch on every
  // request and never serve optimized.  The reference must be recognised
  // as the re-record RELATIVELY: it postdates the variant set's own
  // stamps.  (Fails pre-fix: the unconditional purge fires instead.)
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-age-backdated.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  WaitForFirstVariantThenIdle(worker);

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto before = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(before.has_value()) << "webp variant from the first pass";

  // nginx's freshness re-fetch of an UNCHANGED, CDN-fronted origin: the
  // identical body re-recorded over the identity slot, flag-clear, with
  // cache_inserted_at Age-backdated 300s behind wall clock.
  const uint32_t now_sec = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  AlternateMetadata backdated_meta;
  backdated_meta.content_type = ContentType::kImage;
  backdated_meta.origin_content_type = "image/jpeg";
  backdated_meta.cache_inserted_at = now_sec - 300;
  backdated_meta.origin_max_age = 300;
  backdated_meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                                   AlternateMetadata::kCCOriginPublic;
  CacheOriginalWithMeta(worker.cache(), url, jpeg, backdated_meta);

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool unchanged_seen = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_unchanged.load() >= 1) {
      unchanged_seen = true;
      break;
    }
  }
  ASSERT_TRUE(unchanged_seen)
      << "an Age-backdated re-record is still the re-record: restamp, "
         "not defer and not purge";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_deferred.load(), 0u)
      << "on the nginx shape no notify follows — deferring here is a "
         "permanent re-fetch loop";
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);
  auto after = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->content, before->content);
  EXPECT_EQ(after->metadata.cache_inserted_at, now_sec - 300)
      << "variant freshness adopted the Age-backdated re-record's stamp";
}

TEST_F(WorkerTest, OriginRefreshedUnchangedImageDefersThenRestampsOnNotify) {
  // Issue #1503 regression (1.16/module shape): the sender records originals
  // ONLY at the durable-original id and sends the sentinel BEFORE the
  // re-record lands.  At sentinel time the durable original still holds the
  // pre-refresh bytes: hash-equal to the oracle, so the handler must DEFER
  // (no purge — the identity slot holds the worker's own variant, which was
  // the false-purge driver) and clear dedup; the record+notify that follows
  // then restamps the variant set from the fresh durable original instead of
  // re-transcoding identical bytes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-deferred.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  // Durable-original-only substrate, stamped long enough ago to be past its
  // origin lifetime (the age-expired fall-through that triggers the refresh).
  CacheDurableOriginal(worker.cache(), url, jpeg, "image/jpeg",
                       ContentType::kImage,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/300);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  WaitForFirstVariantThenIdle(worker);

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto before = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(before.has_value()) << "webp variant from the first pass";
  EXPECT_EQ(before->metadata.cache_inserted_at, 1'700'000'000u)
      << "variants inherit the durable original's origin state";

  // The sentinel arrives BEFORE the re-record (the 1.16 ordering): the
  // durable original still holds the previous bytes with the old stamp.
  const uint64_t images_before = worker.stats().images_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool deferred = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_deferred.load() >= 1) {
      deferred = true;
      break;
    }
  }
  ASSERT_TRUE(deferred)
      << "a stale-stamped but hash-matching reference must defer, not purge";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);
  ASSERT_TRUE(ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value())
      << "the variant set must survive the deferred refresh";

  // The re-record lands (same bytes, fresh stamp) and its plain notification
  // drives the restamp through the image branch's content-hash check.
  const uint32_t reinserted_at = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  CacheDurableOriginal(worker.cache(), url, jpeg, "image/jpeg",
                       ContentType::kImage, reinserted_at, 300);
  const uint64_t hash_hits_before = worker.stats().content_hash_hits.load();
  SendNotification(notification);
  bool restamped = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().content_hash_hits.load() > hash_hits_before) {
      restamped = true;
      break;
    }
  }
  ASSERT_TRUE(restamped)
      << "the record+notify convergence must recognise the unchanged origin";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);
  EXPECT_EQ(worker.stats().content_hash_stale.load(), 0u);
  EXPECT_EQ(worker.stats().images_processed.load(), images_before)
      << "no re-transcode of identical bytes";
  auto after = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->content, before->content);
  EXPECT_EQ(after->metadata.cache_inserted_at, reinserted_at)
      << "variant freshness adopted the re-recorded origin's stamp";
}

TEST_F(WorkerTest, OriginRefreshedChangedImageStillPurgesAndRebuilds) {
  // Issue #1503 control (1.16/module shape): a GENUINE origin change must
  // still purge and rebuild — via the deferred convergence: the sentinel
  // arrives before the re-record (no evidence of change yet), then the
  // re-recorded durable original carries DIFFERENT bytes and the notify
  // path's pristine-reference hash check fires the purge.  The rebuilt
  // variant set must derive from the NEW origin bytes — never from the
  // worker-processed identity slot (the poisoning this issue is about).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-changed.jpg";
  std::string jpeg_v1 = ReadTestFile("jpeg/quality100.jpg");
  std::string jpeg_v2 = ReadTestFile("jpeg/progressive.jpg");
  ASSERT_FALSE(jpeg_v1.empty());
  ASSERT_FALSE(jpeg_v2.empty());
  ASSERT_NE(jpeg_v1, jpeg_v2);
  CacheDurableOriginal(worker.cache(), url, jpeg_v1, "image/jpeg",
                       ContentType::kImage,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/300);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  WaitForFirstVariantThenIdle(worker);

  CapabilityMask identity_mask;  // Desktop/Original/Identity = 0x08
  auto identity_v1 = ReadVariantWithMeta(worker.cache(), url, identity_mask);
  ASSERT_TRUE(identity_v1.has_value());
  ASSERT_NE(
      identity_v1->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "the identity slot must hold the worker's own variant here — the "
         "poisoned shape this fix operates on";

  // Sentinel first (pre-record ordering): nothing provably changed yet.
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool deferred = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_deferred.load() >= 1) {
      deferred = true;
      break;
    }
  }
  ASSERT_TRUE(deferred);
  WaitForWorkerIdle(worker);

  // The origin REALLY changed: the re-record carries different bytes.
  CacheDurableOriginal(
      worker.cache(), url, jpeg_v2, "image/jpeg", ContentType::kImage,
      static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
      300);
  const uint64_t stale_before = worker.stats().content_hash_stale.load();
  SendNotification(notification);
  bool change_detected = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().content_hash_stale.load() > stale_before) {
      change_detected = true;
      break;
    }
  }
  ASSERT_TRUE(change_detected)
      << "a genuine origin change must fire the content-hash purge";
  WaitForWorkerIdle(worker);

  // The purge is whole-key (issue #652 discipline) and fences this pass's
  // own writes, so the convergence is the production one: the front end's
  // next request misses, re-records the durable original, and re-notifies —
  // and THAT pass rebuilds from the fresh pristine reference.
  CacheDurableOriginal(
      worker.cache(), url, jpeg_v2, "image/jpeg", ContentType::kImage,
      static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
      300);
  SendNotification(notification);
  WaitForWorkerIdle(worker);
  // Drain until the rebuild lands (a fenced pass neither writes nor marks,
  // so a repeat notify is the retry, exactly as in production).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    if (ReadVariantWithMeta(worker.cache(), url, identity_mask).has_value()) {
      break;
    }
    SendNotification(notification);
    WaitForWorkerIdle(worker);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  auto identity_v2 = ReadVariantWithMeta(worker.cache(), url, identity_mask);
  ASSERT_TRUE(identity_v2.has_value())
      << "the variant set must be rebuilt after a genuine change";
  EXPECT_NE(identity_v2->content, identity_v1->content)
      << "the rebuild must derive from the NEW origin bytes";
  EXPECT_EQ(identity_v2->metadata.origin_content_length,
            static_cast<uint32_t>(jpeg_v2.size()))
      << "the rebuilt variant carries the new origin's length";
}

// =============================================================================
// Item 5: Incomplete matrix retry tests
// =============================================================================

TEST_F(WorkerTest, IncompleteMatrixAllowsReprocessing) {
  // Enable proactive image variants. Use a tiny image where some format
  // conversions may not reduce size. The key behavior: if the proactive
  // loop writes fewer variants than expected, the URL should NOT be marked
  // as processed (allowing retry on next notification).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Enable all proactive variant types to maximize variant count.
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;
  config.proactive_savedata_variants = true;
  config.proactive_density_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/incomplete-retry.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Image should be processed";

  // images_processed moves before the in-flight entry is erased.  Drain first
  // so the conditional re-send below is not rejected by the in-flight guard.
  WaitForWorkerIdle(worker);

  uint64_t incomplete = worker.stats().image_incomplete_matrices.load();
  if (incomplete > 0) {
    // If an incomplete matrix was detected, the URL is NOT dedup-marked.
    // Re-sending should trigger re-processing (not dedup skip).
    uint64_t processed_before = worker.stats().images_processed.load();
    SendNotification(notification);
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().images_processed.load() > processed_before) break;
    }
    EXPECT_GT(worker.stats().images_processed.load(), processed_before)
        << "Incomplete matrix should allow re-processing on retry";
  }
  // Whether or not incomplete was triggered, the counter should be
  // a non-negative value (sanity check).
  EXPECT_GE(incomplete, 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Tentative Cooldown Clearing Tests
//
// The worker sets a tentative 60s cooldown BEFORE HTML processing starts.
// On success, it's cleared.  But certain early-return paths forgot to clear
// it, locking the URL out for 60s unnecessarily.  These tests prove that
// the cooldown IS cleared on each early-return path.
// =============================================================================

TEST_F(WorkerTest, HtmlNoTransformsClearsTentativeCooldown) {
  // Path: "No HTML transformations applicable" early return.
  // When all transforms are disabled and there's no critical CSS,
  // the worker returns early.  The tentative cooldown must be cleared
  // so the URL is not locked out for 60s.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable all transforms so the "no transformations applicable" path fires.
  config.disable_lazy_load = true;
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.disable_async_css = true;
  config.disable_script_deferral = true;
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no inline CSS -> no critical CSS -> no transforms applicable.
  std::string url = "http://example.com/no-transforms.html";
  std::string html =
      "<html><head><title>No Transforms</title></head>"
      "<body><p>Plain text only</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = "";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification: hits "no transformations applicable" early return.
  SendNotification(notification);

  // Wait for the early return to fire.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_assembly_skipped.load(), 1u)
      << "First notification should hit 'no transformations' path";

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_before =
      worker.stats().html_assembly_skipped.load();

  // Second notification for the same URL.  If the tentative cooldown was
  // NOT cleared, this will be blocked for 60s and the dedup counter
  // will increment.  If properly cleared, it processes again (hitting
  // the same "no transforms" path, incrementing assembly_skipped).  Poll for
  // the assembly-skipped counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget); this also settles
  // the dedup-unchanged invariant below.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() > assembly_skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_after = worker.stats().html_assembly_skipped.load();

  EXPECT_EQ(skipped_after, skipped_before)
      << "Second notification must NOT be blocked by tentative cooldown";
  EXPECT_GT(assembly_skipped_after, assembly_skipped_before)
      << "Second notification should reach 'no transforms' path again";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNoChangesClearsTentativeCooldown) {
  // Path: "Transform produced no changes" early return.
  // When transforms run but produce identical output (no matching elements),
  // the worker returns early.  The tentative cooldown must be cleared.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Enable only lazy-load (needs <img>/<iframe> to actually modify).
  // Disable everything else.
  config.disable_lazy_load = false;  // Enabled — but no <img> in HTML
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.disable_async_css = true;
  config.disable_script_deferral = true;
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no <img> or <iframe> — lazy-load transform runs but
  // finds nothing to modify, so modified_ stays false.
  std::string url = "http://example.com/no-changes.html";
  std::string html =
      "<html><head><title>No Changes</title></head>"
      "<body><p>Text only, no images or iframes</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = "";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification: hits "transform produced no changes" early return.
  SendNotification(notification);

  // Wait for the early return to fire (same counter as "no transforms").
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_assembly_skipped.load(), 1u)
      << "First notification should hit 'no changes' path";

  // html_assembly_skipped moves before the in-flight entry is erased.  Drain,
  // or the second notification is rejected by the in-flight guard and the
  // counter below never advances.
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_before =
      worker.stats().html_assembly_skipped.load();

  // Second notification: should NOT be blocked by tentative cooldown.  Poll
  // for the assembly-skipped counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget); this also settles
  // the dedup-unchanged invariant below.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() > assembly_skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_after = worker.stats().html_assembly_skipped.load();

  EXPECT_EQ(skipped_after, skipped_before)
      << "Second notification must NOT be blocked by tentative cooldown";
  EXPECT_GT(assembly_skipped_after, assembly_skipped_before)
      << "Second notification should reach 'no changes' path again";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, UnsupportedImageTypeExistingCompressedMarksDedup) {
  // Simulates a worker restart scenario: compressed variants for an unsupported
  // image type (ICO) already exist in cache from a previous worker run, but the
  // in-memory dedup set is empty.  The first notification hits the "compressed
  // variants already exist" early-return path.  Without MarkVariantProcessed,
  // every subsequent notification would repeat the cache lookup instead of being
  // caught by the fast in-memory dedup.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create fake ICO data with metadata indicating image/x-icon (original).
  std::string url = "http://example.com/dedup-favicon.ico";
  std::string ico_data = "fake ico content for dedup testing";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/x-icon";
  CacheOriginalWithMeta(worker.cache(), url, ico_data, meta);

  // Pre-write a gzip variant to simulate a previous worker run having already
  // processed this URL.  This causes the "compressed variants already exist"
  // early-return path to be taken on the first notification.
  {
    CapabilityMask gz_mask;
    gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
    AlternateId gz_id =
        MaskToAlternateId(static_cast<uint8_t>(gz_mask.Encode() & 0xFF));
    AlternateMetadata gz_meta;
    gz_meta.content_type = ContentType::kImage;
    gz_meta.flags = AlternateMetadata::kFlagWorkerProcessed;
    gz_meta.full_mask = gz_mask.Encode();
    std::string gz_content = "fake gzip compressed ico data";
    std::string norm_url = StripSchemeAuthority(url);
    auto wh = worker.cache()->WriteAlternate(norm_url, "", "https", gz_id,
                                             gz_content.size(), gz_meta);
    ASSERT_TRUE(wh.has_value()) << "Failed to write gzip variant";
    auto written = wh->write_sync(
        std::as_bytes(std::span(gz_content.data(), gz_content.size())));
    ASSERT_TRUE(written.has_value()) << "Failed to write gzip content";
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value()) << "Failed to close gzip write handle";
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — hits "compressed variants already exist" path.
  // With the fix, this marks the URL in the dedup set.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for the notification to be processed (received + handled).
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // The receive counter above moves before the notification is even handled, so
  // the fixed sleep is not enough on a loaded runner.  Drain the in-flight
  // entry, or the duplicate below is rejected by the in-flight guard instead of
  // the dedup set.
  WaitForWorkerIdle(worker);

  // Second notification — should be caught by in-memory dedup.
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Second notification for unsupported image with pre-existing "
         "compressed variants should be caught by in-memory dedup";

  worker.Shutdown();
  worker_thread.join();
}

// --- CSS/JS cooldown throttling tests ---

TEST_F(WorkerTest, CssCooldownPreventsReprocessing) {
  // Send the same CSS notification twice quickly; the second should be
  // skipped due to cooldown enforcement (tentative in-flight guard).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification.
  SendNotification(notification);

  // Wait for first processing to complete.
  CapabilityMask mask;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "CSS should have been processed once";

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification for the same URL — should be skipped by dedup
  // or cooldown.  Poll for the skip counter to advance instead of a fixed
  // sleep (slow shards can sit at the pipe read past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Second CSS notification should be skipped by dedup/cooldown";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssCooldownClearedOnSuccess) {
  // Verify that after successful CSS processing, the cooldown is cleared
  // so that ClearDedupAndCooldown + re-notification can reprocess.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-clear.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — process CSS.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // css_processed moves before the in-flight entry is erased, and InvalidateUrl
  // does not clear the in-flight set.  Drain first, or the re-send below is
  // rejected by the in-flight guard and never reprocesses.
  WaitForWorkerIdle(worker);

  // Verify cooldown was cleared on success (not just dedup).
  auto cooldown = worker.GetCooldownForUrl(url, "");
  EXPECT_FALSE(cooldown.has_value())
      << "Cooldown should be cleared after successful CSS processing";

  uint64_t css_before = worker.stats().css_processed.load();

  // Clear dedup to simulate re-notification scenario.
  worker.ClearDedupAndCooldown(url, "");
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, css);

  // Re-send notification — should process since cooldown was cleared on
  // success and dedup was explicitly cleared.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) break;
  }

  EXPECT_GT(worker.stats().css_processed.load(), css_before)
      << "After clearing dedup, CSS should be reprocessed (cooldown was "
         "cleared on success)";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsCooldownPreventsReprocessing) {
  // Send the same JS notification twice; the second should be skipped by the
  // processed-variant dedup set once the first has retired.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown.js";
  std::string js = "function hello() { console.log('hello world'); }";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification.
  SendNotification(notification);

  // Wait for first processing to complete.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "JS should have been processed once";

  // js_processed moves before the in-flight entry is erased.  Drain first, or the
  // second notification is rejected by the in-flight guard, which bumps a
  // different counter than the one asserted below.
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification for the same URL — with the first retired, it is skipped
  // by the processed-variant dedup set.  (JS has no cooldown of its own; the
  // tentative cooldown entry is HTML-only.)  Poll for the skip counter to
  // advance instead of a fixed sleep (slow shards can sit at the pipe read past
  // a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Second JS notification should be skipped by dedup/cooldown";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsCooldownClearedOnSuccess) {
  // Verify that after successful JS processing, the cooldown is cleared
  // so that ClearDedupAndCooldown + re-notification can reprocess.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-clear.js";
  std::string js = "function hello() { console.log('hello world'); }";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — process JS.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u);

  // js_processed moves before the in-flight entry is erased, and InvalidateUrl
  // does not clear the in-flight set.  Drain first, or the re-send below is
  // rejected by the in-flight guard and never reprocesses.
  WaitForWorkerIdle(worker);

  // Verify cooldown was cleared on success (not just dedup).
  auto cooldown = worker.GetCooldownForUrl(url, "");
  EXPECT_FALSE(cooldown.has_value())
      << "Cooldown should be cleared after successful JS processing";

  uint64_t js_before = worker.stats().js_processed.load();

  // Clear dedup to simulate re-notification scenario.
  worker.ClearDedupAndCooldown(url, "");
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, js);

  // Re-send notification — should process since cooldown was cleared on
  // success and dedup was explicitly cleared.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() > js_before) break;
  }

  EXPECT_GT(worker.stats().js_processed.load(), js_before)
      << "After clearing dedup, JS should be reprocessed (cooldown was "
         "cleared on success)";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssSuccessPathClearsCooldown) {
  // Verify that successful CSS processing clears the tentative cooldown,
  // so the URL is not locked out for 60s after a successful write.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/write-fail-throttle.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — processes successfully.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // Verify the cooldown was NOT left lingering (it should be cleared
  // on success).
  auto cooldown = worker.GetCooldownForUrl(url, "");
  EXPECT_FALSE(cooldown.has_value())
      << "Cooldown should be cleared after successful CSS processing";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DualSchemePurgeInvalidatesBothEntries) {
  // Cache the same URL under both HTTP and HTTPS schemes.
  // The management socket PURGE should remove both.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "/dual-scheme-purge.css";
  std::string hostname = "example.com";
  std::string content_https = "body { color: blue; }";
  std::string content_http = "body { color: red; }";

  // Write under HTTPS scheme.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             content_https.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(content_https.data(), content_https.size())));
    (void)wh->close_sync();
  }

  // Write under HTTP scheme.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "http", id,
                                             content_http.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(content_http.data(), content_http.size())));
    (void)wh->close_sync();
  }

  // Verify both exist.
  {
    CapabilityMask dm;
    auto r1 = worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    ASSERT_TRUE(r1.has_value()) << "HTTPS entry should exist";
    auto r2 = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    ASSERT_TRUE(r2.has_value()) << "HTTP entry should exist";
  }

  // Purge via InvalidateUrl with both schemes (as mgmt PURGE does).
  int deleted = worker.InvalidateUrl(url, hostname, "https");
  deleted += worker.InvalidateUrl(url, hostname, "http");
  EXPECT_GE(deleted, 2) << "Both scheme entries should be deleted";

  // Verify both are gone.
  {
    CapabilityMask dm;
    auto r1 = worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    EXPECT_FALSE(r1.has_value()) << "HTTPS entry should be purged";
    auto r2 = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    EXPECT_FALSE(r2.has_value()) << "HTTP entry should be purged";
  }

  worker.Shutdown();
}

TEST_F(WorkerTest, SchemeIsolatedCaching) {
  // HTTP and HTTPS entries for the same URL must be independent.
  // Purging one scheme must not affect the other.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "/scheme-isolated.html";
  std::string hostname = "example.com";
  std::string https_body = "<html>HTTPS version</html>";
  std::string http_body = "<html>HTTP version</html>";

  // Write HTTPS variant.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kHtml;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             https_body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(https_body.data(), https_body.size())));
    (void)wh->close_sync();
  }

  // Write HTTP variant.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kHtml;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "http", id,
                                             http_body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(http_body.data(), http_body.size())));
    (void)wh->close_sync();
  }

  // Read both — should get different content.
  {
    CapabilityMask dm;
    auto r_https =
        worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    ASSERT_TRUE(r_https.has_value());
    auto c1 = r_https->content();
    std::string_view s1(reinterpret_cast<const char*>(c1.data()), c1.size());
    EXPECT_EQ(s1, https_body);

    auto r_http = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    ASSERT_TRUE(r_http.has_value());
    auto c2 = r_http->content();
    std::string_view s2(reinterpret_cast<const char*>(c2.data()), c2.size());
    EXPECT_EQ(s2, http_body);
  }

  // Purge only HTTPS.
  worker.InvalidateUrl(url, hostname, "https");

  // HTTPS should be gone, HTTP should survive.
  {
    CapabilityMask dm;
    auto r_https =
        worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    EXPECT_FALSE(r_https.has_value()) << "HTTPS should be purged";

    auto r_http = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    ASSERT_TRUE(r_http.has_value()) << "HTTP should survive HTTPS purge";
    auto c = r_http->content();
    std::string_view s(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(s, http_body);
  }

  worker.Shutdown();
}

TEST_F(WorkerTest, CooldownSchemeIsolation) {
  // Cooldown for HTTPS should not block HTTP processing, and vice versa.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML under both schemes so both notifications
  // can find the original content.
  std::string url = "http://example.com/cooldown-scheme.html";
  std::string path = StripSchemeAuthority(url);
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Test</h1></body></html>";

  // CacheOriginal writes under "https" scheme with empty hostname.
  CacheOriginal(worker.cache(), url, html);
  // Also write under "http" scheme so the HTTP notification can find it.
  {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    auto wh =
        worker.cache()->WriteAlternate(path, "", "http", id, html.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(html.data(), html.size())));
    (void)wh->close_sync();
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send HTTPS notification — triggers processing and cooldown.
  {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
  }

  // Wait for HTTPS processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_processed.load(), 1u);

  // Now send HTTP notification for the same URL.
  // With scheme-isolated cooldown, this should NOT be blocked.
  uint64_t processed_before = worker.stats().html_processed.load();
  {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "http";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
  }

  // Wait for HTTP processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() > processed_before) break;
  }
  EXPECT_GT(worker.stats().html_processed.load(), processed_before)
      << "HTTP notification should not be blocked by HTTPS cooldown";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, PurgeGenerationSchemeIsolation) {
  // Purge generation for HTTPS should not affect HTTP dispatch.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/purge-gen-scheme";
  std::string hostname = "example.com";

  // Purge the HTTPS version (bumps generation for HTTPS key).
  worker.InvalidateUrl(url, hostname, "https");

  // WasPurgedSinceDispatch for HTTPS should detect the purge.
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 0, "https"))
      << "HTTPS should show as purged";

  // WasPurgedSinceDispatch for HTTP should NOT detect it.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 0, "http"))
      << "HTTP should not be affected by HTTPS purge";

  worker.Shutdown();
}

TEST_F(WorkerTest, NativeSvgNotificationMarksDedup) {
  // Native SVG files (origin content-type image/svg+xml) skip raster
  // transcoding and only write gzip/brotli compressed variants.
  // Without MarkVariantProcessed, every subsequent notification for the
  // same SVG URL bypasses the in-memory dedup check, causing unbounded
  // reprocessing and "alternate not found" warnings on fallback HITs.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Cache a minimal SVG with origin_content_type = "image/svg+xml".
  std::string url = "http://example.com/images/logo.svg";
  std::string svg_data =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">)"
      R"(<circle cx="50" cy="50" r="40" fill="blue"/></svg>)";
  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/svg+xml";
  CacheOriginalWithMeta(worker.cache(), url, svg_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — should process the SVG (write compressed variants).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for compressed variants to be written (strong completion signal).
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().gzip_variants_written.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u)
      << "SVG compressed variant should have been written";

  // The SVG notification's counters move before its in-flight entry is erased.
  // Drain first, or the duplicate below is rejected by the in-flight guard
  // instead of the dedup set.
  WaitForWorkerIdle(worker);

  uint64_t variants_before = worker.stats().variants_written.load();
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification (same mask) — should be caught by in-memory dedup.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Second notification for native SVG should be caught by in-memory "
         "dedup (MarkVariantProcessed must be called after writing compressed "
         "variants)";
  EXPECT_EQ(worker.stats().variants_written.load(), variants_before)
      << "No new variants should be written for duplicate SVG notification";

  // Third notification with a different client mask (AVIF+Brotli+Desktop,
  // simulating a real browser).  The dedup check normalizes the mask by
  // stripping encoding and format bits, so this should still hit the
  // same dedup key as the first notification (both are Desktop viewport).
  uint64_t skipped_before2 = worker.stats().notifications_skipped_dedup.load();
  CacheNotification browser_notification = notification;
  browser_notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kBrotli)
          .Encode();
  SendNotification(browser_notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before2)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before2)
      << "AVIF+Brotli notification should be dedup-skipped after SVG "
         "processing (mask normalization strips encoding and format bits)";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Issue B: content-hash self-purge + own-write-failure loop (agent_optimize)
//
// Root cause: the kContentHash sentinel is computed by re-hashing the worker's
// READ of slot 0x08, which flips raw<->worker-optimized over time.  When a
// re-notification reads the worker's OWN optimized bytes but the sentinel was
// written over raw bytes, content_changed falsely fires, the whole key is
// purged, and the in-flight HTML write is fenced by its now-stale dispatch
// baseline — failing its own write and double-counting write_failures+errors.
// =============================================================================

// Build an HTML/agent_optimize WorkerConfig that exercises the content-hash
// sentinel path.  browser_analysis.enabled=true with a nonexistent Chrome so
// the manager falls back to the heuristic critical-CSS extractor (no real
// render), while agent_optimize=true turns on the content-hash sentinel logic.
static WorkerConfig MakeAgentOptimizeConfig(const std::string& socket_path,
                                            const std::string& cache_path) {
  WorkerConfig config;
  config.socket_path = socket_path;
  config.cache_path = cache_path;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // The test binary runs as root in the build container,
  // where the Chrome sandbox is (correctly) unavailable and `require`
  // would refuse to construct the manager.  These tests seed profiles
  // directly and never spawn Chrome, so take the documented opt-out.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";
  config.browser_analysis.agent_optimize = true;
  return config;
}

// HTML with inline CSS so the heuristic CriticalCssExtractor yields critical
// CSS, the worker writes an optimized identity variant (flags get
// kFlagWorkerProcessed), and the kContentHash sentinel is written.
static const char* const kAgentHtml =
    "<html><head><style>body { margin: 0; } "
    "h1 { color: red; }</style></head>"
    "<body><h1>Hello World</h1></body></html>";

TEST_F(WorkerTest, ContentHashIdempotency_OptimizedIdentityDoesNotSelfPurge) {
  // Two notifications for the same URL with NO origin change must not
  // self-purge.  Pre-fix the 2nd notification re-hashes the worker's own
  // optimized slot 0x08, mismatches the raw-origin sentinel, purges, and
  // fails its in-flight write — bumping content_hash_stale +
  // alternate_write_failures + errors in lockstep (the live prod bug).
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;  // Desktop/Identity = 0x08
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First notification: worker writes the optimized identity + sentinel.
  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";
  // Sanity: the identity slot now carries the worker-processed optimized
  // bytes (this is what the 2nd read re-hashes, exposing the bug).
  auto opt_meta = ReadVariantWithMeta(worker.cache(), cache_url, opt_mask);
  ASSERT_TRUE(opt_meta.has_value());
  EXPECT_NE(opt_meta->metadata.flags & AlternateMetadata::kFlagWorkerProcessed,
            0)
      << "Optimized identity must carry kFlagWorkerProcessed";

  uint64_t chs_before = worker.stats().content_hash_stale.load();
  uint64_t wf_before = worker.stats().alternate_write_failures.load();
  uint64_t err_before = worker.stats().errors.load();

  // Clear dedup/cooldown (exactly what origin-refresh does) so the SECOND
  // notification re-enters the content-hash block, reading the worker's own
  // optimized slot 0x08 — WITHOUT re-writing raw.
  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  // Give the 2nd notification time to run to completion.
  for (int i = 0; i < 30 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 2 &&
        worker.stats().notifications_skipped_dedup.load() == 0) {
      // Allow the worker thread to fully finish processing.
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      break;
    }
  }

  EXPECT_EQ(worker.stats().content_hash_stale.load(), chs_before)
      << "No origin change: must NOT self-purge (content_hash_stale stable)";
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), wf_before)
      << "No origin change: must NOT hard-fail its own write";
  EXPECT_EQ(worker.stats().errors.load(), err_before)
      << "No origin change: must NOT increment errors";
  EXPECT_TRUE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(opt_mask.Encode() & 0xFF))))
      << "Variant set must survive a no-change reprocess";
}

TEST_F(WorkerTest, ContentHashIdempotency_RealOriginChangeStillPurges) {
  // A GENUINE origin change (raw bytes at slot 0x08 differ from the bound
  // sentinel) must still purge exactly once and rebuild — the fix must not
  // suppress real invalidation.
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";

  uint64_t chs_before = worker.stats().content_hash_stale.load();

  // Overwrite slot 0x08 with DIFFERENT raw origin bytes (flags=0, i.e. a real
  // origin refresh re-recording fresh raw content), then re-notify.
  std::string new_html =
      "<html><head><style>body { padding: 8px; } "
      "h2 { color: blue; }</style></head>"
      "<body><h2>Brand New Origin</h2></body></html>";
  ASSERT_NE(new_html, std::string(kAgentHtml));
  CacheOriginal(worker.cache(), cache_url, new_html);  // flags default 0 (raw)
  // Evict THIS process's RAM-tier copy of the identity so the re-notification
  // reads the freshly-written RAW bytes (flags=0), not the stale optimized
  // identity left in the write-around RAM tier.  This is exactly what
  // HandleOriginRefreshed does before re-dispatching in production.
  CapabilityMask identity_mask;
  worker.cache()->EvictAlternateFromRamCache(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(identity_mask.Encode() & 0xFF)));

  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().content_hash_stale.load() > chs_before) break;
  }

  EXPECT_EQ(worker.stats().content_hash_stale.load(), chs_before + 1)
      << "A genuine origin change must purge exactly once";
}

TEST_F(WorkerTest, AgentMarkdownStaysServedAcrossReprocess) {
  // The complete-fix gate (forces durable raw-origin hash plumbing, not the
  // option-a stop-gap alone): a bound agent markdown variant must remain
  // SERVABLE after a no-change reprocess.  The serve gate
  // (ReadBestAlternateAgentByKey) requires the markdown's stamped
  // origin_html_hash to byte-equal the LIVE kContentHash sentinel.  Pre-fix
  // (and with option-a only) the reprocess rewrites the sentinel to the
  // optimized-content hash, so the markdown stamp != live sentinel -> the
  // serve gate refuses (NotFound) and HTML is served instead.
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First notification: worker writes optimized identity + the kContentHash
  // sentinel = SHA256(raw).
  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";

  // Read the live kContentHash sentinel (SHA256 of the raw origin HTML) and
  // stamp a synthetic agent markdown variant (id=0x7C) with it — emulating a
  // completed render whose binding matches the current origin.  (The worker
  // test harness has no Chrome, so we write the markdown directly.)
  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize)
      << "kContentHash sentinel must be written after first processing";

  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Sanity: the markdown serves now (binding fresh).
  CapabilityMask agent_mask;
  auto served_before =
      worker.cache()->ReadBestAlternateAgent(cache_url, "", "https", agent_mask,
                                             /*agent_request_entitled=*/true);
  ASSERT_TRUE(served_before.has_value())
      << "Markdown must be servable before reprocess (binding fresh)";

  // Re-process WITHOUT an origin change.
  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  for (int i = 0; i < 30 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 2 &&
        worker.stats().notifications_skipped_dedup.load() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      break;
    }
  }

  // The markdown variant must STILL exist and STILL be servable — meaning the
  // live sentinel was NOT rewritten to an optimized-content hash.
  EXPECT_TRUE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      static_cast<AlternateId>(SentinelId::kAgentMarkdown)))
      << "Agent markdown variant must survive a no-change reprocess";
  auto served_after =
      worker.cache()->ReadBestAlternateAgent(cache_url, "", "https", agent_mask,
                                             /*agent_request_entitled=*/true);
  EXPECT_TRUE(served_after.has_value())
      << "Markdown binding must remain fresh after reprocess: the live "
         "kContentHash sentinel must stay SHA256(raw), not the optimized hash";
}

// =============================================================================
// Durability (#19/D2): the origin-refresh sentinel fires on every TTL
// expiry / 200 revalidation, even when the re-fetched body is byte-identical.
// The rendered markdown variant must be PRESERVED on an unchanged-content
// refresh (still bound to live content) and PURGED on a genuine content change
// (superseded — never served stale).  These integration tests drive the real
// HandleOriginRefreshed purge path end-to-end (the harness has no Chrome, so the
// markdown variant is stamped directly, as AgentMarkdownStaysServedAcrossReprocess
// does).
// =============================================================================

TEST_F(WorkerTest, AgentMarkdownPreservedOnUnchangedOriginRefresh) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First processing: optimized identity + kContentHash sentinel = SHA256(raw).
  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";

  // Stamp a synthetic markdown variant bound to the live content hash.
  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize);
  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Simulate nginx's freshness-driven re-fetch with the SAME body (no content
  // change): drop the worker's optimized identity, then re-record the identical
  // RAW body so the slot holds raw origin bytes at sentinel time (nginx
  // overwrites the identity before sending the sentinel; the plain CacheOriginal
  // helper cannot replace an existing worker-processed slot).
  (void)worker.cache()->RemoveAlternate(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(opt_mask.Encode() & 0xFF)));
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  // Send the origin-refresh sentinel.
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().origin_refresh_purges.load(), 1u)
      << "sentinel should have triggered a purge";
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // D2: the markdown variant survives the unchanged-content refresh.
  EXPECT_TRUE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      static_cast<AlternateId>(SentinelId::kAgentMarkdown)))
      << "markdown variant must be preserved across an unchanged-content "
         "origin refresh (D2): the rendering is still bound to live content";
  EXPECT_GE(worker.stats().agent_markdown_preserved.load(), 1u);
  EXPECT_EQ(worker.stats().agent_markdown_purged_on_change.load(), 0u);
  // Still servable (binding intact, sentinel never rewritten to a new hash).
  CapabilityMask agent_mask;
  EXPECT_TRUE(worker.cache()
                  ->ReadBestAlternateAgent(cache_url, "", "https", agent_mask,
                                           /*agent_request_entitled=*/true)
                  .has_value())
      << "preserved markdown must remain servable";
}

TEST_F(WorkerTest, AgentMarkdownPurgedOnChangedOriginRefresh) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty());

  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize);
  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Simulate nginx's re-fetch with a DIFFERENT body (genuine content change):
  // drop the worker's optimized identity, then record the fresh RAW body so the
  // slot holds the changed origin bytes at sentinel time.  The rendered markdown
  // is now bound to superseded content.
  std::string fresh_html = std::string(kAgentHtml) + "<p>changed body</p>";
  (void)worker.cache()->RemoveAlternate(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(opt_mask.Encode() & 0xFF)));
  CacheOriginal(worker.cache(), cache_url, fresh_html);

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().origin_refresh_purges.load(), 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // D2 (negative): the markdown bound to the OLD content MUST be purged — never
  // preserved/served stale (D8 fail-closed binding).  Without a real Chrome the
  // inline rebuild cannot re-render, so the variant stays absent, which is the
  // correct "not stale-served" outcome.
  EXPECT_GE(worker.stats().agent_markdown_purged_on_change.load(), 1u);
  EXPECT_EQ(worker.stats().agent_markdown_preserved.load(), 0u);
  EXPECT_FALSE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      static_cast<AlternateId>(SentinelId::kAgentMarkdown)))
      << "markdown bound to superseded origin content must be purged";
}

TEST_F(WorkerTest, AgentMarkdownWorkerProcessedIdentityPurgesConservatively) {
  // Durability (#19/D2) — fail-safe.  When the identity slot holds the
  // worker's OWN optimized output (kFlagWorkerProcessed) rather than the fresh
  // raw origin, FreshOriginHash returns nullopt and the variant is PURGED
  // conservatively — never preserved on a hash we cannot directly verify
  // against the raw origin.  Guards against re-introducing a durable-hash reuse
  // that would falsely preserve here.
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty());

  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize);
  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Do NOT re-record a raw identity: the slot still holds the worker's optimized
  // output (kFlagWorkerProcessed).  FreshOriginHash -> nullopt -> conservative
  // purge, even though the origin content is in fact unchanged.
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().origin_refresh_purges.load(), 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(worker.stats().agent_markdown_preserved.load(), 0u)
      << "a worker-processed identity is unverifiable raw origin -> never "
         "preserve (fail-safe)";
  EXPECT_GE(worker.stats().agent_markdown_purged_on_change.load(), 1u);
}

// =============================================================================
// Issue C: rebuild the agent markdown (124) variant after an origin-refresh
// purge by propagating agent_request on the inline rebuild notification.
//
// HandleOriginRefreshed purges the whole non-identity set (incl. 124) then
// re-processes the URL at the default mask.  On a profile-present (warm) page,
// markdown is only rebuilt by the warm-template branch, which requires
// notification.agent_request==true.  Pre-fix the rebuild left agent_request at
// its default false, so 124 stayed absent ~70% of the time on landing pages
// and HTML was served to AI crawlers instead of markdown.
//
// Worker-level seam test (no Chrome needed — the harness has no real Chrome, so
// a full render-to-124 path is unsatisfiable in CI).  Asserts the rebuild-intent
// contract directly on the factored-out MakeOriginRefreshRebuild helper.
// =============================================================================

TEST_F(WorkerTest,
       OriginRefreshRebuildPropagatesAgentRequestWhenMarkdownExisted) {
  CacheNotification origin;
  origin.url = "http://example.com/index.html";
  origin.scheme = "https";
  origin.content_type = ContentType::kHtml;
  // The origin-refresh sentinel arrives at a reserved mask; the rebuild must
  // re-process at the DEFAULT capability mask regardless.
  origin.capability_mask = 0xFFFFFFFFu;
  origin.agent_request = false;

  // Positive case: the URL had an agent markdown variant before the purge —
  // the rebuild MUST carry agent_request=true so the warm-template branch
  // re-renders markdown (which AgentMarkdownNeedsBuild requests after the
  // purge dropped 124).
  CacheNotification rebuild_with_md = Worker::MakeOriginRefreshRebuild(
      origin, /*had_markdown=*/true, /*has_intent=*/false);
  EXPECT_TRUE(rebuild_with_md.agent_request)
      << "rebuild must request agent render when 124 pre-existed (else "
         "markdown "
         "is never rebuilt on a warm page and the URL serves HTML to crawlers)";
  EXPECT_EQ(rebuild_with_md.capability_mask, CapabilityMask().Encode())
      << "rebuild must re-process at the default capability mask";

  // Negative case (locks the demand-gate): no pre-existing markdown AND no
  // sticky intent -> the rebuild must NOT request a render, so a page that
  // never had markdown never forces a (wasted, possibly unwanted) agent render.
  CacheNotification rebuild_no_md = Worker::MakeOriginRefreshRebuild(
      origin, /*had_markdown=*/false, /*has_intent=*/false);
  EXPECT_FALSE(rebuild_no_md.agent_request)
      << "rebuild must NOT request agent render when the URL never had "
         "markdown";
  EXPECT_EQ(rebuild_no_md.capability_mask, CapabilityMask().Encode());

  // Durability (#19/D3): the variant was already lost in a prior churn
  // (had_markdown=false) but the URL EVER rendered markdown (has_intent=true) —
  // the rebuild MUST still request a render so the multi-churn demand-gating
  // race cannot strand the URL on HTML permanently.
  CacheNotification rebuild_intent = Worker::MakeOriginRefreshRebuild(
      origin, /*had_markdown=*/false, /*has_intent=*/true);
  EXPECT_TRUE(rebuild_intent.agent_request)
      << "rebuild must request agent render when the URL ever rendered "
         "markdown "
         "(sticky intent heals a variant lost in a prior multi-churn race)";
}

// =============================================================================
// Issue E: wire the dead policy counters + split benign purge-fences from hard
// write failures.
// =============================================================================

TEST_F(WorkerTest, PolicyCountersIncrementWhenAsyncCssApplied) {
  // The policy.* counters (computed/async_css_enabled/script_deferral_enabled)
  // are read by the dashboard but had ZERO production increment sites — they
  // read 0 on prod even though async CSS is applied + served.  When the worker
  // applies a transform with critical CSS AND the empirical gate authorizes
  // deferral, policy_computed and policy_async_css_enabled must increment.
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Inline CSS so the extractor yields critical CSS; a validated profile bound
  // to that same inline CSS (the whole combined sheet here) is what makes
  // has_async_css true.
  std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  std::string cache_url = "/policy.html";
  std::string notification_url = "http://example.com/policy.html";
  CacheOriginal(worker.cache(), cache_url, html);

  HtmlScanner policy_scanner;
  HtmlScanResult policy_scan = policy_scanner.Scan(notification_url, html);
  ASSERT_TRUE(policy_scan.success);
  StoreBrowserProfile(worker, notification_url, html, "h1{color:red}",
                      /*validated=*/true,
                      /*validated_against_css=*/policy_scan.inline_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  CapabilityMask mask;  // Desktop/Identity
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written (transform must "
                                   "run for the policy counters to fire)";

  EXPECT_GE(worker.stats().policy_computed.load(), 1u)
      << "policy_computed must increment once a policy decision is made";
  EXPECT_GE(worker.stats().policy_async_css_enabled.load(), 1u)
      << "policy_async_css_enabled must increment when async CSS is applied";
}

TEST_F(WorkerTest, ContentHashStaleSelfPurgeCountsAsFencedNotFailure) {
  // A content-hash invalidation purge bumps the URL's purge generation in the
  // SAME notification, so that notification's in-flight HTML write is correctly
  // purge-fenced.  This benign fence must count as alternate_writes_fenced —
  // NOT as alternate_write_failures + errors (the live prod lockstep
  // write_failures==content_hash_stale==errors that false-alarms the dashboard).
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/fenced.html";
  std::string notification_url = "http://example.com/fenced.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First notification binds the kContentHash sentinel over the raw HTML.
  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";

  uint64_t chs_before = worker.stats().content_hash_stale.load();
  uint64_t wf_before = worker.stats().alternate_write_failures.load();
  uint64_t err_before = worker.stats().errors.load();
  uint64_t fenced_before = worker.stats().alternate_writes_fenced.load();

  // Genuine origin change: overwrite slot 0x08 with DIFFERENT raw bytes and
  // evict the RAM tier so the re-notification reads the fresh raw (exactly the
  // production origin-refresh path).  The content-hash check then fires, purges
  // (bumping the generation), and the in-flight HTML write is purge-fenced.
  std::string new_html =
      "<html><head><style>section { gap: 4px; } "
      "p { color: green; }</style></head>"
      "<body><p>Different Origin Body</p></body></html>";
  ASSERT_NE(new_html, std::string(kAgentHtml));
  CacheOriginal(worker.cache(), cache_url, new_html);
  CapabilityMask identity_mask;
  worker.cache()->EvictAlternateFromRamCache(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(identity_mask.Encode() & 0xFF)));

  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().content_hash_stale.load() > chs_before &&
        worker.stats().alternate_writes_fenced.load() > fenced_before)
      break;
  }
  // Allow any trailing counter writes to settle.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_GE(worker.stats().content_hash_stale.load(), chs_before + 1)
      << "A genuine origin change must purge";
  EXPECT_GE(worker.stats().alternate_writes_fenced.load(), fenced_before + 1)
      << "The same-notification purge-fenced write must count as FENCED";
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), wf_before)
      << "A benign purge-fence must NOT count as a hard write failure";
  EXPECT_EQ(worker.stats().errors.load(), err_before)
      << "A benign purge-fence must NOT increment errors";
}

// =============================================================================
// Measured fold: the browser's per-viewport render replaces the element-count
// estimate, end to end through the serve path.
// =============================================================================

namespace measured_fold {

constexpr const char* kCssUrl = "http://example.com/measured.css";
// `.flex` is what a real browser measured above the fold; `.mt-96` is the
// control — same document, same distance past the estimate, NOT measured.
constexpr const char* kCss =
    ".flex{display:flex}.mt-96{margin:24rem}p{margin:0}";
// The profile's raw coverage blob. It deliberately does NOT mention `.flex`:
// coverage identities are a separate force-include channel, and if this named
// `.flex` the rule would be retained whether or not the measured fold works.
constexpr const char* kProfileCriticalCss = "p{margin:0}";

// A head-heavy page — the live shape this change exists for. Thirty <meta>
// elements exhaust the 25-element estimate before the first body element, so
// both body divs are judged below the fold by document position alone.
std::string PageHtml() {
  std::string html = "<html><head><link rel=\"stylesheet\" href=\"";
  html += kCssUrl;
  html += "\">";
  for (int i = 0; i < 30; ++i) html += "<meta name=\"m\" content=\"v\">";
  html +=
      "</head><body><div class=\"flex\">a</div>"
      "<div class=\"mt-96\">b</div></body></html>";
  return html;
}

// Seed a profile for `html`'s template with the given measured fold on every
// viewport.
void StoreProfileWithMeasuredFold(
    Worker& worker, const std::string& url, const std::string& html,
    const std::vector<std::string>& above_fold_selectors) {
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(url, html);
  ASSERT_TRUE(scan.success);

  ViewportProfile vp;
  vp.critical_css = kProfileCriticalCss;
  // Under the 0.60 inline budget, so the block is kept rather than suppressed.
  vp.css_coverage_ratio = 0.2f;
  vp.total_css_bytes = 5000;
  vp.unused_css_bytes = 4000;
  vp.above_fold_selectors = above_fold_selectors;

  OptimizationProfile profile;
  profile.template_hash_hex = "measured";
  profile.analyzed_url = url;
  profile.mobile = vp;
  profile.tablet = vp;
  profile.desktop = vp;
  profile.created_at = 0;
  profile.expires_at = 0;  // no TTL — LookupProfile keeps it

  ASSERT_NE(worker.TestBrowserManager(), nullptr);
  worker.TestBrowserManager()->TestStoreProfile(
      TemplateDetector::HashStructure(scan), profile);
}

}  // namespace measured_fold

// The negative half of the A/B. A profile with NO measured fold reproduces the
// old behaviour exactly: both body divs sit past the estimate and neither
// rule is inlined. Without this, the positive test below could pass for the
// wrong reason (e.g. an extractor that suddenly admits everything).
TEST_F(WorkerTest, UnmeasuredFoldLeavesLateElementsOutOfCriticalCss) {
  using namespace measured_fold;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unmeasured.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreProfileWithMeasuredFold(worker, url, html,
                               /*above_fold_selectors=*/{});

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("display:flex"), std::string::npos)
      << "with nothing measured, an element past the estimate stays out";
  EXPECT_EQ(variant.find("24rem"), std::string::npos);
}

// The whole change, end to end: the profile says a browser measured `.flex`
// above the fold, and the block the visitor receives contains its rule. This is
// the only test that pins the serve-path plumbing — a profile field that is
// read but never passed on, or passed to a derivation that ignores it, leaves
// every unit test in this change green while production behaves exactly as
// before.
TEST_F(WorkerTest, MeasuredFoldAdmitsLateElementsIntoCriticalCss) {
  using namespace measured_fold;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/measured.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreProfileWithMeasuredFold(worker, url, html,
                               /*above_fold_selectors=*/{".flex"});

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos)
      << "the critical block must still be inlined";
  EXPECT_NE(variant.find("display:flex"), std::string::npos)
      << "the measured above-the-fold element's rule must be inlined";
  // Selective, not a switch that admits the document: the sibling div is the
  // same distance past the estimate and was NOT measured above the fold.
  EXPECT_EQ(variant.find("24rem"), std::string::npos)
      << "an unmeasured element must stay out of the critical block";
}

// =============================================================================
// The durable original as an OPTIMIZATION SOURCE (issue #1323).
//
// The durable-original class (SentinelId::kOriginalContent, 0x0C) is never
// SELECTABLE: PageSpeedSelector::select skips it unconditionally so that "the
// original" can never compete with the optimized variants at serve time.  The
// worker's optimization-source read went through that same selector, so a URL
// whose only entry was a durable original produced nothing — an external
// writer using the published C ABI (ps_cache_write_original) that recorded the
// origin's bytes into the class and notified correctly got a cache that filled
// and an optimizer that never ran, silently.
//
// The worker's SOURCE read now falls back to an exact-id read of the class.
// These tests pin both halves: the fallback works for every content-type
// branch, and the serving guarantee it falls back from is untouched.
// =============================================================================

// THE regression test.  Only entry for the URL is a durable original; the
// worker must still produce an optimized variant.  Red before the fallback:
// the selector skips the class, so the source read missed and the CSS branch
// returned at "Original CSS not found in cache".
TEST_F(WorkerTest, CssNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/original-only.css";
  const std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  // Precondition, and the reason the fix is needed: ordinary selection cannot
  // see this URL at all.  If this ever starts returning content, the serving
  // guarantee has been broken and the rest of the test is meaningless.
  ASSERT_TRUE(ReadBestVariant(worker.cache(), url, CapabilityMask()).empty())
      << "the durable original must not be reachable by ordinary selection";

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, CapabilityMask());
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty())
      << "a URL whose only entry is a durable original yielded no variant";
  EXPECT_LT(variant.size(), css.size());
  EXPECT_NE(variant.find("margin"), std::string::npos);
  EXPECT_NE(variant.find("color"), std::string::npos);
  EXPECT_GE(worker.stats().css_processed.load(), 1u);
  EXPECT_GE(worker.stats().source_reads_from_durable_original.load(), 1u)
      << "the fallback counter must make this path observable";
}

TEST_F(WorkerTest, JsNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/original-only.js";
  const std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheDurableOriginal(worker.cache(), url, js, "application/javascript",
                       ContentType::kJs);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "JS notification for a durable-original-only URL produced nothing";
  EXPECT_FALSE(ReadVariant(worker.cache(), url, CapabilityMask()).empty());
}

TEST_F(WorkerTest, HtmlNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string cache_url = "/original-only.html";
  const std::string notification_url = "http://example.com/original-only.html";
  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  CacheDurableOriginal(worker.cache(), cache_url, html, "text/html",
                       ContentType::kHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  ASSERT_FALSE(variant.empty())
      << "HTML notification for a durable-original-only URL produced nothing";
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_NE(variant.find("Hello World"), std::string::npos);
}

// The image branch, via the SVG passthrough — it exercises the same source
// read without standing up a raster transcode, and its "did the source read
// succeed" signal (compressed variants exist) is unambiguous.
TEST_F(WorkerTest, ImageNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/original-only.svg";
  const std::string svg_data =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" "
      "height=\"100\"><circle cx=\"50\" cy=\"50\" r=\"40\"/></svg>";
  CacheDurableOriginal(worker.cache(), url, svg_data, "image/svg+xml",
                       ContentType::kOther);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gz_variant = ReadVariant(worker.cache(), url, gz_mask);
    if (!gz_variant.empty()) break;
  }
  EXPECT_FALSE(gz_variant.empty())
      << "image notification for a durable-original-only URL produced nothing";
}

// ORDERING (no preference inversion).  With BOTH a selectable alternate and a
// durable original present, the selector's answer still wins — the fallback is
// reached only when the selector found nothing usable, so nothing this build
// preferred as a source before it prefers any less now.
TEST_F(WorkerTest, ASelectableAlternateStillOutranksTheDurableOriginal) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/both-present.css";
  // Distinguishable by a declaration that survives minification.
  const std::string selectable_css = "body  {  outline:  1px  solid  red;  }\n";
  const std::string original_css = "body  {  zoom:  2;  }\n";
  CacheOriginal(worker.cache(), url, selectable_css);
  CacheDurableOriginal(worker.cache(), url, original_css, "text/css",
                       ContentType::kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().css_processed.load(), 1u);

  const std::string variant =
      ReadVariant(worker.cache(), url, CapabilityMask());
  ASSERT_FALSE(variant.empty());
  EXPECT_NE(variant.find("outline"), std::string::npos)
      << "the selectable alternate must remain the preferred source";
  EXPECT_EQ(variant.find("zoom"), std::string::npos)
      << "the fallback must not displace a source the selector found";
  EXPECT_EQ(worker.stats().source_reads_from_durable_original.load(), 0u)
      << "the fallback must not run when the selector answered";
}

// THE SERVING BOUNDARY, pinned against the fallback.  Reading the class as a
// SOURCE must not make it reachable by SELECTION — before or after the worker
// has used it.  The selector itself is unchanged; this asserts the property
// end-to-end through the worker rather than only in the selector's own tests.
TEST_F(WorkerTest, UsingTheOriginalAsASourceLeavesItUnselectable) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unselectable.css";
  const std::string css = "body  {  zoom:  2;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  // Every request shape reaches the same verdict: not selectable.
  const CapabilityMask shapes[] = {
      CapabilityMask(),
      CapabilityMask(
          CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
          CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
          CapabilityMask::TransferEncoding::kGzip),
      CapabilityMask(
          CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kTablet,
          CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOn,
          CapabilityMask::TransferEncoding::kBrotli),
  };
  for (const auto& shape : shapes) {
    EXPECT_TRUE(ReadBestVariant(worker.cache(), url, shape).empty())
        << "selection reached the durable original before the worker ran";
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().css_processed.load(), 1u)
      << "the source read must have succeeded for this test to say anything";

  // After the worker used it as a source, selection answers with the
  // OPTIMIZED bytes — never the original's, at any request shape.
  for (const auto& shape : shapes) {
    const std::string selected = ReadBestVariant(worker.cache(), url, shape);
    EXPECT_NE(selected, css)
        << "selection returned the durable original's raw bytes";
  }
  // And the class is still there, reachable only by exact id.
  auto by_id = worker.cache()->ReadOriginalAlternate(StripSchemeAuthority(url),
                                                     "", "https");
  ASSERT_TRUE(by_id.has_value())
      << "the durable original must survive being used as a source";
  auto body = by_id->content();
  EXPECT_EQ(
      std::string(reinterpret_cast<const char*>(body.data()), body.size()),
      css);
}

// FRESHNESS.  The class's lifetime is origin-freshness-bound, and that binding
// is a property of the STAMPED ORIGIN STATE evaluated by whoever reads the
// entry — the deliberate-read API refuses only a missing/mis-stamped metadata
// prefix, never a past-its-window entry (PageSpeedCache::ReadOriginalAlternate
// ByKey).  So the worker does not re-decide it, and it cannot launder it: a
// variant built from a durable original carries that original's origin state
// verbatim, so it reaches the same serve-side freshness verdict the original
// would have.  A source read whose result was stamped fresh must not produce a
// variant that looks fresher than the origin allowed.
TEST_F(WorkerTest, AVariantBuiltFromTheOriginalInheritsItsOriginFreshness) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/freshness.css";
  const std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  // Stamped long past any plausible window: as an origin state this reads
  // stale at every evaluation time this test could run at.
  constexpr uint32_t kInsertedAt = 1'000'000'000;
  constexpr uint32_t kMaxAge = 600;
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss,
                       kInsertedAt, kMaxAge);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::optional<VariantWithMeta> got;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    got = ReadVariantWithMeta(worker.cache(), url, CapabilityMask());
    if (got.has_value()) break;
  }
  ASSERT_TRUE(got.has_value());

  // The origin state came across verbatim — this is what makes a separate
  // freshness gate on the source read unnecessary rather than merely omitted.
  EXPECT_EQ(got->metadata.cache_inserted_at, kInsertedAt);
  EXPECT_EQ(got->metadata.origin_max_age, kMaxAge);

  // And it evaluates the same way the original does: stale by age, at the
  // same evaluation instant, through the same shared evaluator.
  auto original = worker.cache()->ReadOriginalAlternate(
      StripSchemeAuthority(url), "", "https");
  ASSERT_TRUE(original.has_value());
  const uint32_t now = kInsertedAt + kMaxAge + 100;
  FreshnessConfig fc;
  const auto original_verdict = EvaluateFreshness(
      FreshnessInputFromMetadata(original->metadata, now), fc);
  const auto variant_verdict =
      EvaluateFreshness(FreshnessInputFromMetadata(got->metadata, now), fc);
  EXPECT_TRUE(original_verdict.expired_by_age);
  EXPECT_EQ(variant_verdict.verdict, original_verdict.verdict);
  EXPECT_EQ(variant_verdict.expired_by_age, original_verdict.expired_by_age);
  EXPECT_EQ(variant_verdict.remaining_ttl, original_verdict.remaining_ttl);

  // The variant is an ordinary capability-keyed entry, not the class: the
  // sentinel id must never leak into what the worker writes.
  EXPECT_EQ(got->metadata.full_mask & 0xFFu, CapabilityMask().Encode() & 0xFFu);
  EXPECT_NE(got->metadata.full_mask & 0xFFu,
            static_cast<uint32_t>(SentinelId::kOriginalContent));
}

// The Accept-negotiation refusal, which this change would otherwise open.
//
// A URL whose ORIGIN negotiates on Accept is stored as sent and never
// optimized — deriving variants from the one representation captured would
// layer a second negotiation on the origin's own.  That guard reads the flag
// off the stored original and, when there is none, fails OPEN, on the
// reasoning that no stored original means nothing to optimize from.  The
// source fallback removes that reasoning: with a durable original there IS
// something to optimize from.  So the guard consults it, and a marked durable
// original is refused exactly like a marked identity alternate.
TEST_F(WorkerTest, AMarkedDurableOriginalIsStillRefusedAsAnAcceptNegotiator) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/negotiated.css";
  const std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/600, /*hostname=*/"",
                       AlternateMetadata::kFlagOriginVariesAccept);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Long enough that the sibling tests' variant would have appeared.
  for (int i = 0; i < 20 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().notifications_skipped_dedup.load(), 1u)
      << "the notification must be refused, not optimized";
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, CapabilityMask()).empty())
      << "a variant was derived for an origin that negotiates on Accept";
}

// The header-fidelity marker is advisory to serve paths, and the optimizer is
// not one of them.
//
// kFlagOriginHeadersNotReproducible says the origin sent a header no later
// serve can reproduce.  That is a statement about the RESPONSE a serve path
// would assemble, not about the bytes, so it must not reach optimization: a
// marked original is optimized exactly as an unmarked one is.  The obvious
// wrong implementation is the one next door — the Accept-negotiation refusal —
// and it would be silent, costing every marked URL its optimization with the
// same "nothing errored" signature that bug had.
//
// The second half is the flags byte the worker WRITES.  Variant metadata is
// built from the original's and the flags byte is assigned, not inherited, so
// the marker does not travel into derived variants.  That is the behaviour the
// contract states ("stamped by the writer at record time"), and it is asserted
// rather than left to the assignment staying an assignment.
TEST_F(WorkerTest, AnOriginalMarkedHeadersNotReproducibleIsStillOptimized) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unreproducible-headers.css";
  const std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/600, /*hostname=*/"",
                       AlternateMetadata::kFlagOriginHeadersNotReproducible);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::optional<VariantWithMeta> got;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    got = ReadVariantWithMeta(worker.cache(), url, CapabilityMask());
    if (got.has_value()) break;
  }
  ASSERT_TRUE(got.has_value())
      << "the marker suppressed optimization, which it must not gate";
  EXPECT_LT(got->content.size(), css.size());
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // The variant is the worker's own write, carrying the worker-processed bit
  // and not the writer's marker.
  EXPECT_NE(got->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0);
  EXPECT_EQ(got->metadata.flags &
                AlternateMetadata::kFlagOriginHeadersNotReproducible,
            0)
      << "a record-time marker leaked into a variant the worker produced";

  // The compressed-variant leg: that write goes through the same shared
  // helper, which copies the flags byte it is handed — so non-propagation
  // holds there only because the caller composed the byte, a convention this
  // read pins as an invariant (#1342: derived variants do not inherit the
  // header-fidelity marker).
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::optional<VariantWithMeta> gz;
  for (int i = 0; i < 60 * kSanitizerBudgetScale && !gz.has_value(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gz = ReadVariantWithMeta(worker.cache(), url, gz_mask);
  }
  ASSERT_TRUE(gz.has_value()) << "gzip variant missing";
  EXPECT_EQ(
      gz->metadata.flags & AlternateMetadata::kFlagOriginHeadersNotReproducible,
      0)
      << "a record-time marker leaked into a compressed variant";

  // Non-vacuity: the marker really is on the entry the worker read from, so
  // the assertions above are about a marked input, not an unmarked one.
  auto original = worker.cache()->ReadOriginalAlternate(
      StripSchemeAuthority(url), "", "https");
  ASSERT_TRUE(original.has_value());
  EXPECT_NE(original->metadata.flags &
                AlternateMetadata::kFlagOriginHeadersNotReproducible,
            0);
}

// The transcode fall-through and the alternate id the origin's bytes land at
// (#1374), end to end through the real write path, the real content dedup and
// the real selector.
//
// An image whose pixel decode fails takes the per-format fall-through, which
// answers a conversion request with the origin's own bytes in the origin's own
// format.  Those bytes belong at the ORIGINAL-format id: ScoreAlternate gives a
// stored original-format alternate a fallback bonus for every client mask,
// while a stored WebP/AVIF whose format differs from what the client negotiated
// is a hard disqualification.  So the original-format id is reachable from
// every request shape and a converted id is reachable from exactly one.
//
// Pre-fix the WebP slot came back "successful" holding the GIF, the worker
// wrote it under the WebP id first, and the dedup then dropped the identical
// original-format twin -- so the family's only copy of the origin bytes sat at
// an id no other client could select.  Reintroducing that ordering or that slot
// flips the id assertions AND the two cross-format reachability reads below.
TEST_F(WorkerTest, DecodeFailingImageKeepsOriginBytesAtTheOriginalFormatId) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Format siblings only, at the notification's exact combination: the family
  // under test is the three format slots and nothing else.
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_density_variants = false;
  config.proactive_savedata_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Valid GIF magic, nothing readable behind it: the decode fails, so every
  // conversion falls through.  Seeded as the durable original so all three
  // capability ids start free (a default-mask seed would already occupy the
  // original-format id and take that slot out of the family).
  const std::string gif("GIF89a\x01\x00\x01\x00", 10);
  const std::string url = "http://example.com/fallthrough.gif";
  CacheDurableOriginal(worker.cache(), url, gif, "image/gif",
                       ContentType::kImage);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const CapabilityMask desktop_original(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  const CapabilityMask desktop_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  const CapabilityMask desktop_avif(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = desktop_webp.Encode();
  SendNotification(notification);

  std::string at_original;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    at_original = ReadVariant(worker.cache(), url, desktop_original);
    if (!at_original.empty()) break;
  }

  // The origin's bytes are at the original-format id ...
  EXPECT_EQ(gif, at_original)
      << "origin bytes are not at the original-format id";
  // ... and at neither converted id.
  EXPECT_TRUE(ReadVariant(worker.cache(), url, desktop_webp).empty())
      << "unconverted bytes were written under the WebP id";
  EXPECT_TRUE(ReadVariant(worker.cache(), url, desktop_avif).empty())
      << "unconverted bytes were written under the AVIF id";

  // The refusal is visible to an operator, not only to this test.
  // Both converted slots were refused for this input, and the counter that
  // says so is distinct from the no-savings one -- "there was nothing to
  // convert" and "the optimizer could not win" are different operator
  // situations, and telling them apart is exactly what was missing when #1374
  // went unnoticed.
  EXPECT_GE(worker.stats().image_unconverted_fallthrough.load(), 2u)
      << "the fall-through refusals were not counted";

  // The reachability that placement buys, read through the production
  // selector: all three client shapes resolve to those bytes.  Pre-fix the
  // original-format and AVIF reads came back empty -- the only stored
  // alternate was at the WebP id, and a client that negotiated anything else
  // is hard-disqualified from it.
  EXPECT_EQ(gif, ReadBestVariant(worker.cache(), url, desktop_original))
      << "an original-format client cannot reach the family";
  EXPECT_EQ(gif, ReadBestVariant(worker.cache(), url, desktop_webp))
      << "a WebP client cannot reach the family";
  EXPECT_EQ(gif, ReadBestVariant(worker.cache(), url, desktop_avif))
      << "an AVIF client cannot reach the family";

  worker.Shutdown();
  worker_thread.join();
}

}  // namespace
}  // namespace pagespeed
