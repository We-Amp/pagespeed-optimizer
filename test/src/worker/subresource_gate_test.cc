// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/subresource_gate.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

// A server whose answers the test sets per absolute URL; a URL it has no
// answer for gets no response at all.
class FakeServer {
 public:
  SubresourceGate::HeadFn Head() {
    return [this](const std::string& origin,
                  const std::string& url) -> std::optional<int> {
      origins.push_back(origin);
      asked.push_back(url);
      auto it = status.find(url);
      if (it == status.end()) return std::nullopt;
      return it->second;
    };
  }
  std::map<std::string, int> status;
  std::vector<std::string> origins;
  std::vector<std::string> asked;
};

class SubresourceGateTest : public ::testing::Test {
 protected:
  SubresourceGateTest()
      : gate_(server_.Head(), [this] { return now_ms_; }, Options()) {}

  static SubresourceGate::Options Options() {
    SubresourceGate::Options o;
    o.admitted_ttl_ms = 300 * 1000;
    o.refused_ttl_ms = 60 * 1000;
    return o;
  }

  // The read holds views, so the URL must outlive it: a string_view of a
  // literal does, a std::string temporary bound to a const reference does not.
  static SubresourceRead Read(std::string_view url) {
    SubresourceRead r;
    r.site = "";
    r.scheme = "https";
    r.host = "example.com";
    r.url = url;
    r.page_host = "example.com";
    r.host_fetch_allowed = true;
    return r;
  }

  FakeServer server_;
  std::int64_t now_ms_ = 1000;
  SubresourceGate gate_;
};

TEST_F(SubresourceGateTest, ServedToAnonymousClientIsAdmitted) {
  server_.status["https://example.com/open/a.css"] = 200;
  EXPECT_EQ(gate_.Check(Read("/open/a.css"), true),
            SubresourceVerdict::kAdmitted);
  ASSERT_EQ(server_.asked.size(), 1u);
  EXPECT_EQ(server_.origins[0], "https://example.com");
  EXPECT_EQ(server_.asked[0], "https://example.com/open/a.css");
  EXPECT_EQ(gate_.heads_sent(), 1u);
  EXPECT_EQ(gate_.refusals(), 0u);
}

TEST_F(SubresourceGateTest, RefusedToAnonymousClientIsRefused) {
  for (int code : {401, 403, 404, 500, 204, 302}) {
    SubresourceGate gate(server_.Head(), [this] { return now_ms_; });
    const std::string url = "/held/" + std::to_string(code) + ".css";
    server_.status["https://example.com" + url] = code;
    EXPECT_EQ(gate.Check(Read(url), true), SubresourceVerdict::kRefused)
        << code;
    EXPECT_FALSE(gate.Admitted(Read(url), true)) << code;
  }
}

TEST_F(SubresourceGateTest, NoAnswerIsUnknownAndNotAdmitted) {
  EXPECT_EQ(gate_.Check(Read("/silent.css"), true),
            SubresourceVerdict::kUnknown);
  EXPECT_FALSE(gate_.Admitted(Read("/silent.css"), true));
  // Remembered like a refusal: no second request inside the period.
  EXPECT_EQ(server_.asked.size(), 1u);
  EXPECT_EQ(gate_.refusals(), 1u);
}

TEST_F(SubresourceGateTest, AnswersAreRememberedPerUrlUntilTheyExpire) {
  server_.status["https://example.com/a.css"] = 200;
  server_.status["https://example.com/b.css"] = 403;
  ASSERT_TRUE(gate_.Admitted(Read("/a.css"), true));
  ASSERT_FALSE(gate_.Admitted(Read("/b.css"), true));
  ASSERT_EQ(server_.asked.size(), 2u);

  // Within both periods: no new request, same answers, even when the server
  // has changed its mind.
  server_.status["https://example.com/a.css"] = 403;
  server_.status["https://example.com/b.css"] = 200;
  now_ms_ += 59 * 1000;
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_FALSE(gate_.Admitted(Read("/b.css"), true));
  EXPECT_EQ(server_.asked.size(), 2u);

  // The refusal expires first and is asked again.
  now_ms_ += 2 * 1000;
  EXPECT_TRUE(gate_.Admitted(Read("/b.css"), true));
  EXPECT_EQ(server_.asked.size(), 3u);
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_EQ(server_.asked.size(), 3u);

  // Then the admission.
  now_ms_ += 300 * 1000;
  EXPECT_FALSE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_EQ(server_.asked.size(), 4u);
}

