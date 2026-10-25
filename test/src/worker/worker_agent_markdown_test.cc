// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Agent markdown and content-hash idempotency tests
//
// - Content-hash self-purge and own-write-failure loops
// - Agent markdown variant durability across origin refreshes
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <cstdint>
#include <span>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/worker.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

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

}  // namespace
}  // namespace pagespeed
