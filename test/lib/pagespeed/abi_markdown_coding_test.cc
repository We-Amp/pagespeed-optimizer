// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The PS_API 1.16 compressed Markdown copies: the coding choice, the binding
// check in the stored facts, the coding in the serve verdict, the per-coding
// entity tag, the claimed ids, the compress-demand flag and the published
// codings.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/pagespeed/pagespeed.h"
#include "sha256.hpp"
#include "src/worker/shared_config.h"
#include "test/test_util/temp_dir.h"

namespace {

const std::string kSite(PS_SITE_ID_CHARS, 'c');
constexpr char kUrl[] = "/page.html";
constexpr char kHost[] = "example.com";
constexpr char kScheme[] = "https";
constexpr char kPage[] = "<html><body><h1>A page</h1></body></html>";
constexpr char kPlain[] = "# A page\n\nThe plain Markdown copy of the page.\n";

using Hash = std::array<std::byte, 32>;

Hash Sha(std::string_view s) {
  return cyclone::crypto::SHA256::hash(std::as_bytes(std::span(s)));
}

Hash Other(uint8_t seed) {
  Hash h{};
  for (size_t i = 0; i < h.size(); ++i) {
    h[i] = static_cast<std::byte>(seed + i + 1);
  }
  return h;
}

std::string Order(std::vector<const char*> lines) {
  int order[3] = {-1, -1, -1};
  const int n = ps_markdown_select_encoding(
      lines.empty() ? nullptr : lines.data(), lines.size(), order);
  if (n < 0) return "error";
  if (n == 0) return "none";
  return order[0] == PS_MARKDOWN_ENCODING_BR     ? "br"
         : order[0] == PS_MARKDOWN_ENCODING_GZIP ? "gzip"
                                                 : "identity";
}

// What the optimizer writes under a coded id, and the bindings it carries.
struct Coded {
  Hash origin = Sha(kPage);
  Hash source = Sha(kPlain);
  std::string bytes = "\x1f\x8b-coded-bytes";
  std::string content_type = "text/markdown";
};

class AbiMarkdownCodingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = pagespeed::test::MakeTempDir();
    path_ = dir_ + "/cache.vol";
    ps_cache_config_t config;
    ps_cache_config_init(&config);
    config.volume_path = path_.c_str();
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    ASSERT_EQ(ps_cache_open(&config, &cache_), PS_OK);
    pagespeed::PageSpeedCacheConfig cpp_config;
    cpp_config.volume_path = path_;
    cpp_config.volume_size = config.volume_size;
    cpp_config.ram_cache_size = 0;  // every read below comes from the volume
    auto cpp = pagespeed::PageSpeedCache::Create(cpp_config);
    ASSERT_TRUE(cpp.has_value());
    writer_ = std::move(*cpp);
  }

  void TearDown() override {
    writer_.reset();
    ps_cache_close(cache_);
    std::filesystem::remove_all(dir_);
  }

  static bool Fill(
      std::expected<pagespeed::WriteResult, cyclone::CacheError> wh,
      std::string_view bytes) {
    if (!wh.has_value()) return false;
    if (!bytes.empty() &&
        !wh->write_sync(std::as_bytes(std::span(bytes))).has_value()) {
      return false;
    }
    return wh->close_sync().has_value();
  }

  // What the module's HTML store writes: fresh, anonymous, under this policy.
  void StorePage() {
    pagespeed::AlternateMetadata meta;
    meta.full_mask = PS_SENTINEL_ORIGINAL;
    meta.content_type = pagespeed::ContentType::kHtml;
    meta.origin_content_type = "text/html";
    meta.flags = PS_FLAG_HEADERS_CLASSIFIED | PS_FLAG_HTML_STORE_V2 |
                 PS_FLAG_ANONYMOUS_RECORDED;
    meta.origin_max_age = 3600;
    meta.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC;
    meta.cache_inserted_at = static_cast<uint32_t>(std::time(nullptr));
    meta.origin_html_hash = Sha(kPage);
    ASSERT_TRUE(Fill(writer_->WriteOriginalAlternate(
                         kUrl, kHost, kScheme, std::strlen(kPage), meta, kSite),
                     kPage));
  }

  // The plain copy, as the optimizer writes it.
  // With `hashed` false, as the browser render path writes it: without its
  // own content hash.
  void StorePlain(bool hashed = true) {
    pagespeed::AlternateMetadata meta;
    meta.full_mask = 0x7C;
    meta.content_type = pagespeed::ContentType::kOther;
    meta.origin_content_type = "text/markdown";
    meta.flags = PS_FLAG_WORKER_PROCESSED;
    meta.origin_html_hash = Sha(kPage);
    if (hashed) meta.render_source_hash = Sha(kPlain);
    ASSERT_TRUE(
        Fill(writer_->WriteAgentAlternate(kUrl, kHost, kScheme,
                                          std::strlen(kPlain), meta, kSite),
             kPlain));
  }

  void StoreCoded(int coding, const Coded& c = {}) {
    pagespeed::AlternateMetadata meta;
    meta.full_mask = coding == PS_MARKDOWN_ENCODING_BR
                         ? PS_SENTINEL_MARKDOWN_BR
                         : PS_SENTINEL_MARKDOWN_GZIP;
    meta.content_type = pagespeed::ContentType::kOther;
    meta.origin_content_type = c.content_type;
    meta.flags = PS_FLAG_WORKER_PROCESSED;
    meta.origin_html_hash = c.origin;
    meta.render_source_hash = c.source;
    ASSERT_TRUE(Fill(writer_->WriteAgentCodedAlternate(
                         kUrl, kHost, kScheme, static_cast<uint8_t>(coding),
                         c.bytes.size(), meta, kSite),
                     c.bytes));
    writer_->EvictAlternateFromRamCache(kUrl, kHost, kScheme, meta.full_mask,
                                        kSite);
  }

  // Bytes that are not a coded copy, written straight under the id.
  void StoreForeign(pagespeed::SentinelId id) {
    const std::string junk = "not a Markdown copy";
    ASSERT_TRUE(Fill(
        writer_->WriteSentinel(kUrl, kHost, kScheme, id, junk.size(), kSite),
        junk));
  }

  ps_read_result_t* Read(uint8_t id) {
    ps_read_result_t* r = nullptr;
    if (ps_cache_read_alternate_site(cache_, kUrl, kHost, kScheme,
                                     kSite.c_str(), id, &r) != PS_OK) {
      return nullptr;
    }
    return r;
  }

  // The facts with the read of `coding`'s copy (none for identity).
  ps_html_stored_facts_t Facts(int coding,
                               std::optional<uint8_t> read_id = std::nullopt) {
    ps_read_result_t* original = Read(PS_SENTINEL_ORIGINAL);
    ps_read_result_t* plain = Read(0x7C);
    ps_read_result_t* coded = nullptr;
    if (coding != PS_MARKDOWN_ENCODING_IDENTITY) {
      coded = Read(read_id.value_or(coding == PS_MARKDOWN_ENCODING_BR
                                        ? PS_SENTINEL_MARKDOWN_BR
                                        : PS_SENTINEL_MARKDOWN_GZIP));
    }
    ps_html_stored_facts_t facts{};
    facts.struct_size = sizeof(facts);
    EXPECT_EQ(ps_html_stored_facts_from_reads_coded(
                  original, plain, nullptr, 0, nullptr, coded, coding, &facts),
              0);
    ps_read_free(coded);
    ps_read_free(plain);
    ps_read_free(original);
    return facts;
  }

  static ps_html_serve_request_t Request(int encoding, uint32_t flags) {
    ps_html_serve_request_t r{};
    r.struct_size = sizeof(r);
    r.mode = PS_HTML_CACHE_SERVE_VARIANTS;
    r.method = "GET";
    r.accept = "text/markdown";
    r.now_seconds = static_cast<uint32_t>(std::time(nullptr));
    r.request_flags = flags;
    r.markdown_encoding = static_cast<uint32_t>(encoding);
    return r;
  }

  static ps_html_serve_verdict_t Classify(const ps_html_serve_request_t& r,
                                          const ps_html_stored_facts_t& f) {
    ps_html_serve_verdict_t v{};
    v.struct_size = sizeof(v);
    EXPECT_EQ(ps_html_serve_classify(&r, &f, &v), 0);
    return v;
  }

  static std::string Tag(int coding) {
    const Hash b = Sha(kPage);
    const Hash m = Sha(kPlain);
    char buf[PS_HTML_MARKDOWN_ETAG_CHARS + 1];
    size_t len = 0;
    EXPECT_EQ(
        ps_html_markdown_etag_coded(reinterpret_cast<const uint8_t*>(b.data()),
                                    reinterpret_cast<const uint8_t*>(m.data()),
                                    coding, buf, sizeof(buf), &len),
        0);
    return buf;
  }

  // A coded copy that may not be sent: the plain copy with IDENTITY_OK, and
  // no_acceptable_encoding without it.
  static void ExpectNotSent(const ps_html_stored_facts_t& facts, int coding) {
    const ps_html_serve_verdict_t with =
        Classify(Request(coding, PS_HTML_SERVE_REQUEST_IDENTITY_OK), facts);
    EXPECT_EQ(with.action, PS_HTML_SERVE_ACTION_SERVE_MARKDOWN);
    EXPECT_EQ(with.markdown_encoding, PS_MARKDOWN_ENCODING_IDENTITY);
    EXPECT_EQ(std::string(with.etag), Tag(PS_MARKDOWN_ENCODING_IDENTITY));
    const ps_html_serve_verdict_t without = Classify(Request(coding, 0), facts);
    EXPECT_EQ(without.action, PS_HTML_SERVE_ACTION_FALL_THROUGH);
    EXPECT_STREQ(ps_html_serve_reason_name(without.reason),
                 "no_acceptable_encoding");
    EXPECT_EQ(without.markdown_encoding, 0);
    EXPECT_STREQ(without.etag, "");
  }

  std::string dir_;
  std::string path_;
  ps_cache_t* cache_ = nullptr;
  std::unique_ptr<pagespeed::PageSpeedCache> writer_;
};