TEST_F(SubresourceGateTest, MemoIsKeyedBySiteSchemeHostAndUrl) {
  server_.status["https://example.com/a.css"] = 200;
  ASSERT_TRUE(gate_.Admitted(Read("/a.css"), true));
  SubresourceRead other_site = Read("/a.css");
  other_site.site = "0123456789abcdef";
  EXPECT_FALSE(gate_.Admitted(other_site, /*may_fetch=*/false));
  SubresourceRead other_scheme = Read("/a.css");
  other_scheme.scheme = "http";
  EXPECT_FALSE(gate_.Admitted(other_scheme, /*may_fetch=*/false));
  SubresourceRead other_query = Read("/a.css?v=2");
  EXPECT_FALSE(gate_.Admitted(other_query, /*may_fetch=*/false));
  // Host case and a default port name the same host.
  SubresourceRead same_host = Read("/a.css");
  same_host.host = "Example.COM:443";
  EXPECT_TRUE(gate_.Admitted(same_host, /*may_fetch=*/false));
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, MemoOnlyCallerNeverAsks) {
  server_.status["https://example.com/a.css"] = 200;
  EXPECT_EQ(gate_.Check(Read("/a.css"), /*may_fetch=*/false),
            SubresourceVerdict::kUnknown);
  EXPECT_TRUE(server_.asked.empty());
  ASSERT_TRUE(gate_.Admitted(Read("/a.css"), /*may_fetch=*/true));
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), /*may_fetch=*/false));
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, AsksOnlyThePageHostForAPermittedSender) {
  server_.status["https://example.com/a.css"] = 200;
  server_.status["https://cdn.example.net/a.css"] = 200;

  SubresourceRead not_permitted = Read("/a.css");
  not_permitted.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(not_permitted, true), SubresourceVerdict::kUnknown);

  SubresourceRead other_host = Read("/a.css");
  other_host.host = "cdn.example.net";
  EXPECT_EQ(gate_.Check(other_host, true), SubresourceVerdict::kUnknown);

  SubresourceRead no_page_host = Read("/a.css");
  no_page_host.page_host = "";
  EXPECT_EQ(gate_.Check(no_page_host, true), SubresourceVerdict::kUnknown);

  SubresourceRead no_host = Read("/a.css");
  no_host.host = "";
  no_host.page_host = "";
  EXPECT_EQ(gate_.Check(no_host, true), SubresourceVerdict::kUnknown);

  EXPECT_TRUE(server_.asked.empty());
  // Not remembered: a permitted sender asks afterwards.
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, OnlyPathKeysAreAsked) {
  for (const char* url : {"", "a.css", "//evil.example/a.css",
                          "https://example.com/a.css", "javascript:x"}) {
    EXPECT_EQ(gate_.Check(Read(url), true), SubresourceVerdict::kUnknown)
        << url;
  }
  SubresourceRead bad_scheme = Read("/a.css");
  bad_scheme.scheme = "ftp";
  EXPECT_EQ(gate_.Check(bad_scheme, true), SubresourceVerdict::kUnknown);
  EXPECT_TRUE(server_.asked.empty());
}

TEST_F(SubresourceGateTest, EmptySchemeIsHttpsAndPortIsKept) {
  server_.status["https://example.com:8443/a.css"] = 200;
  SubresourceRead r = Read("/a.css");
  r.scheme = "";
  r.host = "example.com:8443";
  r.page_host = "EXAMPLE.com:8443";
  EXPECT_TRUE(gate_.Admitted(r, true));
  ASSERT_EQ(server_.origins.size(), 1u);
  EXPECT_EQ(server_.origins[0], "https://example.com:8443");
}

TEST_F(SubresourceGateTest, FullMemoDropsExpiredThenEverything) {
  SubresourceGate::Options o;
  o.max_entries = 2;
  o.refused_ttl_ms = 10;
  SubresourceGate gate(server_.Head(), [this] { return now_ms_; }, o);
  server_.status["https://example.com/a.css"] = 200;
  server_.status["https://example.com/b.css"] = 200;
  server_.status["https://example.com/c.css"] = 200;
  ASSERT_TRUE(gate.Admitted(Read("/a.css"), true));
  ASSERT_TRUE(gate.Admitted(Read("/b.css"), true));
  ASSERT_TRUE(gate.Admitted(Read("/c.css"), true));  // full: cleared
  EXPECT_FALSE(gate.Admitted(Read("/a.css"), false));
  EXPECT_TRUE(gate.Admitted(Read("/c.css"), false));
}

