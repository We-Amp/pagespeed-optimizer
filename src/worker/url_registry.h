// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - In-Memory URL Registry
//
// Tracks all cached URLs for enumeration via the management API.
// Cyclone's cache is content-addressable (SHA-256 keys) and does not
// support URL iteration, so we maintain a bounded in-memory index.
//
// Thread-safe: accessed from worker threads (on write) and the event
// loop (on read/list).

#ifndef PAGESPEED_SRC_WORKER_URL_REGISTRY_H_
#define PAGESPEED_SRC_WORKER_URL_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pagespeed {

// Bounded URL registry with LRU eviction.
//
// Record() is called on every WriteAlternate.  When the registry
// exceeds max_entries, the least-recently recorded URL is evicted.
// Uses std::list + iterator map for O(1) Record/Remove operations.
class UrlRegistry {
 public:
  explicit UrlRegistry(size_t max_entries = 500000);

  // Record a URL+hostname+scheme as cached.  Moves to front if already present.
  void Record(std::string_view url, std::string_view hostname,
              std::string_view scheme);

  // Remove a URL+hostname+scheme (e.g., on purge).
  void Remove(std::string_view url, std::string_view hostname,
              std::string_view scheme);

  // Update the cached alternate count for an already-recorded URL.
  // No-op when the URL is not currently tracked (e.g., LRU-evicted between
  // Record() and this call) — never re-inserts.  Thread-safe.
  //
  // The count is maintained by the worker on the notification (worker-pool)
  // thread so the /v1/cache/urls listing can report it without doing a
  // per-row cache chain traversal on the event-loop thread.
  void SetAlternateCount(std::string_view url, std::string_view hostname,
                         std::string_view scheme, uint32_t count);

  // Current number of tracked URLs.
  size_t size() const;

  // Check whether any URL has ever been recorded for the given hostname.
  // The set of known hostnames is never evicted, so this remains accurate
  // even after LRU eviction purges all entries for a hostname.
  bool HasHostname(std::string_view hostname) const;

  struct Entry {
    std::string url;
    std::string hostname;
    std::string scheme;
    // Number of unique cached alternates for this URL, refreshed by the
    // worker on each notification (and on async agent-markdown writes).
    // Cached here so listing avoids a per-row cache traversal.
    uint32_t alternate_count = 0;
  };

  // Remove all entries atomically.  Returns the entries that were removed.
  std::vector<Entry> ClearAll();

  struct Page {
    std::vector<Entry> entries;
    size_t next_offset;
    bool has_more;
    size_t total;
  };

  // List URLs with offset-based pagination.
  // Returns up to `limit` entries starting at `offset`.
  // When `hostname` is non-empty, only entries matching that hostname
  // are included (offset/total reflect the filtered set).
  Page List(size_t offset, size_t limit, std::string_view hostname = {}) const;

 private:
  // Composite key for the map: "url\0hostname\0scheme"
  static std::string MakeKey(std::string_view url, std::string_view hostname,
                             std::string_view scheme);

  mutable std::mutex mutex_;
  size_t max_entries_;
  // Ordered from newest (front) to oldest (back).
  std::list<std::string> order_;

  struct MapEntry {
    Entry entry;
    std::list<std::string>::iterator order_it;
  };
  std::unordered_map<std::string, MapEntry> entries_;

  // Unbounded set of all hostnames ever seen via Record().
  // Intentionally never pruned on LRU eviction or ClearAll() — the
  // cardinality is tiny (number of distinct vhosts) and a hostname
  // should remain valid for purge/reprocess even after all its entries
  // have been evicted.  Stores normalized forms (lowercase, no trailing
  // dot, default ports stripped).
  std::unordered_set<std::string> known_hostnames_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_URL_REGISTRY_H_
