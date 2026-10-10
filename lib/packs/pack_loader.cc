// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/pack_loader.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "lib/packs/matcher.h"
#include "lib/packs/url_norm.h"
#include "nlohmann/json.hpp"
#include "re2/re2.h"

namespace pagespeed::packs {
namespace {

using Json = nlohmann::json;

#define PACKS_RETURN_IF_ERROR(expr)                \
  do {                                             \
    absl::Status packs_status_ = (expr);           \
    if (!packs_status_.ok()) return packs_status_; \
  } while (0)

std::string Child(const std::string& path, std::string_view key) {
  return path.empty() ? std::string(key) : absl::StrCat(path, ".", key);
}

std::string Index(const std::string& path, size_t i) {
  return absl::StrCat(path, "[", i, "]");
}

const RE2& SemverRe() {
  static const RE2* re = new RE2(
      R"(^(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8}))"
      R"((-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$)");
  return *re;
}

const RE2& LeadingVersionRe() {
  static const RE2* re =
      new RE2(R"(^([0-9]{1,9})\.([0-9]{1,9})\.([0-9]{1,9}))");
  return *re;
}

const RE2& RuleIdRe() {
  static const RE2* re = new RE2("^[a-z0-9-]{1,64}$");
  return *re;
}

const RE2& TableNameRe() {
  static const RE2* re = new RE2("^[a-z0-9_-]{1,64}$");
  return *re;
}

const RE2& HostRe() {
  static const RE2* re = new RE2(
      "^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?"
      "(\\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*$");
  return *re;
}

using Version = std::tuple<int, int, int>;

bool ParseLeadingVersion(std::string_view s, Version* out) {
  std::string a, b, c;
  if (!RE2::PartialMatch(s, LeadingVersionRe(), &a, &b, &c)) return false;
  int x = 0, y = 0, z = 0;
  if (!absl::SimpleAtoi(a, &x) || !absl::SimpleAtoi(b, &y) ||
      !absl::SimpleAtoi(c, &z)) {
    return false;
  }
  *out = {x, y, z};
  return true;
}

class Loader {
 public:
  Loader(std::string_view engine_version, std::string_view source_name)
      : engine_version_(engine_version), file_(source_name) {}

  absl::StatusOr<Pack> Load(std::string_view text);

 private:
  absl::Status Fail(const std::string& path, std::string_view reason) const {
    return absl::InvalidArgumentError(
        absl::StrCat(file_.empty() ? "" : file_ + ": ",
                     path.empty() ? "$" : path, ": ", reason));
  }

  // Object helpers. Each returns an error with the JSON path of the problem.
  absl::Status ExpectObject(const Json& j, const std::string& path) const {
    if (!j.is_object()) return Fail(path, "must be an object");
    return absl::OkStatus();
  }
  absl::Status CheckKeys(
      const Json& obj, const std::string& path,
      std::initializer_list<std::string_view> allowed,
      std::initializer_list<std::string_view> required) const;
  absl::Status GetString(const Json& obj, const std::string& path,
                         std::string_view key, std::string* out) const;
  absl::Status GetBool(const Json& obj, const std::string& path,
                       std::string_view key, bool* out) const;
  absl::Status GetGlobs(const Json& obj, const std::string& path,
                        std::string_view key, std::vector<Glob>* out) const;

  absl::Status LoadInfo(const Json& j);
  absl::Status LoadSites(const Json& j);
  absl::Status LoadDefaults(const Json& j);
  absl::Status LoadTables(const Json& j);
  absl::Status LoadHreflangTable(const Json& j, const std::string& path);
  absl::Status LoadValueTable(const Json& j, const std::string& path,
                              const std::string& name);
  absl::Status LoadRules(const Json& j);
  absl::Status LoadRule(const Json& j, const std::string& path, Rule* rule);
  absl::Status LoadMatch(const Json& j, const std::string& path,
                         RuleMatch* match);
  absl::Status LoadValue(const Json& j, const std::string& path, Rule* rule);
  absl::Status CheckTableValues(const std::string& path,
                                const std::string& table_name, Kind kind);
  absl::Status ParseTpl(const std::string& path, const std::string& text,
                        const Rule& rule, Template* out) const;
  void LintShadowing();

