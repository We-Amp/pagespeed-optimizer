// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The cache side of a front end's site-scoped HTML store: a sidecar paired
// with the durable original it belongs to, the withheld marker (never
// readable as a page, cleared by a re-store of the original), and the
// per-site purge epoch (a max-merge, clamped to the present, safe against
// racing writers on separate handles of one volume).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

const std::string kSiteA(64, 'a');
const std::string kSiteB = std::string(63, 'a') + "b";
constexpr std::string_view kUrl = "/page.html";
constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";
constexpr AlternateId kMarkerId =
    static_cast<AlternateId>(SentinelId::kAgentMarkdownWithheld);
constexpr AlternateId kMarkdownId =
    static_cast<AlternateId>(SentinelId::kAgentMarkdown);

std::array<std::byte, AlternateMetadata::kHashSize> Hash(uint8_t seed) {
  std::array<std::byte, AlternateMetadata::kHashSize> h{};
  for (size_t i = 0; i < h.size(); ++i) {
    h[i] = static_cast<std::byte>(seed + i + 1);
  }
  return h;
}

class SiteHtmlStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
    cache_ = Open();
    ASSERT_NE(cache_, nullptr);
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  // Another handle on the same volume, the way a second process opens it.
  std::unique_ptr<PageSpeedCache> Open() {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    if (!result.has_value()) return nullptr;
    return std::move(*result);
  }

  void WriteOriginal(std::string_view site, std::string_view body,
                     const std::array<std::byte, 32>* hash = nullptr) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
    meta.content_type = ContentType::kHtml;
    meta.origin_content_type = "text/html";
    meta.flags = AlternateMetadata::kFlagHeadersClassified;
    if (hash != nullptr) meta.origin_html_hash = *hash;
    auto wh = cache_->WriteOriginalAlternate(kUrl, kHost, kScheme, body.size(),
                                             meta, site);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  void WriteMarker(std::string_view site) {
    const std::string payload = "\x01marker";
    auto wh = cache_->WriteSentinel(kUrl, kHost, kScheme,
                                    SentinelId::kAgentMarkdownWithheld,
                                    payload.size(), site);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(payload))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  void WriteMarkdown(std::string_view site, std::string_view body) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = "text/markdown";
    auto wh = cache_->WriteAgentAlternate(kUrl, kHost, kScheme, body.size(),
                                          meta, site);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  bool Exists(std::string_view site, AlternateId id) {
    return cache_->AlternateExists(kUrl, kHost, kScheme, id, site);
  }

  std::string temp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

// ------------------------------------------------------------ pairing

TEST_F(SiteHtmlStoreTest, APairedSidecarIsStoredInFormat2WithItsHash) {
  const auto hash = Hash(1);
  const std::vector<HeaderField> headers = {
      {"Content-Type", "text/html"}, {"Referrer-Policy", "no-referrer"}};
  auto written =
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA, hash);
  ASSERT_TRUE(written.has_value());
  EXPECT_TRUE(written->stored);

  auto read = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(read.has_value());
  const auto payload = PageSpeedCache::HeadersSidecarPayload(*read);
  EXPECT_EQ(static_cast<unsigned char>(payload[0]),
            kHeadersSidecarPairedFormatVersion);
  auto block = ParseHeadersSidecarBlock(payload);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block->pairing_hash.has_value());
  EXPECT_EQ(*block->pairing_hash, hash);
  ASSERT_EQ(block->fields.size(), 1u);
  EXPECT_EQ(block->fields[0].name, "Referrer-Policy");
}

TEST_F(SiteHtmlStoreTest, APairedWriteStoresABlockWithNothingToCarry) {
  // Content-Type is reproduced by the entry metadata, so there is nothing
  // for the block to carry. Unpaired, that is no entry at all; paired, it is
  // a recorded, provable "nothing extra" for this original.
  const std::vector<HeaderField> headers = {{"Content-Type", "text/html"}};
  auto unpaired =
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA);
  ASSERT_TRUE(unpaired.has_value());
  EXPECT_FALSE(unpaired->stored);
  EXPECT_FALSE(
      cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA).has_value());

  auto paired = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers,
                                            kSiteA, Hash(2));
  ASSERT_TRUE(paired.has_value());
  EXPECT_TRUE(paired->stored);
  auto read = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(read.has_value());
  auto block =
      ParseHeadersSidecarBlock(PageSpeedCache::HeadersSidecarPayload(*read));
  ASSERT_TRUE(block.has_value());
  EXPECT_TRUE(block->fields.empty());
  EXPECT_TRUE(block->pairing_hash.has_value());
}

