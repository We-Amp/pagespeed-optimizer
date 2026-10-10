// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_PACKS_PACK_H_
#define PAGESPEED_LIB_PACKS_PACK_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "lib/packs/template.h"
#include "re2/re2.h"

namespace pagespeed::packs {

// ---------------------------------------------------------------------------
// Limits. Every bound the loader and the later page pass enforce is named
// here.
// ---------------------------------------------------------------------------
inline constexpr size_t kMaxPackFileBytes = 1024 * 1024;  // 1 MiB
inline constexpr size_t kMaxRules = 200;
inline constexpr size_t kMaxTableEntries = 50000;           // all tables
inline constexpr size_t kMaxTemplateBytes = 16 * 1024;      // 16 KiB
inline constexpr int64_t kRegexMaxMemBytes = 1024 * 1024;   // per regex
inline constexpr size_t kMaxUrlBytes = 2048;                // per value
inline constexpr size_t kMaxTitleChars = 300;               // per value
inline constexpr size_t kMaxDescriptionChars = 1000;        // per value
inline constexpr size_t kMaxJsonLdBytes = 16 * 1024;        // expanded
inline constexpr size_t kMaxAddedBytesPerPage = 64 * 1024;  // per page
inline constexpr size_t kMaxJsonDepth = 32;                 // pack file
inline constexpr size_t kMaxRuleIdLength = 64;
inline constexpr size_t kMaxTableNameLength = 64;
inline constexpr size_t kMaxHostLength = 253;
inline constexpr size_t kMaxGlobBytes = 512;
inline constexpr size_t kMaxRegexBytes = 1024;
inline constexpr int kMaxCaptureRefs = 9;  // {1}..{9}

inline constexpr std::string_view kPackId = "edge-seo";
inline constexpr std::string_view kHreflangTableName = "hreflang";

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

// Report < enforce. The effective mode is the lowest of the global mode, the
// matching site's mode and the rule's own opt-in.
enum class Mode { kReport = 0, kEnforce = 1 };

enum class Kind { kCanonical, kTitle, kDescription, kHreflang, kJsonLd };
inline constexpr int kKindCount = 5;
// Order in which kinds are applied to a page.
inline constexpr Kind kKindOrder[kKindCount] = {Kind::kCanonical, Kind::kTitle,
                                                Kind::kDescription,
                                                Kind::kHreflang, Kind::kJsonLd};

enum class OnPresent { kKeep, kRepair, kReplace };

std::string_view KindName(Kind kind);
std::string_view ModeName(Mode mode);
std::string_view OnPresentName(OnPresent on_present);

// min(global, site, rule). `rule_enforce` false means the rule is report-only.
Mode EffectiveMode(Mode global, Mode site, bool rule_enforce);

struct PackInfo {
  std::string id;
  std::string version;
  std::string publisher;   // informational only
  std::string engine_min;  // empty when absent
  std::string description;
};

struct Site {
  // Exact lowercase host, or the suffix (without "*.") when `wildcard`.
  std::string host;
  bool wildcard = false;
  Mode mode = Mode::kReport;
};

// A compiled path glob: '*' matches within a path segment, '**' crosses
// '/'. Every other character is literal.
struct Glob {
  std::string text;
  std::shared_ptr<const RE2> regex;
};

// Effective path scope of a rule (rule values with defaults applied).
struct RuleMatch {
  std::vector<Glob> paths;                // never empty after loading
  std::vector<Glob> exclude_paths;        // may be empty
  std::string path_regex_text;            // empty when absent
  std::shared_ptr<const RE2> path_regex;  // null when absent
  int path_regex_groups = 0;
};

enum class ValueSource { kTemplate, kTable, kPattern };

struct RuleValue {
  ValueSource source = ValueSource::kTemplate;
  Template tpl;                          // kTemplate
  std::string table;                     // kTable: name of the table
  std::optional<Template> fallback_tpl;  // kTable, optional
  // kPattern (hreflang): normalized code -> href template, sorted by code.
  std::vector<std::pair<std::string, Template>> pattern;
};

struct Rule {
  std::string id;
  Kind kind = Kind::kCanonical;
  bool enabled = true;
  bool enforce = false;
  RuleMatch match;
  RuleValue value;
  OnPresent on_present = OnPresent::kKeep;
};

// A per-URL value table: key is a URL path (with "?query" when the key
// contains one) -> value.
using ValueTable = std::map<std::string, std::string, std::less<>>;

// One hreflang cluster: normalized code -> absolute href, sorted by code.
// `normalized_members` holds NormUrl() of every href, for membership tests.
struct HreflangCluster {
  std::vector<std::pair<std::string, std::string>> entries;
  std::vector<std::string> normalized_members;
};

struct Pack {
  PackInfo info;
  std::vector<Site> sites;
  std::vector<Glob> default_paths;          // "/**" when absent
  std::vector<Glob> default_exclude_paths;  // may be empty
  std::map<std::string, ValueTable, std::less<>> tables;
  std::vector<HreflangCluster> hreflang_clusters;
  std::vector<Rule> rules;
  // Non-fatal findings from the load (currently: statically shadowed rules).
  std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------
// Value checks shared by the loader (table values) and the page pass
// (expanded values).
// ---------------------------------------------------------------------------

// OK when `value` is an absolute http(s) URL with a host, at most
// kMaxUrlBytes long, and free of whitespace and control characters.
absl::Status CheckUrlValue(std::string_view value);

// Number of UTF-8 code points (counts lead bytes; assumes valid UTF-8).
size_t Utf8Length(std::string_view s);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_LIB_PACKS_PACK_H_