constexpr int kCodings[] = {PS_MARKDOWN_ENCODING_GZIP, PS_MARKDOWN_ENCODING_BR};

uint32_t BoundBit(int coding) {
  return coding == PS_MARKDOWN_ENCODING_BR ? PS_HTML_FACTS_BR_COPY_BOUND
                                           : PS_HTML_FACTS_GZIP_COPY_BOUND;
}

uint32_t NotSmallerBit(int coding) {
  return coding == PS_MARKDOWN_ENCODING_BR ? PS_HTML_FACTS_BR_NOT_SMALLER
                                           : PS_HTML_FACTS_GZIP_NOT_SMALLER;
}

// ------------------------------------------------------------ coding choice

TEST(AbiMarkdownCodingChoice, AcceptanceRows) {
  struct Row {
    std::vector<const char*> lines;
    const char* first;
  };
  const Row rows[] = {
      {{"br, gzip"}, "br"},
      {{"gzip"}, "gzip"},
      {{"gzip;q=1, br;q=0.5"}, "gzip"},
      {{"br;q=0, gzip"}, "gzip"},
      {{"br;q=0"}, "identity"},
      {{"br;q=0.0"}, "identity"},
      {{"br;q=0.001, gzip;q=0.001"}, "br"},
      {{"gzip;q=1.000, br;q=0.999"}, "gzip"},
      {{"*"}, "br"},
      {{"*;q=0, identity"}, "identity"},
      {{"identity;q=0"}, "none"},
      {{"br, identity;q=0"}, "br"},
      {{"X-GZIP"}, "gzip"},
      {{}, "identity"},
      {{""}, "identity"},
      {{"gzip;q=abc"}, "identity"},
  };
  for (const Row& row : rows) {
    SCOPED_TRACE(row.lines.empty() ? "(absent)" : row.lines[0]);
    EXPECT_EQ(Order(row.lines), row.first);
  }
}