TEST_F(SiteHtmlStoreTest, ARefusalWritesNothingEvenWhenPaired) {
  const std::vector<HeaderField> headers = {{"Set-Cookie", "a=b"}};
  auto written = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers,
                                             kSiteA, Hash(3));
  ASSERT_TRUE(written.has_value());
  EXPECT_FALSE(written->stored);
  EXPECT_FALSE(
      cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA).has_value());
}

TEST_F(SiteHtmlStoreTest, AMalformedPairingHashIsRefused) {
  const std::vector<HeaderField> headers = {{"Referrer-Policy", "x"}};
  const std::array<std::byte, 32> zero{};
  EXPECT_FALSE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA, zero)
          .has_value());
  const std::array<std::byte, 31> short_hash{std::byte{1}};
  EXPECT_FALSE(cache_
                   ->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA,
                                         short_hash)
                   .has_value());
}

// ------------------------------------------------------------ withheld marker

TEST_F(SiteHtmlStoreTest, NoSelectionReadEverReturnsTheMarker) {
  WriteMarker(kSiteA);
  ASSERT_TRUE(Exists(kSiteA, kMarkerId));
  // Only the marker is stored: every selection read misses rather than
  // handing it out as a page, with and without the agent flag.
  for (bool agent : {false, true}) {
    SCOPED_TRACE(agent);
    EXPECT_FALSE(cache_
                     ->ReadBestAlternate(kUrl, kHost, kScheme, CapabilityMask(),
                                         agent, kSiteA)
                     .has_value());
    EXPECT_FALSE(cache_
                     ->ReadBestAlternateAgent(kUrl, kHost, kScheme,
                                              CapabilityMask(), agent, kSiteA)
                     .has_value());
  }
  // And no exact-id reader of another class returns it.
  EXPECT_FALSE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkdownId, kSiteA)
                   .has_value());
  EXPECT_FALSE(
      cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA).has_value());
  EXPECT_FALSE(
      cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA).has_value());
  // Asked for by its own id, it is there.
  EXPECT_TRUE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkerId, kSiteA)
                  .has_value());
}

TEST_F(SiteHtmlStoreTest, BesideAMarkdownCopyTheAgentReadGetsTheCopy) {
  WriteMarkdown(kSiteA, "# title");
  WriteMarker(kSiteA);
  auto read = cache_->ReadBestAlternate(kUrl, kHost, kScheme, CapabilityMask(),
                                        /*agent_request_entitled=*/true,
                                        kSiteA);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->metadata.full_mask & 0xFF,
            static_cast<uint32_t>(SentinelId::kAgentMarkdown));
}

TEST_F(SiteHtmlStoreTest, ARestoreOfTheOriginalClearsTheMarker) {
  const auto h = Hash(4);
  WriteOriginal(kSiteA, "<html>v1</html>", &h);
  WriteMarker(kSiteA);
  WriteMarker(kSiteB);
  ASSERT_TRUE(Exists(kSiteA, kMarkerId));

  // Same bytes, same hash: still cleared — the marker records a refusal of
  // the copy that was read, not a property of the page.
  WriteOriginal(kSiteA, "<html>v1</html>", &h);
  EXPECT_FALSE(Exists(kSiteA, kMarkerId));
  EXPECT_TRUE(Exists(kSiteA, static_cast<AlternateId>(
                                 SentinelId::kOriginalContent)));
  // Another site's marker is another key.
  EXPECT_TRUE(Exists(kSiteB, kMarkerId));
}