  std::string engine_version_;
  std::string file_;
  Pack pack_;
  size_t table_entries_ = 0;
  std::set<std::pair<std::string, int>> checked_tables_;
};

absl::Status Loader::CheckKeys(
    const Json& obj, const std::string& path,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> required) const {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    const std::string& key = it.key();
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      return Fail(path, absl::StrCat("unknown key \"", key, "\""));
    }
  }
  for (std::string_view key : required) {
    if (!obj.contains(std::string(key))) {
      return Fail(path, absl::StrCat("missing required key \"", key, "\""));
    }
  }
  return absl::OkStatus();
}

absl::Status Loader::GetString(const Json& obj, const std::string& path,
                               std::string_view key, std::string* out) const {
  auto it = obj.find(std::string(key));
  if (it == obj.end()) return absl::OkStatus();
  if (!it->is_string()) return Fail(Child(path, key), "must be a string");
  *out = it->get<std::string>();
  return absl::OkStatus();
}

absl::Status Loader::GetBool(const Json& obj, const std::string& path,
                             std::string_view key, bool* out) const {
  auto it = obj.find(std::string(key));
  if (it == obj.end()) return absl::OkStatus();
  if (!it->is_boolean()) return Fail(Child(path, key), "must be true or false");
  *out = it->get<bool>();
  return absl::OkStatus();
}