TEST(AbiMarkdownCodingChoice, WritesTheOrderAndRefusesMalformedCalls) {
  int order[3] = {-1, -1, -1};
  const char* lines[] = {"gzip;q=0.5", nullptr, "br"};
  ASSERT_EQ(ps_markdown_select_encoding(lines, 3, order), 3);
  EXPECT_EQ(order[0], PS_MARKDOWN_ENCODING_BR);
  EXPECT_EQ(order[1], PS_MARKDOWN_ENCODING_GZIP);
  EXPECT_EQ(order[2], PS_MARKDOWN_ENCODING_IDENTITY);
  EXPECT_EQ(ps_markdown_select_encoding(lines, 3, nullptr), -1);
  EXPECT_EQ(ps_markdown_select_encoding(nullptr, 1, order), -1);
  EXPECT_EQ(ps_markdown_select_encoding(nullptr, 0, order), 1);
  EXPECT_EQ(order[0], PS_MARKDOWN_ENCODING_IDENTITY);
}

// ------------------------------------------------------------ binding rows

TEST_F(AbiMarkdownCodingTest, ABoundCodedCopyGivesItsCodingAndTag) {
  StorePage();
  StorePlain();
  for (int coding : kCodings) {
    SCOPED_TRACE(coding);
    StoreCoded(coding);
    const ps_html_stored_facts_t facts = Facts(coding);
    EXPECT_NE(facts.flags & PS_HTML_FACTS_BINDING_LIVE, 0u);
    EXPECT_NE(facts.flags & BoundBit(coding), 0u);
    EXPECT_EQ(facts.flags & NotSmallerBit(coding), 0u);
    const ps_html_serve_verdict_t v = Classify(Request(coding, 0), facts);
    EXPECT_EQ(v.action, PS_HTML_SERVE_ACTION_SERVE_MARKDOWN);
    EXPECT_EQ(v.markdown_encoding, coding);
    EXPECT_EQ(std::string(v.etag), Tag(coding));
  }
}