TEST_F(SubresourceGateTest, TestOverrideDecidesForEveryGate) {
  SubresourceGate::SetVerdictOverrideForTest(
      [](const SubresourceRead& r) -> std::optional<SubresourceVerdict> {
        if (r.url == "/forced.css") return SubresourceVerdict::kAdmitted;
        return std::nullopt;
      });
  SubresourceRead r = Read("/forced.css");
  r.host_fetch_allowed = false;
  EXPECT_TRUE(gate_.Admitted(r, false));
  EXPECT_EQ(gate_.Check(Read("/other.css"), false),
            SubresourceVerdict::kUnknown);
  SubresourceGate::SetVerdictOverrideForTest(nullptr);
  EXPECT_FALSE(gate_.Admitted(r, false));
  EXPECT_TRUE(server_.asked.empty());
}

// An entry the front end recorded from an anonymous request, read from the
// page's own site keyspace.
SubresourceRead StampedRead(SubresourceRead r, std::uint8_t extra_flags = 0) {
  r.entry_read = true;
  r.entry_flags = AlternateMetadata::kFlagAnonymousRecorded | extra_flags;
  r.entry_site = r.site;
  return r;
}

TEST_F(SubresourceGateTest, StampedEntryUnderUnconfirmedHostIsAdmitted) {
  server_.status["https://example.com/a.css"] = 403;  // never asked
  SubresourceRead r =
      StampedRead(Read("/a.css"), AlternateMetadata::kFlagHeadersClassified);
  r.site = "0123456789abcdef";
  r.entry_site = r.site;
  r.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/true), SubresourceVerdict::kAdmitted);
  // A memo-only caller gets the same answer: the entry is the evidence.
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/false), SubresourceVerdict::kAdmitted);
  EXPECT_TRUE(server_.asked.empty());
  EXPECT_EQ(gate_.heads_sent(), 0u);
  EXPECT_EQ(gate_.refusals(), 0u);
  // Nothing is remembered for the URL: the same URL without the entry is
  // still unknown.
  SubresourceRead bare = Read("/a.css");
  bare.site = r.site;
  bare.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(bare, true), SubresourceVerdict::kUnknown);
}

TEST_F(SubresourceGateTest, UnstampedEntryUnderUnconfirmedHostIsAbsent) {
  server_.status["https://example.com/a.css"] = 200;
  SubresourceRead r = Read("/a.css");
  r.host_fetch_allowed = false;
  r.entry_read = true;
  r.entry_flags = AlternateMetadata::kFlagHeadersClassified |
                  AlternateMetadata::kFlagWorkerProcessed;
  r.entry_site = r.site;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kUnknown);
  EXPECT_FALSE(gate_.Admitted(r, true));
  EXPECT_TRUE(server_.asked.empty());
}

TEST_F(SubresourceGateTest, StampedEntryFromAnotherSiteKeyspaceIsNotAdmitted) {
  SubresourceRead r = StampedRead(Read("/a.css"));
  r.site = "0123456789abcdef";
  r.entry_site = "fedcba9876543210";
  r.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kUnknown);
  EXPECT_TRUE(server_.asked.empty());
  // The default keyspace and a named one are different keyspaces too.
  SubresourceRead to_default = StampedRead(Read("/a.css"));
  to_default.entry_site = "0123456789abcdef";
  to_default.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(to_default, true), SubresourceVerdict::kUnknown);
  // Under a confirmed host such an entry is asked about like any other.
  server_.status["https://example.com/a.css"] = 403;
  r.host_fetch_allowed = true;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kRefused);
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, StampedEntryUnderConfirmedHostIsAskedAsBefore) {
  // The host's answer decides, stamp or not.
  server_.status["https://example.com/a.css"] = 403;
  server_.status["https://example.com/b.css"] = 200;
  SubresourceRead r = StampedRead(Read("/a.css"));
  ASSERT_TRUE(r.host_fetch_allowed);
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/true), SubresourceVerdict::kRefused);
  SubresourceRead b = StampedRead(Read("/b.css"));
  EXPECT_EQ(gate_.Check(b, /*may_fetch=*/true), SubresourceVerdict::kAdmitted);
  ASSERT_EQ(server_.asked.size(), 2u);
  EXPECT_EQ(server_.asked[0], "https://example.com/a.css");
  EXPECT_EQ(server_.asked[1], "https://example.com/b.css");
  EXPECT_EQ(gate_.heads_sent(), 2u);
  // A memo-only caller gets the remembered answer, not the stamp.
  SubresourceRead c = StampedRead(Read("/c.css"));
  EXPECT_EQ(gate_.Check(c, /*may_fetch=*/false), SubresourceVerdict::kUnknown);
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/false), SubresourceVerdict::kRefused);
  EXPECT_EQ(server_.asked.size(), 2u);
}

}  // namespace
}  // namespace pagespeed
