// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/cache.h"

#include "lib/base/atomic_file_writer.h"
#include "lib/base/posix_compat.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
// chmod is a no-op on Windows (ACL-based permissions model).
inline int chmod(const char*, int) { return 0; }
#endif

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/classify/option_context.h"
// The storage layer's on-disk format major (VolumeHeader::kFormatVersionMajor)
// is what the volume filename's fingerprint encodes, and the sweep below has
// to stop at that boundary — so it is read from the storage layer rather than
// restated here, where a format bump would leave it behind.
#include "volume.hpp"

namespace pagespeed {

namespace {

// Trivial selector that matches a specific AlternateId.
class ExactIdSelector : public cyclone::StorageAlternateSelector {
 public:
  explicit ExactIdSelector(cyclone::AlternateId target) : target_(target) {}

  [[nodiscard]] std::optional<size_t> select(
      std::span<const cyclone::AlternateInfo> alternates,
      const cyclone::AlternateSelectionContext& /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == target_) {
        return i;
      }
    }
    return std::nullopt;
  }

 private:
  cyclone::AlternateId target_;
};

// Parse AlternateMetadata from a content prefix and return the prefix
// size.  Returns 0 if the content is too short or metadata is corrupt.
// Uses wire_ct_len (not sanitized origin_content_type.size()) for
// correct content offset computation.
size_t ParseMetadataPrefix(std::span<const std::byte> content,
                           AlternateMetadata* out) {
  if (content.size() < AlternateMetadata::kFixedPrefixSize) {
    return 0;
  }
  auto parsed = AlternateMetadata::Deserialize(content);
  if (!parsed) {
    return 0;
  }
  size_t consumed = parsed->consumed_size;
  // Regression canary: consumed_size and WireSize() are derived from the
  // same values so they always agree for correct code.  A mismatch here
  // means a future change broke a constant or forgot a field — reject
  // the entry early rather than silently miscomputing the content offset.
  if (consumed != parsed->WireSize() || consumed > content.size()) {
    return 0;
  }
  *out = *parsed;
  return consumed;
}

}  // namespace

bool UnlinkLeftEntryLinked(cyclone::CacheError error) {
  switch (error) {
    case cyclone::CacheError::NotFound:
    case cyclone::CacheError::AlternateNotFound:
      // Nothing was there to unlink.
      return false;
    default:
      // Everything else failed with the node in place, or failed in a way
      // this build cannot prove removed it.  Both are the same thing to the
      // caller, and the default arm is deliberate: a storage version that
      // adds an error code should have it read as "still linked" until
      // somebody decides otherwise, not silently join the benign set.
      return true;
  }
}

PageSpeedCache::~PageSpeedCache() {
  if (cache_) {
    cache_->stop();
  }
}

std::expected<std::unique_ptr<PageSpeedCache>, cyclone::CacheError>
PageSpeedCache::Create(const PageSpeedCacheConfig& config) {
  auto ps_cache = std::unique_ptr<PageSpeedCache>(new PageSpeedCache());
  ps_cache->config_ = config;

  // Store the Cyclone config as a member so it survives Create().
  // Cyclone::Cache may hold a reference to the config.
  auto& cc = ps_cache->cyclone_config_;
  cc.enable_checksum = config.enable_checksum;
  cc.verify_checksum_on_read = config.verify_checksum_on_read;
  cc.multi_process_config = config.multi_process;

  // Cyclone's RAM cache is keyed by (CacheKey, AlternateId) so
  // multiple alternates of the same URL coexist in RAM.
  cc.ram_cache_size = config.ram_cache_size;

  // Lease-based region pinning.  add_volume
  // copies these onto the volume config.
  cc.read_lease_duration = config.read_lease_duration;
  cc.lease_wrap_ceiling = config.lease_wrap_ceiling;

  auto cache_result = cyclone::Cache::create(cc);
  if (!cache_result) {
    return std::unexpected(cache_result.error());
  }

  auto& cache = *cache_result;
  auto vol_result = cache->add_volume(config.volume_path, config.volume_size);
  if (!vol_result) {
    return std::unexpected(vol_result.error());
  }

  auto start_result = cache->start();
  if (!start_result) {
    return std::unexpected(start_result.error());
  }

  ps_cache->cache_ = std::move(cache);
  ps_cache->handler_ = config.handler;
  if (!ps_cache->CaptureVolumePathsAndChmod()) {
    // Fatal, never a warning: see CaptureVolumePathsAndChmod's contract.
    return std::unexpected(cyclone::CacheError::IoError);
  }

  // Read existing generation counter (0 if file missing — first startup).
  ps_cache->generation_ = ReadGenerationFile(config.volume_path + ".gen");

  return ps_cache;
}

cyclone::CacheKey PageSpeedCache::ComposeKey(std::string_view url,
                                             std::string_view hostname,
                                             std::string_view scheme) {
  std::string normalized = NormalizeHostname(hostname);
  return ComposeKeyPreNormalized(url, normalized, scheme);
}

cyclone::CacheKey PageSpeedCache::ComposeKeyPreNormalized(
    std::string_view url, std::string_view normalized_hostname,
    std::string_view scheme) {
  // Key format: "scheme://hostname/url" (url already starts with /).
  // This produces distinct keys for http vs https.
  std::string combined;
  combined.reserve(scheme.size() + 3 + normalized_hostname.size() + url.size());
  combined.append(scheme);
  combined.append("://");
  combined.append(normalized_hostname);
  combined.append(url);
  return cyclone::CacheKey(combined);
}