TEST_F(AbiMarkdownCodingTest, AMissingCopyIsNotSent) {
  StorePage();
  StorePlain();
  for (int coding : kCodings) {
    SCOPED_TRACE(coding);
    const ps_html_stored_facts_t facts = Facts(coding);
    EXPECT_EQ(facts.flags & (BoundBit(coding) | NotSmallerBit(coding)), 0u);
    ExpectNotSent(facts, coding);
  }
}

TEST_F(AbiMarkdownCodingTest, AnUnboundCopyIsNotSent) {
  StorePage();
  StorePlain();
  for (int coding : kCodings) {
    for (const char* which : {"render_source_hash", "origin_html_hash"}) {
      SCOPED_TRACE(std::string(which) + " " + std::to_string(coding));
      Coded c;
      if (std::string_view(which) == "render_source_hash") {
        c.source = Other(7);
      } else {
        c.origin = Other(9);
      }
      StoreCoded(coding, c);
      const ps_html_stored_facts_t facts = Facts(coding);
      EXPECT_EQ(facts.flags & BoundBit(coding), 0u);
      ExpectNotSent(facts, coding);
    }
  }
}

TEST_F(AbiMarkdownCodingTest, ACopyBehindAnUnboundPlainCopyIsNotSent) {
  // The page was stored again with other bytes: the plain copy is unbound,
  // and so is every coded copy of it, even one naming the new page.
  StorePage();
  StorePlain();
  for (int coding : kCodings) StoreCoded(coding);
  pagespeed::AlternateMetadata meta;
  meta.full_mask = PS_SENTINEL_ORIGINAL;
  meta.content_type = pagespeed::ContentType::kHtml;
  meta.origin_content_type = "text/html";
  meta.flags = PS_FLAG_HEADERS_CLASSIFIED | PS_FLAG_HTML_STORE_V2 |
               PS_FLAG_ANONYMOUS_RECORDED;
  meta.origin_max_age = 3600;
  meta.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC;
  meta.cache_inserted_at = static_cast<uint32_t>(std::time(nullptr));
  meta.origin_html_hash = Other(3);
  ASSERT_TRUE(Fill(writer_->WriteOriginalAlternate(
                       kUrl, kHost, kScheme, std::strlen(kPage), meta, kSite),
                   kPage));
  for (int coding : kCodings) {
    const ps_html_stored_facts_t facts = Facts(coding);
    EXPECT_EQ(facts.flags & PS_HTML_FACTS_BINDING_LIVE, 0u);
    EXPECT_EQ(facts.flags & BoundBit(coding), 0u);
    const ps_html_serve_verdict_t v = Classify(Request(coding, 0), facts);
    EXPECT_STREQ(ps_html_serve_reason_name(v.reason), "binding_stale");
  }
}

TEST_F(AbiMarkdownCodingTest, APlainCopyWithoutAHashBindsNoCodedCopy) {
  // A plain copy written without its own hash, beside a coded copy whose
  // metadata names the page and the same (zero) hash: two equal zero hashes
  // are not a binding, so the coded copy is never sent.
  StorePage();
  StorePlain(/*hashed=*/false);
  for (int coding : kCodings) {
    SCOPED_TRACE(coding);
    Coded zero;
    zero.source = Hash{};
    StoreCoded(coding, zero);
    const ps_html_stored_facts_t facts = Facts(coding);
    EXPECT_NE(facts.flags & PS_HTML_FACTS_BINDING_LIVE, 0u);
    EXPECT_EQ(facts.flags & (BoundBit(coding) | NotSmallerBit(coding)), 0u);
    const ps_html_serve_verdict_t with =
        Classify(Request(coding, PS_HTML_SERVE_REQUEST_IDENTITY_OK), facts);
    EXPECT_EQ(with.action, PS_HTML_SERVE_ACTION_SERVE_MARKDOWN);
    EXPECT_EQ(with.markdown_encoding, PS_MARKDOWN_ENCODING_IDENTITY);
    EXPECT_STREQ(with.etag, "") << "no hash, no tag";
    const ps_html_serve_verdict_t without = Classify(Request(coding, 0), facts);
    EXPECT_STREQ(ps_html_serve_reason_name(without.reason),
                 "no_acceptable_encoding");
  }
}

