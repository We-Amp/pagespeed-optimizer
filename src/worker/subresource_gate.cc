// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/subresource_gate.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "lib/classify/alternate_metadata.h"
#include "lib/classify/hostname.h"
#include "lib/net/fetch_policy.h"
#include "lib/net/upstream_pin.h"
#include "src/browser/agent_fetcher.h"

namespace pagespeed {

namespace {

std::mutex& OverrideMutex() {
  static std::mutex m;
  return m;
}

SubresourceGate::VerdictOverride& OverrideHook() {
  static SubresourceGate::VerdictOverride hook;
  return hook;
}

std::optional<SubresourceVerdict> RunOverride(const SubresourceRead& read) {
  SubresourceGate::VerdictOverride hook;
  {
    std::lock_guard<std::mutex> lock(OverrideMutex());
    hook = OverrideHook();
  }
  if (!hook) return std::nullopt;
  return hook(read);
}

std::string MemoKey(std::string_view site, std::string_view scheme,
                    std::string_view host, std::string_view url) {
  std::string key;
  key.reserve(site.size() + scheme.size() + host.size() + url.size() + 3);
  key.append(site);
  key.push_back('\n');
  key.append(scheme);
  key.push_back('\n');
  key.append(host);
  key.push_back('\n');
  key.append(url);
  return key;
}

}  // namespace

SubresourceGate::SubresourceGate(HeadFn head, NowFn now, Options options)
    : head_(std::move(head)), now_(std::move(now)), options_(options) {}

void SubresourceGate::SetVerdictOverrideForTest(VerdictOverride hook) {
  std::lock_guard<std::mutex> lock(OverrideMutex());
  OverrideHook() = std::move(hook);
}

SubresourceVerdict SubresourceGate::Check(const SubresourceRead& read,
                                          bool may_fetch) {
  if (auto forced = RunOverride(read); forced.has_value()) return *forced;

  // A stylesheet or image the front end recorded from its response to an
  // anonymous request is what the site serves to any client.  It stands in
  // for the request only where the worker may not ask the page's host; a
  // host it may ask is asked, stamped or not.  Only an entry read from the
  // page's own site keyspace counts.
  if (!read.host_fetch_allowed && read.entry_read &&
      (read.entry_flags & AlternateMetadata::kFlagAnonymousRecorded) != 0 &&
      read.entry_site == read.site) {
    return SubresourceVerdict::kAdmitted;
  }

  // The cache keys every recorded resource by its path; any other form names
  // nothing a front end recorded, and nothing to ask about.
  if (read.url.empty() || read.url.front() != '/' ||
      (read.url.size() > 1 && read.url[1] == '/')) {
    return SubresourceVerdict::kUnknown;
  }
  const std::string host = NormalizeHostname(read.host);
  if (host.empty()) return SubresourceVerdict::kUnknown;
  const std::string_view scheme = read.scheme.empty() ? "https" : read.scheme;
  if (scheme != "https" && scheme != "http") {
    return SubresourceVerdict::kUnknown;
  }

  const std::string key = MemoKey(read.site, scheme, host, read.url);
  const std::int64_t now = now_ ? now_() : 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = memo_.find(key);
    if (it != memo_.end()) {
      if (now < it->second.expires_at_ms) return it->second.verdict;
      memo_.erase(it);
    }
  }

  // Ask only the page's own host, and only for a sender that may ask for
  // fetches from it.
  if (!may_fetch || !read.host_fetch_allowed || !head_ ||
      host != NormalizeHostname(read.page_host)) {
    return SubresourceVerdict::kUnknown;
  }

  const std::string origin = std::string(scheme) + "://" + host;
  const std::string absolute_url = origin + std::string(read.url);
  const std::optional<int> status = head_(origin, absolute_url);

  SubresourceVerdict verdict = SubresourceVerdict::kUnknown;
  if (status.has_value()) {
    verdict = (*status == 200) ? SubresourceVerdict::kAdmitted
                               : SubresourceVerdict::kRefused;
  }
  const std::int64_t ttl = verdict == SubresourceVerdict::kAdmitted
                               ? options_.admitted_ttl_ms
                               : options_.refused_ttl_ms;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++heads_sent_;
    if (verdict != SubresourceVerdict::kAdmitted) ++refusals_;
    if (memo_.size() >= options_.max_entries) {
      std::erase_if(memo_, [now](const auto& entry) {
        return now >= entry.second.expires_at_ms;
      });
      if (memo_.size() >= options_.max_entries) memo_.clear();
    }
    memo_[key] = Entry{verdict, now + ttl};
  }
  return verdict;
}

std::uint64_t SubresourceGate::heads_sent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return heads_sent_;
}

std::uint64_t SubresourceGate::refusals() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return refusals_;
}

SubresourceGate::HeadFn MakeAnonymousHeadFn() {
  return [](const std::string& origin,
            const std::string& absolute_url) -> std::optional<int> {
    std::optional<PinnedUpstream> upstream = ParseUpstream(origin);
    if (!upstream.has_value()) return std::nullopt;
    FetchPolicy policy;
    policy.pinned_upstreams.push_back(*upstream);
    AgentFetcherDeps deps;
    deps.spawn = RealCurlSpawn();
    deps.resolve = RealHostResolver();
    // A redirect is not the resource: it is not followed, and reads as no
    // answer.
    deps.max_redirect_hops = 0;
    deps.curl_opts.head = true;
    deps.curl_opts.max_time_sec = 3;
    deps.curl_opts.max_response_bytes = 64u << 10;
    AgentFetchOutcome outcome =
        FetchSubresource(absolute_url, ResourceClass::kDocument, policy, deps);
    if (!outcome.fulfilled) return std::nullopt;
    return outcome.status;
  };
}

SubresourceGate::NowFn MonotonicNowMs() {
  return []() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
}

}  // namespace pagespeed
