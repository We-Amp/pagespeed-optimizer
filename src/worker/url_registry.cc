// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - In-Memory URL Registry Implementation

#include "src/worker/url_registry.h"

#include <algorithm>
#include <cstdint>

#include "lib/classify/hostname.h"

namespace pagespeed {

UrlRegistry::UrlRegistry(size_t max_entries)
    : max_entries_(std::max(max_entries, size_t{1})) {}

std::string UrlRegistry::MakeKey(std::string_view url,
                                 std::string_view hostname,
                                 std::string_view scheme) {
  std::string key;
  key.reserve(url.size() + 1 + hostname.size() + 1 + scheme.size());
  key.append(url);
  key.push_back('\0');
  key.append(hostname);
  key.push_back('\0');
  key.append(scheme);
  return key;
}

void UrlRegistry::Record(std::string_view url, std::string_view hostname,
                         std::string_view scheme) {
  std::string key = MakeKey(url, hostname, scheme);
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(key);
  if (it != entries_.end()) {
    // Move to front of LRU order (O(1) splice).
    order_.splice(order_.begin(), order_, it->second.order_it);
    return;
  }

  // Evict oldest if at capacity.
  while (order_.size() >= max_entries_) {
    auto& oldest_key = order_.back();
    entries_.erase(oldest_key);
    order_.pop_back();
  }

  order_.push_front(key);
  entries_[key] = MapEntry{
      .entry =
          Entry{std::string(url), std::string(hostname), std::string(scheme)},
      .order_it = order_.begin(),
  };

  known_hostnames_.emplace(NormalizeHostname(hostname));
}

void UrlRegistry::Remove(std::string_view url, std::string_view hostname,
                         std::string_view scheme) {
  std::string key = MakeKey(url, hostname, scheme);
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(key);
  if (it == entries_.end()) return;

  order_.erase(it->second.order_it);
  entries_.erase(it);
}

void UrlRegistry::SetAlternateCount(std::string_view url,
                                    std::string_view hostname,
                                    std::string_view scheme, uint32_t count) {
  std::string key = MakeKey(url, hostname, scheme);
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = entries_.find(key);
  if (it == entries_.end()) return;  // evicted / never recorded: skip.

  it->second.entry.alternate_count = count;
}

std::vector<UrlRegistry::Entry> UrlRegistry::ClearAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Entry> result;
  result.reserve(entries_.size());
  for (const auto& [key, map_entry] : entries_) {
    result.push_back(map_entry.entry);
  }
  entries_.clear();
  order_.clear();
  return result;
}

size_t UrlRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

bool UrlRegistry::HasHostname(std::string_view hostname) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // std::unordered_set has no heterogeneous lookup, so we construct a
  // temporary std::string.  This is cold-path only (management API).
  return known_hostnames_.contains(std::string(hostname));
}

UrlRegistry::Page UrlRegistry::List(size_t offset, size_t limit,
                                    std::string_view hostname) const {
  std::lock_guard<std::mutex> lock(mutex_);

  Page page;

  if (hostname.empty()) {
    // Unfiltered: fast path using positional offset.
    page.total = order_.size();
    if (offset >= order_.size()) {
      page.next_offset = order_.size();
      page.has_more = false;
      return page;
    }

    auto it = order_.begin();
    std::advance(it, offset);

    size_t remaining = order_.size() - offset;
    size_t to_read = std::min(limit, remaining);
    page.entries.reserve(to_read);

    for (size_t count = 0; count < to_read && it != order_.end();
         ++it, ++count) {
      auto map_it = entries_.find(*it);
      if (map_it != entries_.end()) {
        page.entries.push_back(map_it->second.entry);
      }
    }

    page.next_offset = offset + to_read;
    page.has_more = page.next_offset < order_.size();
    return page;
  }

  // Filtered by hostname: scan the full list, count matches, skip to
  // offset, then collect up to limit entries.
  size_t matched = 0;
  page.entries.reserve(std::min(limit, size_t{128}));

  for (const auto& key : order_) {
    auto map_it = entries_.find(key);
    if (map_it == entries_.end()) continue;
    if (map_it->second.entry.hostname != hostname) continue;

    if (matched >= offset && page.entries.size() < limit) {
      page.entries.push_back(map_it->second.entry);
    }
    ++matched;
  }

  page.total = matched;
  page.next_offset = offset + page.entries.size();
  page.has_more = page.next_offset < matched;
  return page;
}

}  // namespace pagespeed