TEST_F(AbiMarkdownCodingTest, ForeignBytesAndAnotherCodingsEntryAreMissing) {
  StorePage();
  StorePlain();
  // Bytes with no entry metadata under each id: the read is a miss.
  StoreForeign(pagespeed::SentinelId::kAgentMarkdownBrotli);
  StoreForeign(pagespeed::SentinelId::kAgentMarkdownGzip);
  EXPECT_EQ(Read(PS_SENTINEL_MARKDOWN_BR), nullptr);
  EXPECT_EQ(Read(PS_SENTINEL_MARKDOWN_GZIP), nullptr);
  for (int coding : kCodings) {
    SCOPED_TRACE(coding);
    const ps_html_stored_facts_t facts = Facts(coding);
    EXPECT_EQ(facts.flags & BoundBit(coding), 0u);
    ExpectNotSent(facts, coding);
  }
  // A bound gzip copy handed over as the brotli read (its metadata names the
  // other id), and a copy of another content type: neither binds.
  StoreCoded(PS_MARKDOWN_ENCODING_GZIP);
  {
    const ps_html_stored_facts_t facts =
        Facts(PS_MARKDOWN_ENCODING_BR, PS_SENTINEL_MARKDOWN_GZIP);
    EXPECT_EQ(facts.flags &
                  (PS_HTML_FACTS_BR_COPY_BOUND | PS_HTML_FACTS_GZIP_COPY_BOUND),
              0u);
    ExpectNotSent(facts, PS_MARKDOWN_ENCODING_BR);
  }
  Coded typed;
  typed.content_type = "text/html";
  StoreCoded(PS_MARKDOWN_ENCODING_BR, typed);
  ExpectNotSent(Facts(PS_MARKDOWN_ENCODING_BR), PS_MARKDOWN_ENCODING_BR);
}

TEST_F(AbiMarkdownCodingTest, ANotSmallerRecordIsReportedAndNeverSent) {
  StorePage();
  StorePlain();
  for (int coding : kCodings) {
    SCOPED_TRACE(coding);
    Coded record;
    record.bytes.clear();
    StoreCoded(coding, record);
    const ps_html_stored_facts_t facts = Facts(coding);
    EXPECT_NE(facts.flags & NotSmallerBit(coding), 0u);
    EXPECT_EQ(facts.flags & BoundBit(coding), 0u);
    ExpectNotSent(facts, coding);
    // A record for another plain copy is not reported.
    record.source = Other(5);
    StoreCoded(coding, record);
    EXPECT_EQ(Facts(coding).flags & NotSmallerBit(coding), 0u);
  }
}

TEST_F(AbiMarkdownCodingTest, NoAcceptableCodingFallsThrough) {
  StorePage();
  StorePlain();
  StoreCoded(PS_MARKDOWN_ENCODING_BR);
  const ps_html_stored_facts_t facts = Facts(PS_MARKDOWN_ENCODING_BR);
  for (int coding : {PS_MARKDOWN_ENCODING_IDENTITY, PS_MARKDOWN_ENCODING_BR}) {
    const ps_html_serve_verdict_t v =
        Classify(Request(coding, PS_HTML_SERVE_REQUEST_ENCODING_NONE |
                                     PS_HTML_SERVE_REQUEST_IDENTITY_OK),
                 facts);
    EXPECT_EQ(v.action, PS_HTML_SERVE_ACTION_FALL_THROUGH);
    EXPECT_STREQ(ps_html_serve_reason_name(v.reason), "no_acceptable_encoding");
  }
  // A reason that decides earlier keeps its name.
  ps_html_stored_facts_t empty{};
  empty.struct_size = sizeof(empty);
  const ps_html_serve_verdict_t v =
      Classify(Request(0, PS_HTML_SERVE_REQUEST_ENCODING_NONE), empty);
  EXPECT_STREQ(ps_html_serve_reason_name(v.reason), "absent");
}

TEST_F(AbiMarkdownCodingTest, FactsRefuseACodedReadWithoutACoding) {
  StorePage();
  StorePlain();
  StoreCoded(PS_MARKDOWN_ENCODING_GZIP);
  ps_read_result_t* coded = Read(PS_SENTINEL_MARKDOWN_GZIP);
  ASSERT_NE(coded, nullptr);
  ps_html_stored_facts_t facts{};
  facts.struct_size = sizeof(facts);
  for (int coding : {PS_MARKDOWN_ENCODING_IDENTITY, 3, -1}) {
    EXPECT_EQ(ps_html_stored_facts_from_reads_coded(
                  nullptr, nullptr, nullptr, 0, nullptr, coded, coding, &facts),
              -1)
        << coding;
  }
  ps_read_free(coded);
  // No coded read: the facts of the _ex form.
  EXPECT_EQ(ps_html_stored_facts_from_reads_coded(nullptr, nullptr, nullptr, 0,
                                                  nullptr, nullptr, 0, &facts),
            0);
}