TEST_F(SiteHtmlStoreTest, TheOriginalStampSurvivesReadsOnEveryHandle) {
  const auto h = Hash(5);
  WriteOriginal(kSiteA, "<html>stamped</html>", &h);
  // Twice on the writing handle (the second read can come from its RAM
  // tier), then on a fresh handle that has only the volume.
  for (int i = 0; i < 2; ++i) {
    auto read = cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->metadata.origin_html_hash, h);
  }
  auto other = Open();
  ASSERT_NE(other, nullptr);
  auto read = other->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->metadata.origin_html_hash, h);
}

// ------------------------------------------------------------ purge epoch

constexpr uint64_t kNow = 1'800'000'000;

TEST_F(SiteHtmlStoreTest, TheEpochRoundTripsPerSite) {
  auto none = cache_->ReadSitePurgeEpoch(kSiteA);
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(*none, 0u);

  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 100, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 100);
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteB).value(), 0u)
      << "another site's epoch moved";

  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteB, kNow - 7, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 100);
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteB).value(), kNow - 7);

  auto other = Open();
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(other->ReadSitePurgeEpoch(kSiteA).value(), kNow - 100);
}

TEST_F(SiteHtmlStoreTest, ALowerEpochIsASuccessfulNoOp) {
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 10, kNow));
  EXPECT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 500, kNow));
  EXPECT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 10, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 10);
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow);
}

TEST_F(SiteHtmlStoreTest, AFutureEpochIsRefused) {
  const uint64_t skew = PageSpeedCache::kSitePurgeEpochSkewSeconds;
  EXPECT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow + skew, kNow));
  auto refused = cache_->WriteSitePurgeEpoch(kSiteA, kNow + skew + 1, kNow);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error(), cyclone::CacheError::InvalidArgument);
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow + skew);
}

TEST_F(SiteHtmlStoreTest, AnEmptySiteHasNoEpoch) {
  EXPECT_FALSE(cache_->WriteSitePurgeEpoch("", kNow, kNow).has_value());
  EXPECT_FALSE(cache_->ReadSitePurgeEpoch("").has_value());
}

TEST_F(SiteHtmlStoreTest, RemovingTheUrlLeavesTheEpoch) {
  WriteOriginal(kSiteA, "<html>x</html>");
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 1, kNow));
  ASSERT_TRUE(cache_->Remove(kUrl, kHost, kScheme, kSiteA));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 1);
}

TEST_F(SiteHtmlStoreTest, ManyRisingWritesStayWithinTheSlotBound) {
  for (uint64_t i = 0; i < 3 * PageSpeedCache::kSitePurgeEpochSlots; ++i) {
    ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 1000 + i, kNow));
  }
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(),
            kNow - 1000 + 3 * PageSpeedCache::kSitePurgeEpochSlots - 1);
}

TEST_F(SiteHtmlStoreTest, RacingWritersOnSeparateHandlesLeaveTheLarger) {
  // Two handles on one volume stand in for two processes: each thread has
  // its own handle and its own in-process lock, so only the storage layer
  // is shared, as it is across processes.
  auto other = Open();
  ASSERT_NE(other, nullptr);
  constexpr int kRounds = 200;
  for (int round = 0; round < kRounds; ++round) {
    const uint64_t base = kNow - 100'000 + static_cast<uint64_t>(round) * 10;
    // Alternate which handle carries the larger value.
    PageSpeedCache* low = (round % 2 == 0) ? cache_.get() : other.get();
    PageSpeedCache* high = (round % 2 == 0) ? other.get() : cache_.get();
    std::atomic<int> ready{0};
    auto run = [&ready](PageSpeedCache* c, uint64_t value) {
      ready.fetch_add(1);
      while (ready.load() < 2) {
      }
      EXPECT_TRUE(c->WriteSitePurgeEpoch(kSiteA, value, kNow).has_value());
    };
    std::thread t1(run, low, base + 1);
    std::thread t2(run, high, base + 3);
    t1.join();
    t2.join();
    ASSERT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), base + 3)
        << "round " << round;
    ASSERT_EQ(other->ReadSitePurgeEpoch(kSiteA).value(), base + 3)
        << "round " << round;
  }
}

}  // namespace
}  // namespace pagespeed