cyclone::CacheKey PageSpeedCache::ComposeKeyPreNormalized(
    std::string_view url, std::string_view normalized_hostname,
    std::string_view scheme, std::string_view option_signature) {
  // The default context is not a special case that happens to work out — it is
  // the SAME CALL, so there is no second code path that could drift from the
  // first and quietly re-key an existing cache.
  if (IsDefaultOptionContext(option_signature)) {
    return ComposeKeyPreNormalized(url, normalized_hostname, scheme);
  }

  // "oc1:" is the key-namespace version, and it is separate from the payload
  // format version on purpose: the payload format can change without moving
  // anybody's key, and the key namespace can be re-cut without touching the
  // payload format. Entangling them would make either change cost the other.
  static constexpr std::string_view kContextKeyPrefix = "oc1:";
  std::string combined;
  combined.reserve(kContextKeyPrefix.size() + option_signature.size() + 1 +
                   scheme.size() + 3 + normalized_hostname.size() + url.size());
  combined.append(kContextKeyPrefix);
  combined.append(option_signature);
  combined.push_back('@');
  combined.append(scheme);
  combined.append("://");
  combined.append(normalized_hostname);
  combined.append(url);
  return cyclone::CacheKey(combined);
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadBestAlternate(std::string_view url,
                                  std::string_view hostname,
                                  std::string_view scheme,
                                  const CapabilityMask& mask,
                                  bool agent_request_entitled) {
  auto key = ComposeKey(url, hostname, scheme);
  auto result = ReadBestAlternateByKey(key, mask, agent_request_entitled);
  if (!result && handler_) {
    handler_->Info("Read miss for %.*s: %s", static_cast<int>(url.size()),
                   url.data(),
                   make_error_code(result.error()).message().c_str());
  }
  return result;
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadBestAlternateAgent(std::string_view url,
                                       std::string_view hostname,
                                       std::string_view scheme,
                                       const CapabilityMask& mask,
                                       bool agent_request_entitled) {
  return ReadBestAlternateAgentByKey(ComposeKey(url, hostname, scheme), mask,
                                     agent_request_entitled);
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadBestAlternateByKey(const cyclone::CacheKey& key,
                                       const CapabilityMask& mask,
                                       bool agent_request_entitled) {
  std::shared_lock lock(reset_mutex_);

  // Serialize the client mask (first 4 bytes LE) + the agent_optimize
  // entitled-request flag (byte[4]) into the selection context. The selector
  // reads byte[4] to gate the kAgentMarkdown sentinel; legacy
  // readers that ignore byte[4] are unaffected. OFF by default.
  uint32_t encoded = mask.Encode();
  std::array<std::byte, 5> mask_bytes{};
  std::memcpy(mask_bytes.data(), &encoded, 4);
  mask_bytes[4] =
      std::byte{static_cast<unsigned char>(agent_request_entitled ? 1 : 0)};

  cyclone::AlternateSelectionContext ctx;
  ctx.request_metadata = mask_bytes;

  auto result = cache_->read_alternate_sync(key, selector_, ctx);
  if (!result) {
    return std::unexpected(result.error());
  }

  // Metadata is stored as a content prefix — written atomically with
  // content and evolved independently of Cyclone (see ReadResult doc).
  ReadResult rr;
  rr.metadata_prefix_size_ =
      ParseMetadataPrefix(result->content(), &rr.metadata);
  // Treat metadata parse failure as MISS — prevents serving raw
  // metadata bytes as content.
  if (rr.metadata_prefix_size_ == 0) {
    return std::unexpected(cyclone::CacheError::NotFound);
  }
  rr.handle = std::move(*result);
  return rr;
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadBestAlternateAgentByKey(const cyclone::CacheKey& key,
                                            const CapabilityMask& mask,
                                            bool agent_request_entitled) {
  auto read = ReadBestAlternateByKey(key, mask, agent_request_entitled);
  if (!read.has_value()) return read;  // miss / parse-fail -> propagate

  // Only the agent markdown variant is gated; normal/non-agent reads pass
  // through unchanged.
  if ((read->metadata.full_mask & 0xFF) !=
      static_cast<uint32_t>(SentinelId::kAgentMarkdown)) {
    return read;
  }

  // Serve-time equality gate (formerly inlined in the nginx handler):
  // the selected markdown's stamped origin_html_hash MUST byte-equal the
  // live kContentHash sentinel for the key, else the origin advanced underneath
  // it -> REFUSE (NotFound) so the caller's miss path re-fetches.  An absent
  // sentinel or short/zero content also fails the compare -> refuse.
  auto live = ReadAlternateByKey(
      key, static_cast<AlternateId>(SentinelId::kContentHash));
  if (live.has_value()) {
    auto lc = live->content();
    if (lc.size() == AlternateMetadata::kHashSize &&
        std::memcmp(lc.data(), read->metadata.origin_html_hash.data(),
                    AlternateMetadata::kHashSize) == 0) {
      return read;  // binding fresh -> serve the markdown
    }
  }
  return std::unexpected(cyclone::CacheError::NotFound);  // stale/unbound
}

std::expected<ReadResult, cyclone::CacheError> PageSpeedCache::ReadAlternate(
    std::string_view url, std::string_view hostname, std::string_view scheme,
    AlternateId id) {
  auto key = ComposeKey(url, hostname, scheme);
  auto result = ReadAlternateByKey(key, id);
  if (!result && handler_) {
    handler_->Info("Read alternate 0x%02X miss for %.*s: %s",
                   static_cast<int>(id), static_cast<int>(url.size()),
                   url.data(),
                   make_error_code(result.error()).message().c_str());
  }
  return result;
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadAlternateByKey(const cyclone::CacheKey& key,
                                   AlternateId id) {
  std::shared_lock lock(reset_mutex_);
  ExactIdSelector selector(static_cast<cyclone::AlternateId>(id));
  cyclone::AlternateSelectionContext ctx;

  auto result = cache_->read_alternate_sync(key, selector, ctx);
  if (!result) {
    return std::unexpected(result.error());
  }

  // ReadAlternate is used for both content alternates and sentinels.
  // Sentinels have no metadata prefix (prefix_size == 0 is normal).
  // Don't reject on parse failure here — callers know what they're
  // reading.
  ReadResult rr;
  rr.metadata_prefix_size_ =
      ParseMetadataPrefix(result->content(), &rr.metadata);
  rr.handle = std::move(*result);
  return rr;
}

std::expected<WriteResult, cyclone::CacheError> PageSpeedCache::WriteAlternate(
    std::string_view url, std::string_view hostname, std::string_view scheme,
    AlternateId id, uint64_t content_length,
    const AlternateMetadata& metadata) {
  return WriteAlternateByKey(ComposeKey(url, hostname, scheme), id,
                             content_length, metadata, url);
}

std::expected<WriteResult, cyclone::CacheError>
PageSpeedCache::WriteAlternateByKey(const cyclone::CacheKey& key,
                                    AlternateId id, uint64_t content_length,
                                    const AlternateMetadata& metadata,
                                    std::string_view url_for_logs) {
  std::shared_lock lock(reset_mutex_);
  // Reject sentinel IDs — use WriteSentinel() instead.
  if (IsSentinel(id)) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  auto serialized = metadata.Serialize();
  if (serialized.size() > config_.max_metadata_size) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  const std::string_view url = url_for_logs;

  // Allocate space for metadata prefix + caller's content.
  // Guard against uint64_t overflow from extreme content_length values.
  if (content_length >
      std::numeric_limits<uint64_t>::max() - serialized.size()) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }
  uint64_t total_length = serialized.size() + content_length;
  auto result = cache_->write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), total_length);
  if (!result) {
    if (handler_ != nullptr) {
      handler_->Warning("Write alternate 0x%02X failed for %.*s: %s",
                        static_cast<int>(id), static_cast<int>(url.size()),
                        url.data(),
                        make_error_code(result.error()).message().c_str());
    }
    return std::unexpected(result.error());
  }

  // Store metadata in the Cyclone document header so that
  // list_alternates_sync returns it in AlternateInfo::header without
  // needing a separate content read (avoids N+1 chain traversals in the
  // /v1/cache/alternates API).
  result->set_header(serialized);

  // Write the metadata prefix immediately.  The caller will then
  // write_sync() the actual content bytes.
  auto meta_written = result->write_sync(serialized);
  if (!meta_written) {
    if (handler_ != nullptr) {
      handler_->Warning(
          "Write alternate 0x%02X metadata failed for %.*s: %s",
          static_cast<int>(id), static_cast<int>(url.size()), url.data(),
          make_error_code(meta_written.error()).message().c_str());
    }
    return std::unexpected(meta_written.error());
  }

  if (handler_ != nullptr) {
    handler_->Info("Write alternate 0x%02X for %.*s (%" PRIu64 " bytes)",
                   static_cast<int>(id), static_cast<int>(url.size()),
                   url.data(), content_length);
  }

  WriteResult wr;
  wr.handle_ = std::move(*result);
  return wr;
}

std::expected<WriteResult, cyclone::CacheError> PageSpeedCache::WriteSentinel(
    std::string_view url, std::string_view hostname, std::string_view scheme,
    SentinelId sentinel, uint64_t content_length) {
  std::shared_lock lock(reset_mutex_);
  auto key = ComposeKey(url, hostname, scheme);
  auto id = static_cast<cyclone::AlternateId>(sentinel);

  // The class is named with its id so the diagnostics below say WHICH
  // sentinel they are about — this writer serves every sentinel class, and
  // "Sentinel" alone would not distinguish a content hash from an llms.txt.
  char class_name[24];
  std::snprintf(class_name, sizeof(class_name), "Sentinel 0x%02X",
                static_cast<unsigned>(sentinel));

  // MEASURE what the previous store for this URL left behind, BEFORE
  // unlinking it — the same arrival-time shape WriteOriginalAlternate uses,
  // and for the same forced reason: this is a STREAMING writer whose entry
  // commits when the CALLER closes the handle, so there is no in-method
  // post-commit moment.  Nothing that matters is lost: a store whose unlink
  // reported the node still linked is counted when it happens, just below.
  if (auto nodes =
          CountChainNodes(key, static_cast<AlternateId>(id), url, class_name);
      nodes.has_value() && *nodes > 1) {
    sentinels_superseded_observed_.fetch_add(1, std::memory_order_relaxed);
    if (handler_ != nullptr) {
      handler_->Info(
          "%s for %.*s arrived at %zu nodes on the chain: reads still serve "
          "the newest, superseded nodes are not reclaimed here",
          class_name, static_cast<int>(url.size()), url.data(), *nodes);
    }
  }

  // Replace, do not add.  Without this the substrate links a second node
  // under this id on every re-record — and the classes routed through this
  // writer re-record in production (the content hash on every change, the
  // llms.txt pair on every TTL refresh), so the chain grew by a node per
  // refresh against the ceiling whose exhaustion stops the KEY from accepting
  // alternate writes.  See UnlinkSupersededAlternate for what this does and
  // does not achieve: one writer per URL keeps the chain at one node,
  // concurrent writers can still interleave into a chain that does not come
  // back down.
  UnlinkSupersededAlternate(key, static_cast<AlternateId>(id), url, class_name,
                            &sentinels_unlink_failed_);

  auto result = cache_->write_alternate_sync(key, id, content_length);
  if (!result) {
    if (handler_ != nullptr) {
      handler_->Warning("Write sentinel 0x%02X failed for %.*s: %s",
                        static_cast<int>(sentinel),
                        static_cast<int>(url.size()), url.data(),
                        make_error_code(result.error()).message().c_str());
    }
    return std::unexpected(result.error());
  }
  if (handler_ != nullptr) {
    handler_->Info("Write sentinel 0x%02X for %.*s (%" PRIu64 " bytes)",
                   static_cast<int>(sentinel), static_cast<int>(url.size()),
                   url.data(), content_length);
  }
  WriteResult wr;
  wr.handle_ = std::move(*result);
  return wr;
}

std::expected<WriteResult, cyclone::CacheError>
PageSpeedCache::WriteAgentAlternate(std::string_view url,
                                    std::string_view hostname,
                                    std::string_view scheme,
                                    uint64_t content_length,
                                    const AlternateMetadata& metadata) {
  std::shared_lock lock(reset_mutex_);
  // The agent_optimize markdown variant is the ONE sentinel
  // written WITH a metadata prefix: it is read back via the strict
  // ReadBestAlternate path, which rejects a missing prefix as NotFound.
  // WriteAlternate refuses sentinel ids and WriteSentinel writes no prefix, so
  // this dedicated path is required. The id is hard-coded to kAgentMarkdown.
  const auto id = static_cast<cyclone::AlternateId>(SentinelId::kAgentMarkdown);

  // Hardening (consolidated review): the variant is keyed under the
  // kAgentMarkdown sentinel, so its metadata MUST self-describe as
  // kAgentMarkdown — otherwise the strict read path
  // (ReadBestAlternateAgentByKey) rejects it as a non-agent variant, leaving an
  // unreadable record. Fail fast on a mis-stamped mask rather than write it.
  // The sole caller already sets this correctly; this guards the contract for
  // future callers. (Mirrors the read-path comparison.)
  if ((metadata.full_mask & 0xFF) !=
      static_cast<uint32_t>(SentinelId::kAgentMarkdown)) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  auto serialized = metadata.Serialize();
  if (serialized.size() > config_.max_metadata_size) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  auto key = ComposeKey(url, hostname, scheme);

  if (content_length >
      std::numeric_limits<uint64_t>::max() - serialized.size()) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  // MEASURE what the previous store for this URL left behind, BEFORE
  // unlinking it — arrival-time, like the other streaming writers (the entry
  // commits when the CALLER closes the handle), and nothing that matters is
  // lost: a store whose unlink reported the node still linked is counted when
  // it happens, just below.
  if (auto nodes = CountChainNodes(key, static_cast<AlternateId>(id), url,
                                   "Agent markdown");
      nodes.has_value() && *nodes > 1) {
    agent_markdown_superseded_observed_.fetch_add(1, std::memory_order_relaxed);
    if (handler_ != nullptr) {
      handler_->Info(
          "Agent markdown for %.*s arrived at %zu nodes on the chain: reads "
          "still serve the newest, superseded nodes are not reclaimed here",
          static_cast<int>(url.size()), url.data(), *nodes);
    }
  }

  // Replace, do not add — the variant is re-recorded whenever the origin's
  // rendered markdown is refreshed, and without the unlink each re-record
  // links another node under this id (see UnlinkSupersededAlternate for the
  // exact scope: single-writer replaces, concurrent writers can accumulate).
  UnlinkSupersededAlternate(key, static_cast<AlternateId>(id), url,
                            "Agent markdown", &agent_markdown_unlink_failed_);

  uint64_t total_length = serialized.size() + content_length;
  auto result = cache_->write_alternate_sync(key, id, total_length);
  if (!result) {
    if (handler_ != nullptr) {
      handler_->Warning("Write agent-markdown failed for %.*s: %s",
                        static_cast<int>(url.size()), url.data(),
                        make_error_code(result.error()).message().c_str());
    }
    return std::unexpected(result.error());
  }

  // Header + prefix, exactly as WriteAlternate, so the strict read path parses.
  result->set_header(serialized);
  auto meta_written = result->write_sync(serialized);
  if (!meta_written) {
    if (handler_ != nullptr) {
      handler_->Warning(
          "Write agent-markdown metadata failed for %.*s: %s",
          static_cast<int>(url.size()), url.data(),
          make_error_code(meta_written.error()).message().c_str());
    }
    return std::unexpected(meta_written.error());
  }
  if (handler_ != nullptr) {
    handler_->Info("Write agent-markdown for %.*s (%" PRIu64 " bytes)",
                   static_cast<int>(url.size()), url.data(), content_length);
  }

  WriteResult wr;
  wr.handle_ = std::move(*result);
  return wr;
}

std::expected<WriteResult, cyclone::CacheError>
PageSpeedCache::WriteOriginalAlternate(std::string_view url,
                                       std::string_view hostname,
                                       std::string_view scheme,
                                       uint64_t content_length,
                                       const AlternateMetadata& metadata) {
  std::shared_lock lock(reset_mutex_);
  const auto id =
      static_cast<cyclone::AlternateId>(SentinelId::kOriginalContent);

  // The entry must self-describe as the class it is stored under.  A blob
  // found at 0x0C whose metadata claims a capability mask is not a durable
  // original that was mis-stamped — it is a variant written into the wrong
  // slot, and the reader has no way to tell which.  Refuse rather than store
  // an entry whose own metadata contradicts its id.
  if ((metadata.full_mask & 0xFF) !=
      static_cast<uint32_t>(SentinelId::kOriginalContent)) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  // Declared length over the cap: skip the store before the write handle
  // exists.  This is not merely an early version of the streaming check — the
  // storage layer reserves the declared length on the first write, so
  // accepting a declaration it will honour is already the memory spike.
  const uint64_t cap = EffectiveMaxOriginalContentLength();
  if (content_length > cap) {
    originals_over_cap_skipped_.fetch_add(1, std::memory_order_relaxed);
    if (handler_ != nullptr) {
      handler_->Info("Original not stored for %.*s: %" PRIu64
                     " bytes exceeds the %" PRIu64 "-byte cap",
                     static_cast<int>(url.size()), url.data(), content_length,
                     cap);
    }
    return std::unexpected(cyclone::CacheError::NoSpace);
  }

  auto serialized = metadata.Serialize();
  if (serialized.size() > config_.max_metadata_size) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  auto key = ComposeKey(url, hostname, scheme);

  if (content_length >
      std::numeric_limits<uint64_t>::max() - serialized.size()) {
    return std::unexpected(cyclone::CacheError::InvalidArgument);
  }

  // MEASURE what the previous store for this URL left behind, BEFORE unlinking
  // it.  Two things make the arrival of the next store the right moment for
  // this class, rather than the moment of commit:
  //
  //   * This is a STREAMING writer.  The entry commits when the CALLER closes
  //     the handle, long after this method returns, so there is no in-method
  //     point at which the new node exists to be counted.  Reaching that
  //     moment would mean carrying a pointer back into this cache on the
  //     handle and walking the chain from the caller's close — outside the
  //     reset lock this method holds — which buys immediacy at the price of a
  //     lifetime and locking hazard the class does not otherwise have.
  //   * Nothing is lost that matters.  A store that grows the chain
  //     DETERMINISTICALLY — one whose unlink reported the old node still
  //     linked — is already counted at the moment it happens, just below.
  //     What this measures is the other case, a concurrent interleaving,
  //     which by definition needs a later observer anyway.
  //
  // So: "stores that ARRIVED to find more than one node", which is the
  // accumulation signal one store later.  Costs a chain walk, next to the one
  // the unlink below performs anyway.
  if (auto nodes = CountChainNodes(key, static_cast<AlternateId>(id), url,
                                   "Durable original");
      nodes.has_value() && *nodes > 1) {
    originals_superseded_observed_.fetch_add(1, std::memory_order_relaxed);
    if (handler_ != nullptr) {
      handler_->Info(
          "Durable original for %.*s arrived at %zu nodes on the chain: reads "
          "still serve the newest, superseded nodes are not reclaimed here",
          static_cast<int>(url.size()), url.data(), *nodes);
    }
  }

  // Replace, do not add.  Without this the substrate links a second node
  // under this id on every re-record — and originals are whole response
  // bodies, so a superseded node costs volume as well as chain depth.  See
  // UnlinkSupersededAlternate for what it does and does not achieve: with one
  // writer per URL this keeps the chain at one node, and concurrent writers
  // can still interleave into a chain that does not come back down.
  UnlinkSupersededAlternate(key, static_cast<AlternateId>(id), url,
                            "Durable original", &originals_unlink_failed_);

  uint64_t total_length = serialized.size() + content_length;
  auto result = cache_->write_alternate_sync(key, id, total_length);
  if (!result) {
    if (handler_ != nullptr) {
      handler_->Warning("Write original failed for %.*s: %s",
                        static_cast<int>(url.size()), url.data(),
                        make_error_code(result.error()).message().c_str());
    }
    return std::unexpected(result.error());
  }

  // Header + prefix, exactly as WriteAlternate, so an exact-id read parses the
  // metadata back.  Written through the raw handle, BEFORE the cap is armed:
  // the cap is a content cap, and the caller's budget must be the number it
  // was given rather than that number minus a prefix it never sees.
  result->set_header(serialized);
  auto meta_written = result->write_sync(serialized);
  if (!meta_written) {
    if (handler_ != nullptr) {
      handler_->Warning(
          "Write original metadata failed for %.*s: %s",
          static_cast<int>(url.size()), url.data(),
          make_error_code(meta_written.error()).message().c_str());
    }
    return std::unexpected(meta_written.error());
  }

  if (handler_ != nullptr) {
    handler_->Info("Write original for %.*s (%" PRIu64 " bytes)",
                   static_cast<int>(url.size()), url.data(), content_length);
  }

  WriteResult wr;
  wr.handle_ = std::move(*result);
  wr.content_cap_ = cap;
  wr.over_cap_counter_ = &originals_over_cap_skipped_;
  return wr;
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadOriginalAlternate(std::string_view url,
                                      std::string_view hostname,
                                      std::string_view scheme) {
  auto key = ComposeKey(url, hostname, scheme);
  auto result = ReadOriginalAlternateByKey(key);
  if (!result && handler_ != nullptr) {
    handler_->Info("Original miss for %.*s: %s", static_cast<int>(url.size()),
                   url.data(),
                   make_error_code(result.error()).message().c_str());
  }
  return result;
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadOriginalAlternateByKey(const cyclone::CacheKey& key) {
  auto result = ReadAlternateByKey(
      key, static_cast<AlternateId>(SentinelId::kOriginalContent));
  if (!result) return result;
  // The class always carries a metadata prefix.  A blob at this id without a
  // parseable one is not this class — it is a peer speaking a format this
  // build does not have, or a slot someone else wrote — and serving its raw
  // bytes as content would hand the reader a metadata header as if it were
  // the origin's body.  MISS, and the caller's own miss path runs.
  if (result->metadata_prefix_size_ == 0) {
    return std::unexpected(cyclone::CacheError::NotFound);
  }
  // And it must say it is this class.  This is the reader half of the write
  // side's self-description check, and it is not redundant with it: the write
  // side can only speak for entries THIS build wrote through that entry point.
  // An entry can also arrive at this id from a peer build, from a future
  // in-tree path, or from a raw alternate write — and for those the prefix
  // parses fine while every field behind it (content type, flags, the origin
  // state that governs lifetime) belongs to a different class.  The write
  // side's own comment says the reader has no way to tell a mis-stamped
  // original from a misplaced variant; it does have one, so it uses it.
  // Symmetric with ReadBestAlternateAgentByKey, which refuses a selected blob
  // whose low mask byte is not its own class.
  if ((result->metadata.full_mask & 0xFF) !=
      static_cast<uint32_t>(SentinelId::kOriginalContent)) {
    return std::unexpected(cyclone::CacheError::NotFound);
  }
  return result;
}

void PageSpeedCache::UnlinkSupersededAlternate(
    const cyclone::CacheKey& key, AlternateId id, std::string_view url,
    std::string_view class_name, std::atomic<uint64_t>* unlink_failed) {
  const auto cy_id = static_cast<cyclone::AlternateId>(id);
  // The RAM tier is write-around — populated by reads, untouched by writes —
  // so a reader in this process would otherwise keep being answered with the
  // entry this call is replacing.
  cache_->evict_from_ram_cache(key, cy_id);
  auto unlinked = cache_->remove_alternate_sync(key, cy_id);
  if (unlinked.has_value() || !UnlinkLeftEntryLinked(unlinked.error())) {
    return;
  }
  unlink_failed->fetch_add(1, std::memory_order_relaxed);
  if (handler_ != nullptr) {
    handler_->Warning(
        "%.*s unlink failed for %.*s (%s); storing anyway — the superseded "
        "entry stays linked and the chain grows by one",
        static_cast<int>(class_name.size()), class_name.data(),
        static_cast<int>(url.size()), url.data(),
        make_error_code(unlinked.error()).message().c_str());
  }
}

std::optional<size_t> PageSpeedCache::CountChainNodes(
    const cyclone::CacheKey& key, AlternateId id, std::string_view url,
    std::string_view class_name) {
  auto listed = cache_->list_alternates_sync(key);
  if (!listed.has_value()) {
    if (listed.error() == cyclone::CacheError::NotFound ||
        listed.error() == cyclone::CacheError::AlternateNotFound) {
      // A key with no alternates yet: the first store for a URL lands here
      // by design, and the depth is knowably zero, not unmeasured.
      return 0;
    }
    if (handler_ != nullptr) {
      handler_->Info(
          "%.*s chain listing failed for %.*s (%s): depth unmeasured for "
          "this store",
          static_cast<int>(class_name.size()), class_name.data(),
          static_cast<int>(url.size()), url.data(),
          make_error_code(listed.error()).message().c_str());
    }
    return std::nullopt;
  }
  size_t nodes = 0;
  for (const auto& alt : *listed) {
    if (static_cast<AlternateId>(alt.id) == id) ++nodes;
  }
  return nodes;
}

std::expected<PageSpeedCache::HeadersSidecarWrite, cyclone::CacheError>
PageSpeedCache::WriteHeadersSidecar(std::string_view url,
                                    std::string_view hostname,
                                    std::string_view scheme,
                                    std::span<const HeaderField> headers) {
  std::shared_lock lock(reset_mutex_);
  const auto id =
      static_cast<cyclone::AlternateId>(SentinelId::kHeadersSidecar);

  // The gate runs HERE, on the caller's raw header set, and its verdict is
  // the only thing that decides whether bytes are stored.  A caller cannot
  // pre-curate a block: that decision is what this class exists to own.
  SidecarClassification classification = ClassifyForHeadersSidecar(headers);

  HeadersSidecarWrite outcome;
  outcome.verdict = classification.verdict;

  if (!classification.swap_eligible()) {
    // Counted, not returned as an error: this is the ordinary shape of a
    // response that is served plain.  The two counters are DISJOINT — see
    // their accessors — so that the permanent class is never reported as part
    // of the closable one.
    if (classification.verdict == SidecarVerdict::kNeverOptimized) {
      headers_sidecar_never_optimized_.fetch_add(1, std::memory_order_relaxed);
    } else {
      headers_sidecar_fall_through_.fetch_add(1, std::memory_order_relaxed);
    }
    if (handler_ != nullptr) {
      handler_->Info("Headers sidecar not stored for %.*s: %s",
                     static_cast<int>(url.size()), url.data(),
                     classification.reasons.empty()
                         ? "no reason recorded"
                         : classification.reasons.front().c_str());
    }
    return outcome;
  }

  if (classification.payload.empty()) {
    // Swap-eligible with nothing to carry: every header this response sent is
    // already reproduced by the entry metadata.  Not a fall-through, and not
    // an entry.
    return outcome;
  }

  auto key = ComposeKey(url, hostname, scheme);

  // ONE ENTRY PER URL, as far as it can be made true from here.  At the
  // pinned storage layer a write to an id that already exists LINKS A SECOND
  // ALTERNATE under the same id: reads still find the newest, but the chain
  // grows by one node per re-store, and a chain has a depth budget whose
  // exhaustion stops the KEY from accepting alternate writes.  For a class
  // re-offered on every origin response for its URL that is the difference
  // between one entry and a pile of superseded ones, so the old entry is
  // unlinked first.  The RAM copy goes with it: that tier is write-around, so
  // a reader in this process would otherwise keep being answered with the
  // block this call is replacing.
  //
  // What this does NOT achieve, stated where the code is rather than only in
  // the header: unlink-then-write is not one operation, so concurrent writers
  // for the same URL can still interleave into a chain of two, which is a
  // fixed point. The counters below are how that stays visible.
  UnlinkSupersededAlternate(key, static_cast<AlternateId>(id), url,
                            "Headers sidecar", &headers_sidecar_unlink_failed_);

  auto result = cache_->write_alternate_sync(
      key, id, static_cast<uint64_t>(classification.payload.size()));
  if (!result) {
    if (handler_ != nullptr) {
      handler_->Warning("Write headers sidecar failed for %.*s: %s",
                        static_cast<int>(url.size()), url.data(),
                        make_error_code(result.error()).message().c_str());
    }
    return std::unexpected(result.error());
  }

  // No metadata prefix: this class is a VERSIONED PAYLOAD — its first byte is
  // its own format version, written by ClassifyForHeadersSidecar, and the
  // whole blob is the block.
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(classification.payload.data()),
      classification.payload.size()));
  if (!written) {
    if (handler_ != nullptr) {
      handler_->Warning("Write headers sidecar payload failed for %.*s: %s",
                        static_cast<int>(url.size()), url.data(),
                        make_error_code(written.error()).message().c_str());
    }
    return std::unexpected(written.error());
  }
  auto closed = result->close_sync();
  if (!closed) {
    if (handler_ != nullptr) {
      handler_->Warning("Close headers sidecar failed for %.*s: %s",
                        static_cast<int>(url.size()), url.data(),
                        make_error_code(closed.error()).message().c_str());
    }
    return std::unexpected(closed.error());
  }

  // The RAM tier is write-around, so evict again now that the new bytes are
  // committed: the read that repopulates it must be the one that finds them.
  cache_->evict_from_ram_cache(key, id);

  // MEASURE the depth this pass ended at, rather than assuming the one the
  // code path intends.  The single-writer path leaves exactly one node; a
  // concurrent writer, or an unlink that failed above, leaves more, and that
  // is the limitation this class cannot close at this storage version.  An
  // unmeasured limitation is indistinguishable from an absent one.
  //
  // The cost is one chain walk per store, paid deliberately: the alternative
  // is a documented caveat nobody can confirm or refute from a running cache.
  // OBSERVABILITY ONLY — nothing reads this to decide anything, and the store
  // has already committed by the time it is taken.
  if (auto nodes = CountChainNodes(key, static_cast<AlternateId>(id), url,
                                   "Headers sidecar");
      nodes.has_value() && *nodes > 1) {
    headers_sidecar_superseded_observed_.fetch_add(1,
                                                   std::memory_order_relaxed);
    if (handler_ != nullptr) {
      handler_->Info(
          "Headers sidecar for %.*s left %zu nodes on the chain: reads "
          "still serve the newest, superseded nodes are not reclaimed here",
          static_cast<int>(url.size()), url.data(), *nodes);
    }
  }

  if (handler_ != nullptr) {
    handler_->Info("Write headers sidecar for %.*s (%zu fields, %zu bytes)",
                   static_cast<int>(url.size()), url.data(),
                   classification.fields, classification.payload.size());
  }

  outcome.stored = true;
  outcome.fields = classification.fields;
  outcome.payload_bytes = classification.payload.size();
  return outcome;
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadHeadersSidecar(std::string_view url,
                                   std::string_view hostname,
                                   std::string_view scheme) {
  auto key = ComposeKey(url, hostname, scheme);
  auto result = ReadHeadersSidecarByKey(key);
  if (!result && handler_ != nullptr) {
    handler_->Info("Headers sidecar miss for %.*s: %s",
                   static_cast<int>(url.size()), url.data(),
                   make_error_code(result.error()).message().c_str());
  }
  return result;
}

std::expected<ReadResult, cyclone::CacheError>
PageSpeedCache::ReadHeadersSidecarByKey(const cyclone::CacheKey& key) {
  auto result = ReadAlternateByKey(
      key, static_cast<AlternateId>(SentinelId::kHeadersSidecar));
  if (!result) return result;
  // Validate on the way out rather than leaving it to every caller.  The
  // parse is the class's own membership test: a blob at this id that does not
  // parse as this format is not this class — a peer speaking a version this
  // build does not have, or a slot someone else wrote — and the caller gets
  // the class or it gets nothing.
  if (!ParseHeadersSidecar(HeadersSidecarPayload(*result)).has_value()) {
    return std::unexpected(cyclone::CacheError::NotFound);
  }
  return result;
}

std::string_view PageSpeedCache::HeadersSidecarPayload(
    const ReadResult& result) {
  // The RAW blob, deliberately: ReadResult::content() strips an
  // entry-metadata prefix, and this class does not carry one.  Today the
  // prefix parse cannot succeed on a sidecar payload (its first byte is below
  // the lowest metadata version), so the two agree — but relying on that
  // would make a format-version choice load-bearing for content offsets,
  // which is exactly the coupling the version byte exists to avoid.
  auto blob = result.handle.content();
  return std::string_view(reinterpret_cast<const char*>(blob.data()),
                          blob.size());
}

std::expected<void, cyclone::CacheError> PageSpeedCache::Remove(
    std::string_view url, std::string_view hostname, std::string_view scheme) {
  std::shared_lock lock(reset_mutex_);
  auto key = ComposeKey(url, hostname, scheme);
  if (handler_ != nullptr) {
    handler_->Info("Remove %.*s", static_cast<int>(url.size()), url.data());
  }
  return cache_->remove_sync(key);
}

std::expected<void, cyclone::CacheError> PageSpeedCache::RemoveAlternate(
    std::string_view url, std::string_view hostname, std::string_view scheme,
    AlternateId id) {
  std::shared_lock lock(reset_mutex_);
  auto key = ComposeKey(url, hostname, scheme);
  if (handler_ != nullptr) {
    handler_->Info("RemoveAlternate %.*s id=0x%02x",
                   static_cast<int>(url.size()), url.data(),
                   static_cast<unsigned>(id));
  }
  auto cy_id = static_cast<cyclone::AlternateId>(id);
  // Evict the per-process RAM-tier copy first so a concurrent read in this
  // process cannot re-serve the removed alternate from RAM (no-op when the
  // RAM tier is disabled).
  cache_->evict_from_ram_cache(key, cy_id);
  return cache_->remove_alternate_sync(key, cy_id);
}

std::expected<size_t, cyclone::CacheError>
PageSpeedCache::RemoveAlternatesExcept(std::string_view url,
                                       std::string_view hostname,
                                       std::string_view scheme,
                                       std::span<const AlternateId> preserve) {
  std::shared_lock lock(reset_mutex_);
  constexpr auto kOriginal =
      static_cast<AlternateId>(SentinelId::kOriginalContent);
  for (AlternateId keep : preserve) {
    if (keep == kOriginal) {
      // See the header: the durable original dies WITH the family or the
      // family is rebuilt from bytes whose origin is gone.  Refuse the whole
      // call — a partially honoured purge is the failure this exists to stop.
      return std::unexpected(cyclone::CacheError::InvalidArgument);
    }
  }
  auto is_preserved = [&](AlternateId id) {
    for (AlternateId k : preserve) {
      if (k == id) return true;
    }
    return false;
  };

  auto key = ComposeKey(url, hostname, scheme);

  // One listing is one traversal of the key's alternate chain, and a
  // traversal is CAPPED, so a listing need not show the whole chain and
  // removing "everything listed" need not empty the key.  The storage layer
  // unlinks a superseded same-id document as it writes, which keeps an
  // ordinary chain far below that cap — but that splice is BEST-EFFORT by its
  // own contract (one that cannot proceed safely is deferred), so a short
  // listing stays possible and the pass repeats while non-preserved entries
  // are still listed AND the listing is observably changing.
  //
  // A chain the storage layer cannot walk — its nodes linking in a cycle,
  // which its write path documents as a post-wrap possibility — is the shape
  // that no number of passes can clear: every removal reports success (it
  // re-points the directory at the head's successor, which is the head) and
  // the listing comes back identical.  The write path resets such a chain
  // when every node it can see carries the id being written, but a key that
  // holds an original as well as a variant is past that heal, and neither the
  // read walk nor the removal walk guards against the cycle at all.  That
  // NO-OP SUCCESS is the only signature that escalates to dropping the whole
  // key, and the escalation costs a preserved entry, so it is deliberately
  // narrow:
  //
  //   * a removal that FAILED (anything but "the alternate is already gone")
  //     is a maybe-transient storage verdict — the pinned storage layer
  //     reports Busy when a concurrent wrap races a middle-node removal, and
  //     a middle-node removal is precisely what a PRESERVED head produces.
  //     Such a pass ends the loop and never escalates: the stale variants
  //     survive this purge and the next refresh retries, exactly as before
  //     this function grew passes.
  //   * a listing that could not be READ is not evidence of anything.  The
  //     read has its own bounded revalidation and reports the same verdict
  //     for a lost revalidation as for genuine corruption, so it ends the
  //     loop without escalating too.
  //
  // Progress is measured on the LISTING, not on the removals' return values,
  // so a no-op success is detected on the next pass rather than after the
  // budget is spent.
  struct ChainShape {
    size_t entries = 0;
    uint64_t head_offset = 0;
    bool operator==(const ChainShape&) const = default;
  };
  auto shape_of = [](const std::vector<cyclone::AlternateInfo>& alts) {
    ChainShape shape;
    shape.entries = alts.size();
    shape.head_offset = alts.empty() ? 0 : alts.front().disk_offset;
    return shape;
  };

  constexpr int kMaxPasses = 4;
  size_t removed = 0;
  bool unwalkable = false;  // removals succeeded, the listing did not change
  std::optional<ChainShape> previous_shape;

  for (int pass = 0; pass < kMaxPasses; ++pass) {
    auto alternates = cache_->list_alternates_sync(key);
    if (!alternates) {
      if (pass == 0 && alternates.error() != cyclone::CacheError::NotFound) {
        return std::unexpected(alternates.error());
      }
      // NotFound: the key is gone, nothing survives.  Any other verdict: the
      // listing could not be read, which is not evidence of a survivor.
      break;
    }

    const ChainShape shape = shape_of(*alternates);
    if (previous_shape.has_value() && shape == *previous_shape) {
      // The previous pass removed entries and reported success, yet the chain
      // looks exactly as it did: nothing can be unlinked from it.
      unwalkable = true;
      break;
    }
    previous_shape = shape;

    bool listed_non_preserved = false;
    bool removal_failed = false;
    for (const auto& alt : *alternates) {
      const auto id = static_cast<AlternateId>(alt.id);
      if (is_preserved(id)) continue;
      listed_non_preserved = true;
      auto cy_id = static_cast<cyclone::AlternateId>(id);
      // Evict this process's RAM copy first, for the reason RemoveAlternate
      // does: the RAM tier is write-around, so a concurrent read here could
      // otherwise re-serve an alternate that is gone from disk.
      cache_->evict_from_ram_cache(key, cy_id);
      auto dropped = cache_->remove_alternate_sync(key, cy_id);
      if (dropped.has_value()) {
        ++removed;
      } else if (dropped.error() != cyclone::CacheError::AlternateNotFound) {
        removal_failed = true;
      }
    }
    if (!listed_non_preserved) {
      break;  // Only preserved entries (or nothing) remain.
    }
    if (removal_failed) {
      break;  // Maybe transient: leave the rest to the next refresh.
    }
  }

  if (unwalkable) {
    // Confirm POSITIVELY before destroying anything: only an enumerated
    // non-preserved entry justifies the drop.  A listing that cannot be read
    // is retried and, if it stays unreadable, taken as no evidence at all.
    bool survivor = false;
    for (int attempt = 0; attempt < 3 && !survivor; ++attempt) {
      auto remaining = cache_->list_alternates_sync(key);
      if (!remaining) {
        if (remaining.error() == cyclone::CacheError::NotFound) break;
        std::this_thread::yield();
        continue;  // Unreadable — retry; never escalate on a failed read.
      }
      for (const auto& alt : *remaining) {
        if (!is_preserved(static_cast<AlternateId>(alt.id))) {
          survivor = true;
          break;
        }
      }
      if (!survivor) break;  // Positively free of non-preserved entries.
    }
    if (survivor) {
      if (handler_ != nullptr) {
        handler_->Warning(
            "RemoveAlternatesExcept %.*s: the alternate chain cannot be "
            "unlinked (removals report success and the chain is unchanged); "
            "removing the whole key",
            static_cast<int>(url.size()), url.data());
      }
      auto dropped = cache_->remove_sync(key);
      if (!dropped && dropped.error() != cyclone::CacheError::NotFound) {
        return std::unexpected(dropped.error());
      }
      // Counted as one removal: the caller's measure is progress, and the
      // key's whole directory entry went with this call.
      ++removed;
    }
  }

  if (handler_ != nullptr) {
    handler_->Info("RemoveAlternatesExcept %.*s removed=%zu",
                   static_cast<int>(url.size()), url.data(), removed);
  }
  return removed;
}

void PageSpeedCache::EvictAlternateFromRamCache(std::string_view url,
                                                std::string_view hostname,
                                                std::string_view scheme,
                                                AlternateId id) {
  std::shared_lock lock(reset_mutex_);
  auto key = ComposeKey(url, hostname, scheme);
  cache_->evict_from_ram_cache(key, static_cast<cyclone::AlternateId>(id));
}

bool PageSpeedCache::AlternateExists(std::string_view url,
                                     std::string_view hostname,
                                     std::string_view scheme, AlternateId id) {
  std::shared_lock lock(reset_mutex_);
  auto key = ComposeKey(url, hostname, scheme);
  auto alts = cache_->list_alternates_sync(key);
  if (!alts) {
    return false;
  }
  auto target = static_cast<cyclone::AlternateId>(id);
  for (const auto& alt : *alts) {
    if (alt.id == target) {
      return true;
    }
  }
  return false;
}

std::expected<std::vector<cyclone::AlternateInfo>, cyclone::CacheError>
PageSpeedCache::ListAlternates(std::string_view url, std::string_view hostname,
                               std::string_view scheme) {
  std::shared_lock lock(reset_mutex_);
  auto key = ComposeKey(url, hostname, scheme);
  return cache_->list_alternates_sync(key);
}

cyclone::CacheStats PageSpeedCache::Stats() const {
  std::shared_lock lock(reset_mutex_);
  return cache_->stats();
}

uint64_t PageSpeedCache::VolumeCapacityBytes() const {
  std::shared_lock lock(reset_mutex_);
  return cache_->total_capacity();
}

std::expected<void, cyclone::CacheError> PageSpeedCache::ResetVolume() {
  // Exclusive lock: blocks until all in-flight operations complete.
  std::unique_lock lock(reset_mutex_);

  // 1. Stop the old Cyclone cache (closes volumes, unmaps files).
  if (cache_) {
    cache_->stop();
    cache_.reset();
  }

  // 2. Delete the volume file(s) so Cyclone creates fresh ones.  The
  // clean-slate helper is used rather than the remembered instance paths so
  // that a fingerprint-named file this instance did not open — one left by an
  // earlier geometry of the SAME format — goes too.  It stops at the format
  // boundary: a volume written in another format is not this build's data and
  // survives the reset (see IsFingerprintSibling), so a purge no longer costs
  // an operator the rollback they were keeping.
  RemoveVolumeFiles(config_.volume_path);

  // 3. Create a fresh Cyclone cache with the same config.
  auto cache_result = cyclone::Cache::create(cyclone_config_);
  if (!cache_result) {
    return std::unexpected(cache_result.error());
  }

  auto& cache = *cache_result;
  auto vol_result = cache->add_volume(config_.volume_path, config_.volume_size);
  if (!vol_result) {
    return std::unexpected(vol_result.error());
  }

  auto start_result = cache->start();
  if (!start_result) {
    return std::unexpected(start_result.error());
  }

  cache_ = std::move(cache);
  if (!CaptureVolumePathsAndChmod()) {
    // Fatal, never a warning: see CaptureVolumePathsAndChmod's contract.
    return std::unexpected(cyclone::CacheError::IoError);
  }

  // Bump generation so other processes detect the reset.
  ++generation_;
  if (!WriteGenerationFile() && (handler_ != nullptr)) {
    handler_->Warning(
        "Cache reset succeeded but generation file write failed; "
        "other processes may not detect the reset (generation %" PRIu64 ")",
        generation_);
  }

  if (handler_ != nullptr) {
    handler_->Info("Cache volume reset: %s (generation %" PRIu64 ")",
                   config_.volume_path.c_str(), generation_);
  }
  return {};
}

namespace {

// Why three states and not a bool: "the mode could not be set" and "this is
// not the file the cache was told to use" are different reports, and the
// caller has to be able to say which one it is refusing on.
enum class VolumeModeStatus : std::uint8_t {
  kOk,
  kNotALoneRegularFile,
  kFailed
};

// Give a volume file its exact mode without ever following a symlink.
// Returns kFailed with errno set on failure.  POSIX-only: on Windows the
// mode bits are not the access model and there is nothing to set.
VolumeModeStatus SetVolumeFileMode(const std::string& path) {
#ifdef _WIN32
  (void)path;
  return VolumeModeStatus::kOk;
#else
  // O_NOFOLLOW: a symlink at this path fails with ELOOP rather than being
  // followed, which is exactly the case the caller must refuse on.
  int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    // errno is the caller's diagnosis (ELOOP == symlink).
    return VolumeModeStatus::kFailed;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return VolumeModeStatus::kFailed;
  }
  // The volume has to be a regular file that nothing else has a name for.
  // O_NOFOLLOW ruled out a symlink AT this path, but a HARD LINK is the same
  // substitution with no symlink to catch: the cache directory is
  // group-writable by design, so during the vacancy ResetVolume opens
  // between removing the old file and the new one appearing, a peer can link
  // a name of its own to the volume -- or link this name to bytes outside
  // the cache.  st_nlink == 1 is the cheap statement that nobody did.
  if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
    ::close(fd);
    return VolumeModeStatus::kNotALoneRegularFile;
  }
  // Already right: nothing to set, and the caller's question is answered.
  //
  // This function runs in BOTH peers of the shared cache.  The daemon creates
  // the volume and owns it; a web-server worker only ever opens one the daemon
  // already made.  fchmod is owner-only (POSIX: effective uid == owner, or
  // CAP_FOWNER), so the worker CANNOT set a mode it did not need set -- it
  // gets EPERM on a correctly-installed host and, without this check, refuses
  // the volume and takes in-place optimization down for the life of the
  // process.  What the caller has to know is that the mode is 0660; who put it
  // there is not part of the contract.  Read through the same O_NOFOLLOW
  // descriptor, so it still cannot be a symlink's target.
  if ((st.st_mode & 07777) == 0660) {
    ::close(fd);
    return VolumeModeStatus::kOk;
  }
  const bool ok = ::fchmod(fd, 0660) == 0;
  const int saved_errno = errno;
  ::close(fd);
  errno = saved_errno;
  return ok ? VolumeModeStatus::kOk : VolumeModeStatus::kFailed;
#endif
}

}  // namespace

bool PageSpeedCache::CaptureVolumePathsAndChmod() {
  volume_file_paths_.clear();
  bool all_ok = true;
  for (const auto& vf : cache_->volume_files()) {
    volume_file_paths_.push_back(vf.file_path);
    // Ensure the volume file is readable/writable by the daemon AND its
    // cache-sharing peers (web-server workers, group `pagespeed`), never by
    // the world.  Explicit chmod after create — never umask-derived — is
    // the worker-side fix for cyclone's 0666 create-mode request
    // (cyclone src/core/volume.cpp), preferred over patching cyclone to
    // avoid a cyclone release + Bazel pin move.  Was 0666 before the
    // H1-H3 privilege drop.
    // Set through a descriptor opened O_NOFOLLOW rather than a path
    // chmod(): the cache directory is group-writable by design (3770), so a
    // path-based chmod would follow a symlink a peer could have planted and
    // relax the mode of a file outside the cache. fchmod() can only ever
    // act on the file we opened.
    const VolumeModeStatus status = SetVolumeFileMode(vf.file_path);
    if (status != VolumeModeStatus::kOk) {
      const int saved_errno = errno;
      if (handler_ != nullptr) {
        if (status == VolumeModeStatus::kNotALoneRegularFile) {
          handler_->Error(
              "Refusing to use cache volume %s: it is not a lone regular "
              "file. Either it is not a regular file at all, or a second "
              "name is hard-linked to the same bytes -- which means the "
              "volume can be read and rewritten through a name the cache "
              "never chose. Remove it and let the daemon recreate the "
              "volume.",
              vf.file_path.c_str());
        } else if (saved_errno == ELOOP) {
          handler_->Error(
              "Refusing to use cache volume %s: it is a SYMLINK. The volume "
              "file must be a regular file the cache itself created; a "
              "symlink here means the bytes would be written somewhere the "
              "cache never chose. Remove it and let the daemon recreate the "
              "volume.",
              vf.file_path.c_str());
        } else {
          handler_->Error(
              "Refusing to use cache volume %s: could not set its mode to "
              "0660 (%s). The volume is shared with the web server through "
              "group permissions, so a mode that could not be set is a "
              "sharing boundary that cannot be vouched for.",
              vf.file_path.c_str(), strerror(saved_errno));
        }
      }
      all_ok = false;
    }
  }
  return all_ok;
}

std::string PageSpeedCache::VolumeFilePath() const {
  std::shared_lock lock(reset_mutex_);
  return volume_file_paths_.empty() ? std::string() : volume_file_paths_[0];
}

namespace {

// True iff `name` is "<stem>-<current format major>-<16 lowercase hex><ext>" —
// the shape Cyclone's structural-fingerprint naming produces for a volume
// configured at "<stem><ext>" (mirrors Cyclone's own tail parse).
//
// THE FORMAT MAJOR IS PART OF THE MATCH, not just part of the shape.  The
// digit run between the dashes is the on-disk format the file was written in,
// which is exactly what the fingerprint exists to keep apart: a build opens
// the file bearing ITS major and never touches the others.  Matching any digit
// run made every earlier format's volume a sibling of this one, so the
// clean-slate sweep below deleted files this build does not own and cannot
// read — and those files are the warm rollback an operator keeps deliberately
// after a format change.  A file whose major is not ours is therefore not a
// sibling at all.  The constant comes from the storage layer, so a future
// format bump moves this rule with it rather than leaving a literal behind.
bool IsFingerprintSibling(const std::string& name, const std::string& stem,
                          const std::string& ext) {
  const std::string major =
      std::to_string(cyclone::VolumeHeader::kFormatVersionMajor);
  const size_t mid_size = 1 + major.size() + 1 + 16;  // "-" major "-" 16-hex
  if (name.size() != stem.size() + mid_size + ext.size()) {
    return false;
  }
  if (name.compare(0, stem.size(), stem) != 0) {
    return false;
  }
  if (!ext.empty() &&
      name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
    return false;
  }
  const std::string mid = name.substr(stem.size(), mid_size);
  const size_t hash_begin = mid.size() - 16;
  if (mid.front() != '-' || mid[hash_begin - 1] != '-') {
    return false;
  }
  if (mid.compare(1, major.size(), major) != 0) {
    return false;  // Another format's volume: not ours to sweep.
  }
  for (size_t i = hash_begin; i < mid.size(); ++i) {
    const char c = mid[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

}  // namespace

void PageSpeedCache::RemoveVolumeFiles(const std::string& volume_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  // The raw legacy path and its small-tier sibling.
  fs::remove(volume_path, ec);
  fs::remove(volume_path + ".small", ec);
  // Fingerprint-shaped siblings of THIS format, both shapes:
  // "<stem>-<major>-<hex><ext>" for the main volume,
  // "<basename>-<major>-<hex>.small" for the small-tier sibling (Cyclone
  // fingerprints "<volume_path>.small", whose stem is the full basename of
  // volume_path).  A volume written in an earlier format is deliberately left
  // alone — see IsFingerprintSibling.  It is the operator's to delete, once
  // they are sure they will not roll back to the build that wrote it; nothing
  // in this process will reclaim that space for them.
  const fs::path p(volume_path);
  const fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
  const std::string stem = p.stem().string();
  const std::string ext = p.extension().string();
  const std::string base = p.filename().string();
  std::vector<fs::path> to_remove;
  for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (IsFingerprintSibling(name, stem, ext) ||
        IsFingerprintSibling(name, base, ".small")) {
      to_remove.push_back(it->path());
    }
  }
  for (const auto& stale : to_remove) {
    fs::remove(stale, ec);
  }
}

uint64_t PageSpeedCache::ReadGenerationFile(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    return 0;
  }
  uint64_t gen = 0;
  f >> gen;
  return f.fail() ? 0 : gen;
}

bool PageSpeedCache::WriteGenerationFile() {
  std::string gen_path = config_.volume_path + ".gen";

  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%" PRIu64 "\n", generation_);

  // 0644: owner rw, group/other r — nginx (nobody) needs read access.
  bool ok = AtomicWriteFile(gen_path, std::string_view(buf, len), 0644);
  if (!ok && (handler_ != nullptr)) {
    handler_->Warning("Failed to write generation file: %s", gen_path.c_str());
  }
  return ok;
}

}  // namespace pagespeed