// ------------------------------------------------------------ ETag rows

TEST(AbiMarkdownCodingEtag, OneTagPerCodingSameShape) {
  const Hash b = Sha(kPage);
  const Hash m = Sha(kPlain);
  const auto* bp = reinterpret_cast<const uint8_t*>(b.data());
  const auto* mp = reinterpret_cast<const uint8_t*>(m.data());
  char plain[PS_HTML_MARKDOWN_ETAG_CHARS + 1];
  size_t len = 0;
  ASSERT_EQ(ps_html_markdown_etag(bp, mp, plain, sizeof(plain), &len), 0);
  std::string tags[3];
  for (int coding : {0, 1, 2}) {
    char buf[PS_HTML_MARKDOWN_ETAG_CHARS + 1];
    ASSERT_EQ(
        ps_html_markdown_etag_coded(bp, mp, coding, buf, sizeof(buf), &len), 0);
    EXPECT_EQ(len, size_t{PS_HTML_MARKDOWN_ETAG_CHARS});
    tags[coding] = buf;
    EXPECT_EQ(tags[coding].size(), 24u);
    EXPECT_EQ(tags[coding].substr(0, 7), "W/\"psm-");
    EXPECT_EQ(tags[coding].back(), '"');
  }
  EXPECT_EQ(tags[0], plain) << "the identity tag is the 1.14 tag";
  EXPECT_NE(tags[1], tags[0]);
  EXPECT_NE(tags[2], tags[0]);
  EXPECT_NE(tags[1], tags[2]);
  char buf[PS_HTML_MARKDOWN_ETAG_CHARS + 1];
  EXPECT_EQ(ps_html_markdown_etag_coded(bp, mp, 3, buf, sizeof(buf), &len), -1);
  EXPECT_EQ(ps_html_markdown_etag_coded(bp, mp, -1, buf, sizeof(buf), &len),
            -1);
  char tiny[4];
  EXPECT_EQ(ps_html_markdown_etag_coded(bp, mp, 2, tiny, sizeof(tiny), &len),
            -3);
}

TEST_F(AbiMarkdownCodingTest, IfNoneMatchConfirmsOnlyTheCodingSent) {
  StorePage();
  StorePlain();
  StoreCoded(PS_MARKDOWN_ENCODING_BR);
  const ps_html_stored_facts_t facts = Facts(PS_MARKDOWN_ENCODING_BR);
  const std::string identity_tag = Tag(PS_MARKDOWN_ENCODING_IDENTITY);
  const std::string br_tag = Tag(PS_MARKDOWN_ENCODING_BR);

  ps_html_serve_request_t r = Request(PS_MARKDOWN_ENCODING_BR, 0);
  r.if_none_match = identity_tag.c_str();
  ps_html_serve_verdict_t v = Classify(r, facts);
  EXPECT_EQ(v.action, PS_HTML_SERVE_ACTION_SERVE_MARKDOWN);
  EXPECT_EQ(v.not_modified, 0) << "the plain copy's tag on a br request: 200";

  r.if_none_match = br_tag.c_str();
  v = Classify(r, facts);
  EXPECT_EQ(v.not_modified, 1) << "the br tag on a br request: 304";
  EXPECT_EQ(std::string(v.etag), br_tag);

  // And the plain copy is confirmed only by its own tag.
  ps_html_serve_request_t plain = Request(PS_MARKDOWN_ENCODING_IDENTITY, 0);
  plain.if_none_match = br_tag.c_str();
  EXPECT_EQ(Classify(plain, facts).not_modified, 0);
  plain.if_none_match = identity_tag.c_str();
  EXPECT_EQ(Classify(plain, facts).not_modified, 1);
}

// ------------------------------------------------------------ ABI rows

