// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/matcher.h"

#include <algorithm>
#include <cstddef>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "lib/packs/url_norm.h"

namespace pagespeed::packs {

absl::StatusOr<Glob> CompileGlob(std::string_view text) {
  if (text.empty()) return absl::InvalidArgumentError("glob is empty");
  if (text.size() > kMaxGlobBytes) {
    return absl::InvalidArgumentError(
        absl::StrCat("glob is longer than ", kMaxGlobBytes, " bytes"));
  }
  if (text.front() != '/') {
    return absl::InvalidArgumentError("glob must start with '/'");
  }
  std::string pattern = "(?s)";
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '?' || c == '#' || static_cast<unsigned char>(c) <= 0x20 ||
        static_cast<unsigned char>(c) == 0x7f) {
      return absl::InvalidArgumentError(
          absl::StrCat("glob contains the character at offset ", i,
                       " which cannot appear in a URL path"));
    }
    if (c == '*') {
      size_t run = 1;
      while (i + run < text.size() && text[i + run] == '*') ++run;
      if (run > 2) {
        return absl::InvalidArgumentError(
            "glob contains '***'; use '*' or '**'");
      }
      pattern += run == 1 ? "[^/]*" : ".*";
      i += run;
    } else {
      pattern += RE2::QuoteMeta(std::string_view(&text[i], 1));
      ++i;
    }
  }
  RE2::Options options;
  options.set_max_mem(kRegexMaxMemBytes);
  options.set_log_errors(false);
  auto re = std::make_shared<const RE2>(pattern, options);
  if (!re->ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("glob does not compile: ", re->error()));
  }
  Glob glob;
  glob.text = std::string(text);
  glob.regex = std::move(re);
  return glob;
}

bool MatchGlob(const Glob& glob, std::string_view path) {
  return glob.regex != nullptr && RE2::FullMatch(path, *glob.regex);
}

std::string NormalizeHost(std::string_view host) {
  std::string_view h = host;
  if (!h.empty() && h.front() == '[') {
    const size_t close = h.find(']');
    if (close != std::string_view::npos) h = h.substr(0, close + 1);
  } else {
    const size_t colon = h.rfind(':');
    if (colon != std::string_view::npos) h = h.substr(0, colon);
  }
  return absl::AsciiStrToLower(h);
}

bool SiteMatchesHost(const Site& site, std::string_view normalized_host) {
  if (!site.wildcard) return normalized_host == site.host;
  // "*.example.org" matches "a.example.org" and "a.b.example.org" only.
  if (normalized_host.size() <= site.host.size() + 1) return false;
  const size_t split = normalized_host.size() - site.host.size();
  return normalized_host[split - 1] == '.' &&
         normalized_host.substr(split) == site.host;
}

const Site* FindSite(const Pack& pack, std::string_view host) {
  const std::string h = NormalizeHost(host);
  const Site* best = nullptr;
  for (const Site& site : pack.sites) {
    if (!SiteMatchesHost(site, h)) continue;
    if (best == nullptr) {
      best = &site;
    } else if (!site.wildcard && best->wildcard) {
      best = &site;
    } else if (site.wildcard && best->wildcard &&
               site.host.size() > best->host.size()) {
      best = &site;
    }
  }
  return best;
}

std::optional<std::vector<std::string>> MatchRulePath(const Rule& rule,
                                                      std::string_view path) {
  const RuleMatch& m = rule.match;
  bool included = false;
  for (const Glob& g : m.paths) {
    if (MatchGlob(g, path)) {
      included = true;
      break;
    }
  }
  if (!included) return std::nullopt;
  for (const Glob& g : m.exclude_paths) {
    if (MatchGlob(g, path)) return std::nullopt;
  }

  std::vector<std::string> captures;
  if (m.path_regex != nullptr) {
    const int groups = std::min(m.path_regex_groups, kMaxCaptureRefs);
    std::vector<std::string_view> sub(static_cast<size_t>(groups) + 1);
    if (!m.path_regex->Match(path, 0, path.size(), RE2::ANCHOR_BOTH, sub.data(),
                             static_cast<int>(sub.size()))) {
      return std::nullopt;
    }
    for (int i = 1; i <= groups; ++i) {
      captures.emplace_back(sub[static_cast<size_t>(i)]);
    }
  }
  return captures;
}

Selection SelectRules(const Pack& pack, std::string_view host,
                      std::string_view path) {
  Selection sel;
  sel.site = FindSite(pack, host);
  if (sel.site == nullptr) return sel;
  for (const Rule& rule : pack.rules) {
    if (!rule.enabled) continue;
    auto captures = MatchRulePath(rule, path);
    if (!captures.has_value()) continue;
    auto& slot = sel.by_kind[static_cast<size_t>(rule.kind)];
    if (slot.has_value()) {
      sel.shadowed.push_back(&rule);
    } else {
      slot = RuleHit{&rule, *std::move(captures)};
    }
  }
  return sel;
}

const std::string* LookupTableValue(const ValueTable& table,
                                    std::string_view path,
                                    std::string_view query) {
  if (!query.empty()) {
    auto it = table.find(absl::StrCat(path, "?", query));
    if (it != table.end()) return &it->second;
  }
  auto it = table.find(path);
  return it == table.end() ? nullptr : &it->second;
}

const HreflangCluster* FindCluster(const Pack& pack,
                                   std::string_view page_url) {
  const auto norm = NormUrl(page_url);
  if (!norm.has_value()) return nullptr;
  for (const HreflangCluster& cluster : pack.hreflang_clusters) {
    if (std::find(cluster.normalized_members.begin(),
                  cluster.normalized_members.end(),
                  *norm) != cluster.normalized_members.end()) {
      return &cluster;
    }
  }
  return nullptr;
}

}  // namespace pagespeed::packs