absl::Status Loader::GetGlobs(const Json& obj, const std::string& path,
                              std::string_view key,
                              std::vector<Glob>* out) const {
  auto it = obj.find(std::string(key));
  if (it == obj.end()) return absl::OkStatus();
  const std::string kpath = Child(path, key);
  if (!it->is_array()) return Fail(kpath, "must be an array of path globs");
  if (it->empty()) return Fail(kpath, "must not be empty");
  out->clear();
  for (size_t i = 0; i < it->size(); ++i) {
    const Json& e = (*it)[i];
    if (!e.is_string()) return Fail(Index(kpath, i), "must be a string");
    auto glob = CompileGlob(e.get<std::string>());
    if (!glob.ok()) return Fail(Index(kpath, i), glob.status().message());
    out->push_back(*std::move(glob));
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadInfo(const Json& j) {
  const std::string path = "pack";
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(CheckKeys(
      j, path, {"id", "version", "publisher", "engine_min", "description"},
      {"id", "version"}));
  PackInfo& info = pack_.info;
  info.publisher = "local";
  PACKS_RETURN_IF_ERROR(GetString(j, path, "id", &info.id));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "version", &info.version));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "publisher", &info.publisher));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "engine_min", &info.engine_min));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "description", &info.description));

  if (info.id != kPackId) {
    return Fail("pack.id", absl::StrCat("must be \"", kPackId, "\""));
  }
  if (!RE2::FullMatch(info.version, SemverRe())) {
    return Fail("pack.version",
                "must be a semantic version (MAJOR.MINOR.PATCH)");
  }
  if (j.contains("engine_min")) {
    if (!RE2::FullMatch(info.engine_min, SemverRe())) {
      return Fail("pack.engine_min",
                  "must be a semantic version (MAJOR.MINOR.PATCH)");
    }
    if (!engine_version_.empty()) {
      Version have{0, 0, 0};
      Version need{0, 0, 0};
      if (!ParseLeadingVersion(engine_version_, &have)) {
        return Fail("pack.engine_min",
                    absl::StrCat("cannot compare against engine version \"",
                                 engine_version_, "\""));
      }
      ParseLeadingVersion(info.engine_min, &need);
      if (need > have) {
        return Fail(
            "pack.engine_min",
            absl::StrCat("requires engine ", info.engine_min,
                         " or newer; this engine is ", engine_version_));
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadSites(const Json& j) {
  const std::string path = "sites";
  if (!j.is_array()) return Fail(path, "must be an array");
  if (j.empty()) return Fail(path, "must list at least one site");
  std::set<std::string> seen;
  for (size_t i = 0; i < j.size(); ++i) {
    const std::string p = Index(path, i);
    const Json& e = j[i];
    PACKS_RETURN_IF_ERROR(ExpectObject(e, p));
    PACKS_RETURN_IF_ERROR(CheckKeys(e, p, {"host", "mode"}, {"host"}));
    std::string host, mode = "report";
    PACKS_RETURN_IF_ERROR(GetString(e, p, "host", &host));
    PACKS_RETURN_IF_ERROR(GetString(e, p, "mode", &mode));
    Site site;
    std::string_view h = host;
    if (h.substr(0, 2) == "*.") {
      site.wildcard = true;
      h.remove_prefix(2);
    }
    if (h.empty() || h.size() > kMaxHostLength ||
        !RE2::FullMatch(h, HostRe())) {
      return Fail(Child(p, "host"),
                  "must be a lowercase host name, or \"*.\" followed by one "
                  "(no port, no other wildcard)");
    }
    site.host = std::string(h);
    if (mode == "report") {
      site.mode = Mode::kReport;
    } else if (mode == "enforce") {
      site.mode = Mode::kEnforce;
    } else {
      return Fail(Child(p, "mode"), "must be \"report\" or \"enforce\"");
    }
    if (!seen.insert(host).second) {
      return Fail(Child(p, "host"),
                  absl::StrCat("duplicate site \"", host, "\""));
    }
    pack_.sites.push_back(std::move(site));
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadDefaults(const Json& j) {
  const std::string path = "defaults";
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(CheckKeys(j, path, {"paths", "exclude_paths"}, {}));
  PACKS_RETURN_IF_ERROR(GetGlobs(j, path, "paths", &pack_.default_paths));
  PACKS_RETURN_IF_ERROR(
      GetGlobs(j, path, "exclude_paths", &pack_.default_exclude_paths));
  return absl::OkStatus();
}

absl::Status Loader::LoadHreflangTable(const Json& j, const std::string& path) {
  if (!j.is_array()) {
    return Fail(path, "the hreflang table must be an array of clusters");
  }
  for (size_t i = 0; i < j.size(); ++i) {
    const std::string cp = Index(path, i);
    const Json& cluster = j[i];
    PACKS_RETURN_IF_ERROR(ExpectObject(cluster, cp));
    if (cluster.empty()) return Fail(cp, "cluster must not be empty");
    HreflangCluster out;
    std::set<std::string> codes;
    for (auto it = cluster.begin(); it != cluster.end(); ++it) {
      const std::string ep = Child(cp, it.key());
      if (++table_entries_ > kMaxTableEntries) {
        return Fail("tables", absl::StrCat("holds more than ", kMaxTableEntries,
                                           " entries in total"));
      }
      auto code = NormalizeHreflangCode(it.key());
      if (!code.has_value()) {
        return Fail(ep, "not a valid hreflang code");
      }
      if (!codes.insert(*code).second) {
        return Fail(ep, absl::StrCat("duplicate hreflang code \"", *code,
                                     "\" in cluster"));
      }
      if (!it->is_string()) return Fail(ep, "must be a string");
      const std::string href = it->get<std::string>();
      absl::Status st = CheckUrlValue(href);
      if (!st.ok()) return Fail(ep, st.message());
      out.entries.emplace_back(*code, href);
    }
    // JSON objects are unordered; sort so the emitted order is deterministic.
    std::sort(out.entries.begin(), out.entries.end());
    out.normalized_members.clear();
    for (const auto& entry : out.entries) {
      out.normalized_members.push_back(*NormUrl(entry.second));
    }
    pack_.hreflang_clusters.push_back(std::move(out));
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadValueTable(const Json& j, const std::string& path,
                                    const std::string& name) {
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  ValueTable table;
  for (auto it = j.begin(); it != j.end(); ++it) {
    const std::string& key = it.key();
    const std::string ep = absl::StrCat(path, "[\"", key, "\"]");
    if (++table_entries_ > kMaxTableEntries) {
      return Fail("tables", absl::StrCat("holds more than ", kMaxTableEntries,
                                         " entries in total"));
    }
    if (key.empty() || key.front() != '/' || key.size() > kMaxUrlBytes ||
        key.find('#') != std::string::npos ||
        std::any_of(key.begin(), key.end(), [](char c) {
          return static_cast<unsigned char>(c) <= 0x20 ||
                 static_cast<unsigned char>(c) == 0x7f;
        })) {
      return Fail(path, absl::StrCat("key \"", key,
                                     "\" must be a URL path starting with '/' "
                                     "(optionally with ?query), without "
                                     "whitespace or '#'"));
    }
    if (!it->is_string()) return Fail(ep, "must be a string");
    const std::string& value = it->get_ref<const std::string&>();
    if (value.size() > kMaxTemplateBytes) {
      return Fail(ep, absl::StrCat("value is longer than ", kMaxTemplateBytes,
                                   " bytes"));
    }
    table.emplace(key, value);
  }
  pack_.tables.emplace(name, std::move(table));
  return absl::OkStatus();
}

absl::Status Loader::LoadTables(const Json& j) {
  const std::string path = "tables";
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  for (auto it = j.begin(); it != j.end(); ++it) {
    const std::string& name = it.key();
    const std::string tp = Child(path, name);
    if (!RE2::FullMatch(name, TableNameRe())) {
      return Fail(path, absl::StrCat("table name \"", name,
                                     "\" must match [a-z0-9_-]{1,64}"));
    }
    if (name == kHreflangTableName) {
      PACKS_RETURN_IF_ERROR(LoadHreflangTable(*it, tp));
    } else {
      PACKS_RETURN_IF_ERROR(LoadValueTable(*it, tp, name));
    }
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadMatch(const Json& j, const std::string& path,
                               RuleMatch* match) {
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(
      CheckKeys(j, path, {"paths", "exclude_paths", "path_regex"}, {}));
  PACKS_RETURN_IF_ERROR(GetGlobs(j, path, "paths", &match->paths));
  PACKS_RETURN_IF_ERROR(
      GetGlobs(j, path, "exclude_paths", &match->exclude_paths));
  if (j.contains("path_regex")) {
    const std::string rp = Child(path, "path_regex");
    std::string text;
    PACKS_RETURN_IF_ERROR(GetString(j, path, "path_regex", &text));
    if (text.empty()) return Fail(rp, "must not be empty");
    if (text.size() > kMaxRegexBytes) {
      return Fail(rp,
                  absl::StrCat("is longer than ", kMaxRegexBytes, " bytes"));
    }
    RE2::Options options;
    options.set_max_mem(kRegexMaxMemBytes);
    options.set_log_errors(false);
    auto re = std::make_shared<const RE2>(text, options);
    if (!re->ok()) {
      return Fail(rp, absl::StrCat("invalid RE2 pattern: ", re->error()));
    }
    match->path_regex_text = std::move(text);
    match->path_regex_groups = re->NumberOfCapturingGroups();
    match->path_regex = std::move(re);
  }
  return absl::OkStatus();
}

absl::Status Loader::ParseTpl(const std::string& path, const std::string& text,
                              const Rule& rule, Template* out) const {
  if (text.empty()) return Fail(path, "template must not be empty");
  if (text.size() > kMaxTemplateBytes) {
    return Fail(path, absl::StrCat("template is longer than ",
                                   kMaxTemplateBytes, " bytes"));
  }
  TemplateParseOptions options;
  options.allow_page_values = rule.kind == Kind::kJsonLd;
  options.capture_groups = rule.match.path_regex_groups;
  auto tpl = ParseTemplate(text, options);
  if (!tpl.ok()) return Fail(path, tpl.status().message());
  *out = *std::move(tpl);
  return absl::OkStatus();
}

absl::Status Loader::CheckTableValues(const std::string& path,
                                      const std::string& table_name,
                                      Kind kind) {
  if (!checked_tables_.emplace(table_name, static_cast<int>(kind)).second) {
    return absl::OkStatus();
  }
  const ValueTable& table = pack_.tables.at(table_name);
  for (const auto& [key, value] : table) {
    const std::string ep =
        absl::StrCat("tables.", table_name, "[\"", key, "\"]");
    switch (kind) {
      case Kind::kCanonical: {
        absl::Status st = CheckUrlValue(value);
        if (!st.ok()) {
          return Fail(ep, absl::StrCat(st.message(), " (used by ", path, ")"));
        }
        break;
      }
      case Kind::kTitle:
        if (value.empty() || Utf8Length(value) > kMaxTitleChars) {
          return Fail(ep, absl::StrCat("a title must be 1 to ", kMaxTitleChars,
                                       " characters (used by ", path, ")"));
        }
        break;
      case Kind::kDescription:
        if (value.empty() || Utf8Length(value) > kMaxDescriptionChars) {
          return Fail(ep, absl::StrCat("a description must be 1 to ",
                                       kMaxDescriptionChars,
                                       " characters (used by ", path, ")"));
        }
        break;
      case Kind::kHreflang:
      case Kind::kJsonLd:
        break;
    }
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadValue(const Json& j, const std::string& path,
                               Rule* rule) {
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(CheckKeys(
      j, path, {"template", "table", "fallback_template", "pattern"}, {}));
  const int sources = static_cast<int>(j.contains("template")) +
                      static_cast<int>(j.contains("table")) +
                      static_cast<int>(j.contains("pattern"));
  if (sources != 1) {
    return Fail(path,
                "must have exactly one of \"template\", \"table\" or "
                "\"pattern\"");
  }
  if (j.contains("fallback_template") && !j.contains("table")) {
    return Fail(Child(path, "fallback_template"),
                "is only allowed together with \"table\"");
  }
  RuleValue& value = rule->value;
  const Kind kind = rule->kind;

  if (j.contains("template")) {
    if (kind == Kind::kHreflang) {
      return Fail(Child(path, "template"),
                  "hreflang rules take \"table\" or \"pattern\", not a "
                  "template");
    }
    std::string text;
    PACKS_RETURN_IF_ERROR(GetString(j, path, "template", &text));
    value.source = ValueSource::kTemplate;
    return ParseTpl(Child(path, "template"), text, *rule, &value.tpl);
  }

  if (j.contains("pattern")) {
    const std::string pp = Child(path, "pattern");
    if (kind != Kind::kHreflang) {
      return Fail(pp, "\"pattern\" is only allowed for hreflang rules");
    }
    const Json& pat = j["pattern"];
    PACKS_RETURN_IF_ERROR(ExpectObject(pat, pp));
    if (pat.empty()) return Fail(pp, "must not be empty");
    value.source = ValueSource::kPattern;
    std::set<std::string> codes;
    for (auto it = pat.begin(); it != pat.end(); ++it) {
      const std::string ep = Child(pp, it.key());
      auto code = NormalizeHreflangCode(it.key());
      if (!code.has_value()) return Fail(ep, "not a valid hreflang code");
      if (!codes.insert(*code).second) {
        return Fail(ep,
                    absl::StrCat("duplicate hreflang code \"", *code, "\""));
      }
      if (!it->is_string()) return Fail(ep, "must be a string");
      Template tpl;
      PACKS_RETURN_IF_ERROR(ParseTpl(ep, it->get<std::string>(), *rule, &tpl));
      value.pattern.emplace_back(*code, std::move(tpl));
    }
    // JSON objects are unordered; sort so the emitted order is deterministic.
    std::sort(value.pattern.begin(), value.pattern.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return absl::OkStatus();
  }

  // "table"
  const std::string tp = Child(path, "table");
  PACKS_RETURN_IF_ERROR(GetString(j, path, "table", &value.table));
  value.source = ValueSource::kTable;
  if (kind == Kind::kJsonLd) {
    return Fail(tp, "jsonld rules take a \"template\"");
  }
  if (kind == Kind::kHreflang) {
    if (value.table != kHreflangTableName) {
      return Fail(tp, "hreflang rules can only use the table \"hreflang\"");
    }
    if (j.contains("fallback_template")) {
      return Fail(Child(path, "fallback_template"),
                  "is not allowed for the hreflang table");
    }
    return absl::OkStatus();
  }
  if (value.table == kHreflangTableName) {
    return Fail(tp, "the table \"hreflang\" is only for hreflang rules");
  }
  if (pack_.tables.find(value.table) == pack_.tables.end()) {
    return Fail(tp, absl::StrCat("unknown table \"", value.table, "\""));
  }
  PACKS_RETURN_IF_ERROR(CheckTableValues(path, value.table, kind));
  if (j.contains("fallback_template")) {
    std::string text;
    PACKS_RETURN_IF_ERROR(GetString(j, path, "fallback_template", &text));
    Template tpl;
    PACKS_RETURN_IF_ERROR(
        ParseTpl(Child(path, "fallback_template"), text, *rule, &tpl));
    value.fallback_tpl = std::move(tpl);
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadRule(const Json& j, const std::string& path,
                              Rule* rule) {
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(CheckKeys(
      j, path,
      {"id", "kind", "enabled", "enforce", "match", "value", "on_present"},
      {"id", "kind", "value"}));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "id", &rule->id));
  if (!RE2::FullMatch(rule->id, RuleIdRe())) {
    return Fail(Child(path, "id"), "must match [a-z0-9-]{1,64}");
  }

  std::string kind;
  PACKS_RETURN_IF_ERROR(GetString(j, path, "kind", &kind));
  if (kind == "canonical") {
    rule->kind = Kind::kCanonical;
  } else if (kind == "title") {
    rule->kind = Kind::kTitle;
  } else if (kind == "description") {
    rule->kind = Kind::kDescription;
  } else if (kind == "hreflang") {
    rule->kind = Kind::kHreflang;
  } else if (kind == "jsonld") {
    rule->kind = Kind::kJsonLd;
  } else {
    return Fail(Child(path, "kind"),
                "must be one of canonical, title, description, hreflang, "
                "jsonld");
  }

  PACKS_RETURN_IF_ERROR(GetBool(j, path, "enabled", &rule->enabled));
  PACKS_RETURN_IF_ERROR(GetBool(j, path, "enforce", &rule->enforce));

  std::string on_present = "keep";
  PACKS_RETURN_IF_ERROR(GetString(j, path, "on_present", &on_present));
  if (on_present == "keep") {
    rule->on_present = OnPresent::kKeep;
  } else if (on_present == "repair") {
    rule->on_present = OnPresent::kRepair;
  } else if (on_present == "replace") {
    rule->on_present = OnPresent::kReplace;
  } else {
    return Fail(Child(path, "on_present"),
                "must be \"keep\", \"repair\" or \"replace\"");
  }

  if (j.contains("match")) {
    PACKS_RETURN_IF_ERROR(
        LoadMatch(j["match"], Child(path, "match"), &rule->match));
  }
  if (rule->match.paths.empty()) rule->match.paths = pack_.default_paths;
  if (rule->match.exclude_paths.empty()) {
    rule->match.exclude_paths = pack_.default_exclude_paths;
  }
  return LoadValue(j["value"], Child(path, "value"), rule);
}

absl::Status Loader::LoadRules(const Json& j) {
  const std::string path = "rules";
  if (!j.is_array()) return Fail(path, "must be an array");
  if (j.size() > kMaxRules) {
    return Fail(path, absl::StrCat("has ", j.size(), " rules; the limit is ",
                                   kMaxRules));
  }
  std::unordered_set<std::string> ids;
  for (size_t i = 0; i < j.size(); ++i) {
    Rule rule;
    const std::string p = Index(path, i);
    PACKS_RETURN_IF_ERROR(LoadRule(j[i], p, &rule));
    if (!ids.insert(rule.id).second) {
      return Fail(Child(p, "id"),
                  absl::StrCat("duplicate rule id \"", rule.id, "\""));
    }
    pack_.rules.push_back(std::move(rule));
  }
  return absl::OkStatus();
}

// True when every path matched by `inner` is certainly matched by `outer`.
bool GlobCovers(const Glob& outer, const Glob& inner) {
  if (outer.text == inner.text) return true;
  constexpr std::string_view kTail = "**";
  if (outer.text.size() >= kTail.size() &&
      outer.text.compare(outer.text.size() - kTail.size(), kTail.size(),
                         kTail) == 0) {
    const std::string_view prefix(outer.text.data(),
                                  outer.text.size() - kTail.size());
    return inner.text.size() >= prefix.size() &&
           std::string_view(inner.text).substr(0, prefix.size()) == prefix;
  }
  return false;
}

void Loader::LintShadowing() {
  for (size_t b = 0; b < pack_.rules.size(); ++b) {
    const Rule& later = pack_.rules[b];
    if (!later.enabled) continue;
    for (size_t a = 0; a < b; ++a) {
      const Rule& earlier = pack_.rules[a];
      if (!earlier.enabled || earlier.kind != later.kind) continue;
      if (earlier.match.path_regex != nullptr ||
          !earlier.match.exclude_paths.empty()) {
        continue;
      }
      const bool covered = std::all_of(
          later.match.paths.begin(), later.match.paths.end(),
          [&](const Glob& g) {
            return std::any_of(earlier.match.paths.begin(),
                               earlier.match.paths.end(),
                               [&](const Glob& o) { return GlobCovers(o, g); });
          });
      if (covered) {
        pack_.warnings.push_back(
            absl::StrCat("rule \"", later.id, "\" (", KindName(later.kind),
                         ") can never apply: rule \"", earlier.id,
                         "\" comes first and matches every path it matches"));
        break;
      }
    }
  }
}

absl::StatusOr<Pack> Loader::Load(std::string_view text) {
  if (text.size() > kMaxPackFileBytes) {
    return Fail("", absl::StrCat("file is ", text.size(),
                                 " bytes; the limit is ", kMaxPackFileBytes));
  }

  // Duplicate keys are a load error (nlohmann would silently keep the last),
  // as is excessive nesting.
  std::vector<std::set<std::string>> key_stack;
  std::string duplicate_key;
  bool too_deep = false;
  Json::parser_callback_t cb = [&](int depth, Json::parse_event_t event,
                                   Json& parsed) {
    switch (event) {
      case Json::parse_event_t::object_start:
      case Json::parse_event_t::array_start:
        key_stack.emplace_back();
        if (static_cast<size_t>(depth) > kMaxJsonDepth) too_deep = true;
        break;
      case Json::parse_event_t::object_end:
      case Json::parse_event_t::array_end:
        if (!key_stack.empty()) key_stack.pop_back();
        break;
      case Json::parse_event_t::key:
        if (!key_stack.empty() &&
            !key_stack.back().insert(parsed.get<std::string>()).second &&
            duplicate_key.empty()) {
          duplicate_key = parsed.get<std::string>();
        }
        break;
      case Json::parse_event_t::value:
        break;
    }
    return true;
  };

  Json root;
  try {
    root = Json::parse(text.begin(), text.end(), cb, true, false);
  } catch (const Json::exception& e) {
    return Fail("", absl::StrCat("malformed JSON: ", e.what()));
  }
  if (!duplicate_key.empty()) {
    return Fail("", absl::StrCat("duplicate key \"", duplicate_key, "\""));
  }
  if (too_deep) {
    return Fail("",
                absl::StrCat("nested deeper than ", kMaxJsonDepth, " levels"));
  }

  PACKS_RETURN_IF_ERROR(ExpectObject(root, ""));
  PACKS_RETURN_IF_ERROR(
      CheckKeys(root, "", {"pack", "sites", "defaults", "tables", "rules"},
                {"pack", "sites", "rules"}));

  PACKS_RETURN_IF_ERROR(LoadInfo(root["pack"]));
  PACKS_RETURN_IF_ERROR(LoadSites(root["sites"]));
  if (root.contains("defaults")) {
    PACKS_RETURN_IF_ERROR(LoadDefaults(root["defaults"]));
  }
  if (pack_.default_paths.empty()) {
    auto all = CompileGlob("/**");
    if (!all.ok()) return all.status();
    pack_.default_paths.push_back(*std::move(all));
  }
  if (root.contains("tables")) {
    PACKS_RETURN_IF_ERROR(LoadTables(root["tables"]));
  }
  PACKS_RETURN_IF_ERROR(LoadRules(root["rules"]));
  LintShadowing();
  return std::move(pack_);
}

}  // namespace

absl::StatusOr<Pack> LoadPack(std::string_view json,
                              std::string_view engine_version,
                              std::string_view source_name) {
  Loader loader(engine_version, source_name);
  return loader.Load(json);
}

absl::StatusOr<Pack> LoadPackFile(const std::string& path,
                                  std::string_view engine_version) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return absl::NotFoundError(
        absl::StrCat(path, ": cannot read pack file: ", ec.message()));
  }
  if (size > kMaxPackFileBytes) {
    return absl::InvalidArgumentError(absl::StrCat(path, ": $: file is ", size,
                                                   " bytes; the limit is ",
                                                   kMaxPackFileBytes));
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(absl::StrCat(path, ": cannot open pack file"));
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return LoadPack(buf.str(), engine_version, path);
}

}  // namespace pagespeed::packs