TEST_F(AbiMarkdownCodingTest, AReservedZeroReadsAsIdentity) {
  StorePage();
  StorePlain();
  StoreCoded(PS_MARKDOWN_ENCODING_BR);
  const ps_html_stored_facts_t facts = Facts(PS_MARKDOWN_ENCODING_BR);
  // A 1.15 caller: the same struct size, the reserved word zero.
  ps_html_serve_request_t r = Request(0, 0);
  r.reserved = 0;
  ps_html_serve_verdict_t v = Classify(r, facts);
  EXPECT_EQ(v.action, PS_HTML_SERVE_ACTION_SERVE_MARKDOWN);
  EXPECT_EQ(v.markdown_encoding, PS_MARKDOWN_ENCODING_IDENTITY);
  EXPECT_EQ(std::string(v.etag), Tag(PS_MARKDOWN_ENCODING_IDENTITY));
  // Both names are the one word, and values above 2 read as identity.
  r.reserved = PS_MARKDOWN_ENCODING_BR;
  EXPECT_EQ(r.markdown_encoding, uint32_t{PS_MARKDOWN_ENCODING_BR});
  EXPECT_EQ(Classify(r, facts).markdown_encoding, PS_MARKDOWN_ENCODING_BR);
  for (uint32_t enc : {3u, 7u, 0xFFFFFFFFu}) {
    r.markdown_encoding = enc;
    v = Classify(r, facts);
    EXPECT_EQ(v.action, PS_HTML_SERVE_ACTION_SERVE_MARKDOWN) << enc;
    EXPECT_EQ(v.markdown_encoding, PS_MARKDOWN_ENCODING_IDENTITY) << enc;
  }
}

TEST_F(AbiMarkdownCodingTest, A115VerdictGetsNoWritePastItsSize) {
  StorePage();
  StorePlain();
  StoreCoded(PS_MARKDOWN_ENCODING_BR);
  const ps_html_stored_facts_t facts = Facts(PS_MARKDOWN_ENCODING_BR);
  constexpr size_t k115 = offsetof(ps_html_serve_verdict_t, etag) +
                          sizeof(ps_html_serve_verdict_t::etag);
  static_assert(k115 == 64, "the 1.15 verdict was 64 bytes");
  unsigned char buf[sizeof(ps_html_serve_verdict_t)];
  std::memset(buf, 0x5A, sizeof(buf));
  auto* v = reinterpret_cast<ps_html_serve_verdict_t*>(buf);
  v->struct_size = k115;
  const ps_html_serve_request_t r = Request(PS_MARKDOWN_ENCODING_BR, 0);
  ASSERT_EQ(ps_html_serve_classify(&r, &facts, v), 0);
  EXPECT_EQ(v->action, PS_HTML_SERVE_ACTION_SERVE_MARKDOWN);
  for (size_t i = k115; i < sizeof(buf); ++i) EXPECT_EQ(buf[i], 0x5A) << i;
  // A caller that cannot learn the coding is answered in identity: the tag
  // it holds is the plain copy's.
  EXPECT_EQ(std::string(v->etag), Tag(PS_MARKDOWN_ENCODING_IDENTITY));
}

TEST_F(AbiMarkdownCodingTest, TheGenericSentinelWriteRefusesBothIds) {
  for (uint8_t id :
       {uint8_t{PS_SENTINEL_MARKDOWN_BR}, uint8_t{PS_SENTINEL_MARKDOWN_GZIP}}) {
    ps_write_handle_t* wh = nullptr;
    EXPECT_EQ(ps_cache_write_sentinel(cache_, kUrl, kHost, kScheme, id, 4, &wh),
              PS_ERR_INVALID_ARG)
        << static_cast<int>(id);
    EXPECT_EQ(wh, nullptr);
  }
  // A free sentinel-shaped id stays writable.
  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_sentinel(cache_, kUrl, kHost, kScheme, 0xFC, 4, &wh),
            PS_OK);
  ps_write_abort(wh);
}

TEST_F(AbiMarkdownCodingTest, TheCodedIdsAreNeverSelected) {
  StorePage();
  StorePlain();
  for (int coding : kCodings) StoreCoded(coding);
  ps_read_result_t* r = nullptr;
  ASSERT_EQ(ps_cache_read_best_site(cache_, kUrl, kHost, kScheme, kSite.c_str(),
                                    0x08, &r),
            PS_ERR_NOT_FOUND);
}

TEST_F(AbiMarkdownCodingTest, TheCodedWriterTakesACodingNotAnId) {
  pagespeed::AlternateMetadata meta;
  meta.full_mask = PS_SENTINEL_MARKDOWN_BR;
  for (uint8_t coding : {uint8_t{0}, uint8_t{3}, uint8_t{0xDC}}) {
    EXPECT_FALSE(writer_
                     ->WriteAgentCodedAlternate(kUrl, kHost, kScheme, coding, 1,
                                                meta, kSite)
                     .has_value())
        << static_cast<int>(coding);
  }
  // The metadata must self-describe as the coding's id.
  EXPECT_FALSE(writer_
                   ->WriteAgentCodedAlternate(kUrl, kHost, kScheme,
                                              PS_MARKDOWN_ENCODING_GZIP, 1,
                                              meta, kSite)
                   .has_value());
}

// ------------------------------------------------------------ notify flag

TEST(AbiMarkdownCodingNotify, TheCompressFlagNeedsTheStoredPageShape) {
  const std::string site(PS_SITE_ID_CHARS, 'a');
  ps_notify_params_t p;
  ps_notify_params_init_auto(&p);
  p.url = "/page.html";
  p.hostname = "example.com";
  p.scheme = "https";
  p.content_type = PS_CONTENT_HTML;
  p.mask = 0x08;
  p.site = site.c_str();
  // Without the HTML-stored flag: refused before any connection.
  p.notify_flags = PS_NOTIFY_FLAG_FROM_MODULE | PS_NOTIFY_FLAG_HOST_VOUCHED |
                   PS_NOTIFY_FLAG_MARKDOWN_COMPRESS;
  EXPECT_EQ(ps_notify_worker_ex("/nonexistent/socket", &p), PS_ERR_INVALID_ARG);
  // In its shape it is accepted and only the delivery fails.
  p.notify_flags |= PS_NOTIFY_FLAG_HTML_STORED;
  EXPECT_NE(ps_notify_worker_ex("/nonexistent/socket", &p), PS_ERR_INVALID_ARG);
}

// ------------------------------------------------------------ advertisement

TEST(AbiMarkdownCodingConfig, TheWorkerPublishesItsCodings) {
  EXPECT_EQ(ps_read_shared_config_markdown_codings(nullptr), -1);
  const std::string dir = pagespeed::test::MakeTempDir();
  const std::string cache_path = dir + "/cache.vol";
  const std::string conf = pagespeed::SharedConfigFilePath(cache_path);
  EXPECT_EQ(ps_read_shared_config_markdown_codings(cache_path.c_str()), -1)
      << "no config: a worker that publishes none";
  pagespeed::SharedConfig sc;
  ASSERT_TRUE(pagespeed::WriteSharedConfigFile(conf, sc));
  EXPECT_EQ(ps_read_shared_config_markdown_codings(cache_path.c_str()), -1)
      << "a config without the key: a worker that predates it";
  struct Case {
    int gzip_level;
    int brotli_level;
    int want;
  };
  for (const Case& c :
       {Case{6, 6, 6}, Case{0, 6, 4}, Case{6, 0, 2}, Case{0, 0, 0}}) {
    sc.markdown_codings =
        pagespeed::MarkdownCodingsForLevels(c.gzip_level, c.brotli_level);
    ASSERT_TRUE(pagespeed::WriteSharedConfigFile(conf, sc));
    EXPECT_EQ(ps_read_shared_config_markdown_codings(cache_path.c_str()),
              c.want)
        << c.gzip_level << "/" << c.brotli_level;
  }
  // A worker whose mode derives no Markdown copies publishes none, so no
  // front end sends it a compress demand it would refuse.
  sc.markdown_codings = pagespeed::PublishedMarkdownCodings(false, 6, 6);
  ASSERT_TRUE(pagespeed::WriteSharedConfigFile(conf, sc));
  EXPECT_EQ(ps_read_shared_config_markdown_codings(cache_path.c_str()), -1);
  EXPECT_EQ(pagespeed::PublishedMarkdownCodings(true, 6, 0), 2);
  EXPECT_EQ(1 << PS_MARKDOWN_ENCODING_GZIP, 2);
  EXPECT_EQ(1 << PS_MARKDOWN_ENCODING_BR, 4);
  std::filesystem::remove_all(dir);
}

TEST(AbiMarkdownCodingConfig, AMalformedValueIsNotPublished) {
  for (const char* value : {"br,", "", ",gzip", "br,,gzip"}) {
    const pagespeed::SharedConfig sc = pagespeed::ParseSharedConfig(
        std::string("version=1\nmarkdown_codings=") + value + "\n");
    EXPECT_EQ(sc.markdown_codings, -1) << value;
  }
  EXPECT_EQ(pagespeed::ParseSharedConfig("markdown_codings=gzip,br\n")
                .markdown_codings,
            6);
  EXPECT_EQ(
      pagespeed::ParseSharedConfig("markdown_codings=none\n").markdown_codings,
      0);
  // A coding a later worker adds is skipped; the known ones are kept.
  EXPECT_EQ(pagespeed::ParseSharedConfig("markdown_codings=br,zstd\n")
                .markdown_codings,
            4);
  EXPECT_EQ(pagespeed::ParseSharedConfig("markdown_codings=zstd,gzip,br\n")
                .markdown_codings,
            6);
  EXPECT_EQ(
      pagespeed::ParseSharedConfig("markdown_codings=zstd\n").markdown_codings,
      0);
}

}  // namespace
