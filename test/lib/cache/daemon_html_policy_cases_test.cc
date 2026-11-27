// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Schema check for the shared HTML cache policy cases
// (test/lib/cache/daemon_html_policy_cases.json). It proves the file parses,
// that every row has the documented shape with values from the closed sets,
// that row labels stay public-safe, that the Accept rows match the shared
// vector file line for line, and that the file matches its hash pin. It runs
// no policy: the policy runner asserts the rows against the library.
//
// The C API 1.15 surface is built, so its keys, calls and values are part of
// the closed sets every row is checked against, and no row may be pending: a
// `pending` marker (the value kPendingSurface once named the 1.15 surface
// before the library had it) would hide a row from the runner.

#include "test/lib/cache/daemon_html_policy_cases.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pagespeed {
namespace test {
namespace {

using nlohmann::json;
using StringSet = std::set<std::string>;

constexpr char kVectorPath[] =
    "test/lib/classify/testdata/accept_markdown_vectors.txt";
constexpr int kVectorCount = 42;

json LoadCases() {
  const std::string contents = ReadDaemonHtmlPolicyCases();
  EXPECT_FALSE(contents.empty()) << kDaemonHtmlPolicyCasesPath;
  return json::parse(contents, /*cb=*/nullptr, /*allow_exceptions=*/false);
}

// Every problem is collected with the row id, so one run lists them all.
class Checker {
 public:
  void Fail(const std::string& where, const std::string& what) {
    errors_.push_back(where + ": " + what);
  }
  const std::vector<std::string>& errors() const { return errors_; }

  void Keys(const std::string& where, const json& obj, const StringSet& allowed,
            const StringSet& required) {
    if (!obj.is_object()) {
      Fail(where, "not an object");
      return;
    }
    for (const auto& [key, value] : obj.items()) {
      if (allowed.count(key) == 0) Fail(where, "unknown key '" + key + "'");
    }
    for (const std::string& key : required) {
      if (!obj.contains(key)) Fail(where, "missing key '" + key + "'");
    }
  }
  void Bool(const std::string& where, const json& obj, const char* key) {
    if (obj.contains(key) && !obj[key].is_boolean()) {
      Fail(where, std::string(key) + " is not a boolean");
    }
  }
  void Uint(const std::string& where, const json& obj, const char* key) {
    if (obj.contains(key) && !obj[key].is_number_unsigned()) {
      Fail(where, std::string(key) + " is not a non-negative integer");
    }
  }
  void Str(const std::string& where, const json& obj, const char* key) {
    if (obj.contains(key) && !obj[key].is_string()) {
      Fail(where, std::string(key) + " is not a string");
    }
  }
  void OneOf(const std::string& where, const json& obj, const char* key,
             const StringSet& values) {
    if (!obj.contains(key)) return;
    if (!obj[key].is_string() ||
        values.count(obj[key].get<std::string>()) == 0) {
      Fail(where, std::string(key) + " is not one of the allowed values");
    }
  }
  void SubsetOf(const std::string& where, const json& obj, const char* key,
                const StringSet& values) {
    if (!obj.contains(key)) return;
    if (!obj[key].is_array()) {
      Fail(where, std::string(key) + " is not an array");
      return;
    }
    StringSet seen;
    for (const json& v : obj[key]) {
      if (!v.is_string() || values.count(v.get<std::string>()) == 0) {
        Fail(where, std::string(key) + " has a value outside its set");
      } else if (!seen.insert(v.get<std::string>()).second) {
        Fail(where, std::string(key) + " repeats a value");
      }
    }
  }
  void Stamp(const std::string& where, const json& obj, const char* key) {
    if (!obj.contains(key)) return;
    static const std::regex kStamp("[0-9a-f]{16}");
    if (!obj[key].is_string() ||
        !std::regex_match(obj[key].get<std::string>(), kStamp)) {
      Fail(where, std::string(key) + " is not 16 lowercase hex characters");
    }
  }
  void HeaderLines(const std::string& where, const json& obj, const char* key,
                   bool allow_empty_name) {
    if (!obj.contains(key)) return;
    bool ok = obj[key].is_array();
    if (ok) {
      for (const json& line : obj[key]) {
        ok = ok && line.is_array() && line.size() == 2 && line[0].is_string() &&
             line[1].is_string() &&
             (allow_empty_name || !line[0].get<std::string>().empty());
      }
    }
    if (!ok) Fail(where, std::string(key) + " is not a list of [name, value]");
  }
  void Strings(const std::string& where, const json& obj, const char* key) {
    if (!obj.contains(key)) return;
    bool ok = obj[key].is_array();
    if (ok) {
      for (const json& v : obj[key]) ok = ok && v.is_string();
    }
    if (!ok) Fail(where, std::string(key) + " is not a list of strings");
  }
  void Hash(const std::string& where, const json& obj, const char* key,
            bool nullable) {
    if (!obj.contains(key)) return;
    if (nullable && obj[key].is_null()) return;
    static const std::regex kHex("[0-9a-f]{64}");
    if (!obj[key].is_string() ||
        !std::regex_match(obj[key].get<std::string>(), kHex)) {
      Fail(where, std::string(key) + " is not 64 lowercase hex characters");
    }
  }

 private:
  std::vector<std::string> errors_;
};

const StringSet kModes = {"off", "report-only", "serve-variants",
                          "serve-cached-html"};
const StringSet kStoreActions = {"refuse",
                                 "store",
                                 "store_no_notify",
                                 "would_store",
                                 "would_store_no_notify",
                                 "malformed"};
const StringSet kStoreReasons = {"none",
                                 "method",
                                 "status",
                                 "incomplete",
                                 "auth_request",
                                 "cookie_request",
                                 "set_cookie",
                                 "private",
                                 "no_store",
                                 "no_lifetime",
                                 "vary",
                                 "headers",
                                 "nonce_csp",
                                 "nonce_link",
                                 "content_type",
                                 "encoding",
                                 "size",
                                 "url",
                                 "personalisation_signal",
                                 "no_transform",
                                 "mode"};
const StringSet kStoreFlags = {"lifetime_explicit", "needs_validation",
                               "has_validators", "explicit_shared"};
const StringSet kCcFlags = {
    "no_cache",         "must_revalidate",  "no_store",
    "private",          "public",           "immutable",
    "s_maxage_present", "proxy_revalidate", "no_transform"};
const StringSet kRequestFlags = {"authorization", "authenticated_user",
                                 "cookie", "no_store"};
const StringSet kServeActions = {"fall_through", "serve_markdown", "serve_html",
                                 "validate", "malformed"};
// The 1.12 serve reasons plus the ones the 1.13 surface appends.
const StringSet kServeReasons = {"fresh",
                                 "mode",
                                 "auth_request",
                                 "cookie_request",
                                 "client_no_cache",
                                 "needs_validation",
                                 "stale",
                                 "headers_not_reproducible",
                                 "sidecar_missing",
                                 "purged",
                                 "absent",
                                 "binding_stale",
                                 "parent_stale",
                                 "method",
                                 "range",
                                 "cors",
                                 "no_transform",
                                 "withheld",
                                 "skew"};
const StringSet kOutcomes = {"markdown_200", "markdown_304", "fall_through"};

// The value pending rows carried while the 1.15 surface was unbuilt. No row
// may carry it, or any other pending marker, now.
constexpr char kPendingSurface[] = "c-api-1.15";

// What the 1.15 surface adds to the sets above.
const StringSet kServeReasons115 = {
    "scope",        "capped",          "conditional",    "credentialed",
    "legacy_entry", "policy_mismatch", "request_header", "path"};
const StringSet kStoreReasons115 = {"request_header", "path"};
const StringSet kStoreFlags115 = {"request_anonymous"};
const StringSet kRequestFlags115 = {"explicit_public"};
const StringSet kServeRequestFlags115 = {"fresh_only", "html_fallback",
                                         "conditional", "explicit_public"};
const StringSet kScopes115 = {"agents", "all"};
const StringSet kStructSizes115 = {"1.14", "1.15"};
const StringSet kEntryFlags115 = {"store_v2", "anonymous_recorded"};
const StringSet kWrittenBy115 = {"front", "module_v1", "module_v2_no_stamp",
                                 "module_v2_anonymous",
                                 "module_v2_credentialed"};
const StringSet kGuardReasons115 = {"none", "request_header", "path"};
const StringSet kCookieClasses115 = {"none", "ignorable_only", "blocking"};
const StringSet kCredentialed115 = {"anonymous-only", "explicit-public"};
const StringSet kListVersions115 = {"current", "next"};
// The calls only the 1.15 surface answers.
const StringSet kCalls115 = {"store_write",  "guard", "cookies",
                             "ignore_guard", "stamp", "invalidate",
                             "format"};

StringSet Union(const StringSet& a, const StringSet& b) {
  StringSet out = a;
  out.insert(b.begin(), b.end());
  return out;
}

// The set a row may use: the base set plus the 1.15 additions. The `pending`
// argument is kept so the checks below read as they did while the additions
// were gated; the surface is built, so every row may use them.
StringSet For(bool /*pending*/, const StringSet& base, const StringSet& added) {
  return Union(base, added);
}
const StringSet kMethods = {"GET",    "HEAD",    "POST", "PUT",
                            "DELETE", "OPTIONS", "PATCH"};
const StringSet kContents = {"html", "css", "js", "image", "other"};
const StringSet kDeriverVersions = {"current", "previous"};
const StringSet kWithheldReasons = {"below_floor", "too_large", "encoded",
                                    "hash_mismatch", "extract_failed"};

// The request guard's input (call guard, and request_guard on store and serve).
void CheckGuardInput(Checker& c, const std::string& w, const json& g) {
  c.Keys(w, g, {"raw_path", "headers", "keyed_scheme"},
         {"raw_path", "headers", "keyed_scheme"});
  c.Str(w, g, "raw_path");
  c.HeaderLines(w, g, "headers", /*allow_empty_name=*/false);
  c.OneOf(w, g, "keyed_scheme", {"http", "https"});
}

void CheckStore(Checker& c, const std::string& w, const json& in,
                const json& ex, bool pending, bool write) {
  StringSet keys = {"mode",          "method",         "url_path", "request",
                    "status",        "headers",        "body",     "body_bytes",
                    "body_complete", "max_body_bytes", "now_s"};
  if (pending) {
    keys.insert("request_guard");
    keys.insert("credentialed");
  }
  if (write) {
    keys.insert("policy_stamp");
    keys.insert("capture_start_ms");
    keys.insert("sidecar_format3");
  }
  StringSet required = {
      "mode",    "method", "url_path",      "request",        "status",
      "headers", "body",   "body_complete", "max_body_bytes", "now_s"};
  if (write) {
    required.insert("policy_stamp");
    required.insert("capture_start_ms");
  }
  c.Keys(w + " input", in, keys, required);
  if (in.contains("mode") && !in["mode"].is_number_integer()) {
    c.OneOf(w + " input", in, "mode", kModes);
  }
  c.OneOf(w + " input", in, "method", kMethods);
  c.Str(w + " input", in, "url_path");
  c.SubsetOf(w + " input", in, "request",
             For(pending, kRequestFlags, kRequestFlags115));
  c.Uint(w + " input", in, "status");
  c.Str(w + " input", in, "body");
  c.Uint(w + " input", in, "body_bytes");
  c.Bool(w + " input", in, "body_complete");
  c.Uint(w + " input", in, "max_body_bytes");
  c.Uint(w + " input", in, "now_s");
  c.Stamp(w + " input", in, "policy_stamp");
  c.Uint(w + " input", in, "capture_start_ms");
  c.Bool(w + " input", in, "sidecar_format3");
  c.OneOf(w + " input", in, "credentialed", kCredentialed115);
  c.HeaderLines(w + " input", in, "headers", /*allow_empty_name=*/false);
  if (in.contains("request_guard")) {
    CheckGuardInput(c, w + " request_guard", in["request_guard"]);
  }
  const StringSet reasons = For(pending, kStoreReasons, kStoreReasons115);
  if (write) {
    c.Keys(w + " expect", ex,
           {"action", "reason", "written", "entry_flags", "entry_flags_clear",
            "policy_stamp"},
           {"action", "written"});
    c.OneOf(w + " expect", ex, "action", kStoreActions);
    c.OneOf(w + " expect", ex, "reason", reasons);
    c.Bool(w + " expect", ex, "written");
    c.SubsetOf(w + " expect", ex, "entry_flags", kEntryFlags115);
    c.SubsetOf(w + " expect", ex, "entry_flags_clear", kEntryFlags115);
    c.Stamp(w + " expect", ex, "policy_stamp");
    if (ex.value("written", false) != ex.contains("entry_flags")) {
      c.Fail(w + " expect", "entry_flags is asserted exactly when written");
    }
    return;
  }
  StringSet expect_keys = {"action",     "reason", "detail",
                           "lifetime_s", "flags",  "cc_flags"};
  if (pending) expect_keys.insert("flags_clear");
  c.Keys(w + " expect", ex, expect_keys, {"action"});
  c.OneOf(w + " expect", ex, "action", kStoreActions);
  c.OneOf(w + " expect", ex, "reason", reasons);
  c.Str(w + " expect", ex, "detail");
  c.Uint(w + " expect", ex, "lifetime_s");
  const StringSet flags = For(pending, kStoreFlags, kStoreFlags115);
  c.SubsetOf(w + " expect", ex, "flags", flags);
  c.SubsetOf(w + " expect", ex, "flags_clear", flags);
  c.SubsetOf(w + " expect", ex, "cc_flags", kCcFlags);
}

void CheckServeRequest(Checker& c, const std::string& w, const json& r) {
  c.Keys(w, r,
         {"method", "accept", "authorization", "authenticated_user", "cookie",
          "cache_control", "pragma", "range", "if_range", "origin",
          "if_none_match", "now_s"},
         {"method", "now_s"});
  c.OneOf(w, r, "method", kMethods);
  c.Strings(w, r, "accept");
  for (const char* key : {"authorization", "authenticated_user", "cookie",
                          "range", "if_range", "origin"}) {
    c.Bool(w, r, key);
  }
  for (const char* key : {"cache_control", "pragma", "if_none_match"}) {
    c.Str(w, r, key);
  }
  c.Uint(w, r, "now_s");
}

// serve_scope is a name from the set, or an integer for a value outside it.
void CheckScopeAndClass(Checker& c, const std::string& w, const json& in) {
  if (in.contains("serve_scope") && !in["serve_scope"].is_number_unsigned()) {
    c.OneOf(w, in, "serve_scope", kScopes115);
  }
  c.Uint(w, in, "agent_class");
  c.SubsetOf(w, in, "request_flags", kServeRequestFlags115);
  c.Stamp(w, in, "policy_stamp");
}

void CheckServe(Checker& c, const std::string& w, const json& in,
                const json& ex, bool pending) {
  StringSet keys = {"mode", "request", "stored"};
  if (pending) {
    keys.insert({"struct_size", "serve_scope", "agent_class",
                 "html_max_lifetime_s", "request_flags", "policy_stamp",
                 "request_guard"});
  }
  c.Keys(w + " input", in, keys, {"mode", "request", "stored"});
  if (in.contains("mode") && !in["mode"].is_number_integer()) {
    c.OneOf(w + " input", in, "mode", kModes);
  }
  c.OneOf(w + " input", in, "struct_size", kStructSizes115);
  CheckScopeAndClass(c, w + " input", in);
  c.Uint(w + " input", in, "html_max_lifetime_s");
  if (in.contains("request_guard")) {
    CheckGuardInput(c, w + " request_guard", in["request_guard"]);
  }
  if (in.contains("request"))
    CheckServeRequest(c, w + " request", in["request"]);
  if (in.contains("stored")) {
    const json& s = in["stored"];
    const StringSet required = {"present",
                                "stored_at",
                                "age_at_store",
                                "lifetime_s",
                                "needs_validation",
                                "explicit_shared",
                                "headers_not_reproducible",
                                "sidecar_required",
                                "sidecar_present",
                                "purged",
                                "cc_flags",
                                "variant_present",
                                "binding_live",
                                "withheld",
                                "render_ttl_s",
                                "binding_hash",
                                "markdown_hash"};
    StringSet allowed = required;
    if (pending) {
      allowed.insert({"store_v2", "anonymous_recorded",
                      "validated_this_request", "entry_policy_stamp"});
    }
    c.Keys(w + " stored", s, allowed, required);
    for (const char* key :
         {"present", "needs_validation", "explicit_shared",
          "headers_not_reproducible", "sidecar_required", "sidecar_present",
          "purged", "variant_present", "binding_live", "withheld", "store_v2",
          "anonymous_recorded", "validated_this_request"}) {
      c.Bool(w + " stored", s, key);
    }
    for (const char* key :
         {"stored_at", "age_at_store", "lifetime_s", "render_ttl_s"}) {
      c.Uint(w + " stored", s, key);
    }
    c.SubsetOf(w + " stored", s, "cc_flags", kCcFlags);
    c.Hash(w + " stored", s, "binding_hash", false);
    c.Hash(w + " stored", s, "markdown_hash", false);
    c.Stamp(w + " stored", s, "entry_policy_stamp");
  }
  StringSet expect_keys = {"action",         "reason", "enforce",
                           "wants_markdown", "age_s",  "outcome"};
  if (pending) expect_keys.insert("not_modified");
  c.Keys(w + " expect", ex, expect_keys, {"action"});
  c.OneOf(w + " expect", ex, "action", kServeActions);
  c.OneOf(w + " expect", ex, "reason",
          For(pending, kServeReasons, kServeReasons115));
  c.Bool(w + " expect", ex, "enforce");
  c.Bool(w + " expect", ex, "wants_markdown");
  c.Bool(w + " expect", ex, "not_modified");
  c.Uint(w + " expect", ex, "age_s");
  c.OneOf(w + " expect", ex, "outcome", kOutcomes);
  if (ex.value("outcome", "") == "markdown_304" &&
      !(in.contains("request") && in["request"].contains("if_none_match"))) {
    c.Fail(w, "a 304 outcome is asserted without an if_none_match");
  }
}

void CheckFacts(Checker& c, const std::string& w, const json& in,
                const json& ex, bool pending) {
  c.Keys(w + " input", in, For(pending, {"volume", "now_s"}, {"serve_request"}),
         {"volume", "now_s"});
  c.Uint(w + " input", in, "now_s");
  if (in.contains("serve_request")) {
    const json& r = in["serve_request"];
    c.Keys(w + " serve_request", r,
           {"mode", "accept", "serve_scope", "agent_class", "request_flags",
            "policy_stamp", "struct_size"},
           {"mode"});
    c.OneOf(w + " serve_request", r, "struct_size", kStructSizes115);
    c.OneOf(w + " serve_request", r, "mode", kModes);
    c.Strings(w + " serve_request", r, "accept");
    CheckScopeAndClass(c, w + " serve_request", r);
  }
  if (in.contains("volume")) {
    const json& v = in["volume"];
    c.Keys(w + " volume", v,
           For(pending, {"original", "markdown", "withheld_marker", "sidecar"},
               {"sidecar_policy_stamp"}),
           {"sidecar"});
    c.Bool(w + " volume", v, "sidecar");
    c.Stamp(w + " volume", v, "sidecar_policy_stamp");
    if (v.contains("original")) {
      const json& o = v["original"];
      c.Keys(w + " original", o,
             For(pending,
                 {"stored_at", "lifetime_s", "cc_flags", "origin_html_hash",
                  "sidecar_required"},
                 {"written_by"}),
             {"stored_at", "lifetime_s", "cc_flags", "origin_html_hash"});
      c.Bool(w + " original", o, "sidecar_required");
      c.Uint(w + " original", o, "stored_at");
      c.Uint(w + " original", o, "lifetime_s");
      c.SubsetOf(w + " original", o, "cc_flags", kCcFlags);
      c.Hash(w + " original", o, "origin_html_hash", true);
      c.OneOf(w + " original", o, "written_by", kWrittenBy115);
    }
    if (v.contains("markdown")) {
      const json& m = v["markdown"];
      c.Keys(w + " markdown", m, {"origin_html_hash", "markdown_hash"},
             {"origin_html_hash", "markdown_hash"});
      c.Hash(w + " markdown", m, "origin_html_hash", false);
      c.Hash(w + " markdown", m, "markdown_hash", false);
    }
    if (v.contains("withheld_marker")) {
      const json& m = v["withheld_marker"];
      c.Keys(w + " withheld_marker", m,
             {"parent_hash", "deriver_version", "reason"},
             {"parent_hash", "deriver_version", "reason"});
      c.Hash(w + " withheld_marker", m, "parent_hash", false);
      c.OneOf(w + " withheld_marker", m, "deriver_version", kDeriverVersions);
      c.OneOf(w + " withheld_marker", m, "reason", kWithheldReasons);
    }
  }
  c.Keys(w + " expect", ex,
         For(pending,
             {"present", "variant_present", "binding_live", "withheld",
              "explicit_shared", "sidecar_present", "sidecar_required",
              "needs_validation", "stored_at", "age_at_store", "lifetime_s",
              "cc_flags", "serve", "etag"},
             {"store_v2", "anonymous_recorded", "entry_policy_stamp"}),
         {"present"});
  if (ex.contains("etag") && !ex["etag"].is_null()) {
    static const std::regex kFactsTag("W/\"psm-[0-9a-f]{16}\"");
    if (!ex["etag"].is_string() ||
        !std::regex_match(ex["etag"].get<std::string>(), kFactsTag)) {
      c.Fail(w + " expect", "etag is not null or W/\"psm-<16 hex>\"");
    }
  }
  for (const char* key :
       {"present", "variant_present", "binding_live", "withheld",
        "explicit_shared", "sidecar_present", "sidecar_required",
        "needs_validation", "store_v2", "anonymous_recorded"}) {
    c.Bool(w + " expect", ex, key);
  }
  for (const char* key : {"stored_at", "age_at_store", "lifetime_s"}) {
    c.Uint(w + " expect", ex, key);
  }
  c.Stamp(w + " expect", ex, "entry_policy_stamp");
  c.SubsetOf(w + " expect", ex, "cc_flags", kCcFlags);
  if (ex.contains("serve")) {
    c.Keys(w + " expect serve", ex["serve"], {"action", "reason"},
           {"action", "reason"});
    c.OneOf(w + " expect serve", ex["serve"], "action", kServeActions);
    c.OneOf(w + " expect serve", ex["serve"], "reason",
            For(pending, kServeReasons, kServeReasons115));
  }
}

void CheckGuard(Checker& c, const std::string& w, const json& in,
                const json& ex) {
  CheckGuardInput(c, w + " input", in);
  c.Keys(w + " expect", ex, {"reason", "header_index", "xfp"}, {"reason"});
  c.OneOf(w + " expect", ex, "reason", kGuardReasons115);
  c.Uint(w + " expect", ex, "header_index");
  c.Bool(w + " expect", ex, "xfp");
}

void CheckCookies(Checker& c, const std::string& w, const json& in,
                  const json& ex) {
  c.Keys(w + " input", in, {"cookie_lines", "ignore_builtin", "additions"},
         {"cookie_lines", "ignore_builtin", "additions"});
  c.Strings(w + " input", in, "cookie_lines");
  c.Bool(w + " input", in, "ignore_builtin");
  c.Strings(w + " input", in, "additions");
  c.Keys(w + " expect", ex, {"class", "first_blocking", "ignorable_values"},
         {"class"});
  c.OneOf(w + " expect", ex, "class", kCookieClasses115);
  c.Str(w + " expect", ex, "first_blocking");
  c.Strings(w + " expect", ex, "ignorable_values");
  if (ex.contains("first_blocking") && ex.value("class", "") != "blocking") {
    c.Fail(w + " expect", "first_blocking is asserted only for blocking");
  }
}

void CheckIgnoreGuard(Checker& c, const std::string& w, const json& in,
                      const json& ex) {
  c.Keys(w + " input", in, {"addition"}, {"addition"});
  c.Str(w + " input", in, "addition");
  c.Keys(w + " expect", ex, {"accepted", "warning"}, {"accepted", "warning"});
  c.Bool(w + " expect", ex, "accepted");
  c.Bool(w + " expect", ex, "warning");
}

void CheckStampCall(Checker& c, const std::string& w, const json& in,
                    const json& ex) {
  c.Keys(w + " input", in, {"a", "b"}, {"a", "b"});
  for (const char* side : {"a", "b"}) {
    if (!in.contains(side)) continue;
    const std::string sw = w + " " + side;
    const json& s = in[side];
    c.Keys(sw, s,
           {"credentialed", "ignore_builtin", "list_version", "additions"},
           {"credentialed", "ignore_builtin", "list_version", "additions"});
    c.OneOf(sw, s, "credentialed", kCredentialed115);
    c.Bool(sw, s, "ignore_builtin");
    c.OneOf(sw, s, "list_version", kListVersions115);
    c.Strings(sw, s, "additions");
  }
  c.Keys(w + " expect", ex, {"equal", "nonzero"}, {"equal"});
  c.Bool(w + " expect", ex, "equal");
  c.Bool(w + " expect", ex, "nonzero");
}

void CheckInvalidate(Checker& c, const std::string& w, const json& in,
                     const json& ex) {
  c.Keys(w + " input", in, {"method", "status"}, {"method", "status"});
  if (in.contains("method") && !in["method"].is_null()) {
    c.Str(w + " input", in, "method");
  }
  c.Uint(w + " input", in, "status");
  c.Keys(w + " expect", ex, {"invalidates"}, {"invalidates"});
  c.Bool(w + " expect", ex, "invalidates");
}

void CheckFormat(Checker& c, const std::string& w, const json& in,
                 const json& ex) {
  c.Keys(w + " input", in, {"fields"}, {"fields"});
  c.HeaderLines(w + " input", in, "fields", /*allow_empty_name=*/false);
  c.Keys(w + " expect", ex, {"line"}, {"line"});
  if (ex.contains("line") && !ex["line"].is_null()) {
    c.Str(w + " expect", ex, "line");
    if (ex["line"].is_string() &&
        json::parse(ex["line"].get<std::string>(), nullptr, false)
            .is_discarded()) {
      c.Fail(w + " expect", "line is not one JSON object");
    }
  }
}

void CheckEtag(Checker& c, const std::string& w, const json& in,
               const json& ex) {
  c.Keys(w + " input", in, {"binding_hash", "markdown_hash"},
         {"binding_hash", "markdown_hash"});
  c.Hash(w + " input", in, "binding_hash", false);
  c.Hash(w + " input", in, "markdown_hash", false);
  c.Keys(w + " expect", ex, {"etag"}, {"etag"});
  static const std::regex kTag("W/\"psm-[0-9a-f]{16}\"");
  if (!ex.contains("etag") || !ex["etag"].is_string() ||
      !std::regex_match(ex["etag"].get<std::string>(), kTag)) {
    c.Fail(w + " expect", "etag is not W/\"psm-<16 hex>\"");
  }
}

void CheckInm(Checker& c, const std::string& w, const json& in,
              const json& ex) {
  c.Keys(w + " input", in, {"if_none_match", "etag"},
         {"if_none_match", "etag"});
  c.Str(w + " input", in, "if_none_match");
  c.Str(w + " input", in, "etag");
  c.Keys(w + " expect", ex, {"match"}, {"match"});
  c.Bool(w + " expect", ex, "match");
}

void CheckWriteParams(Checker& c, const std::string& w, const json& in,
                      const json& ex) {
  c.Keys(w + " input", in, {"capture_start_ms", "age"}, {"capture_start_ms"});
  c.Uint(w + " input", in, "capture_start_ms");
  if (in.contains("age")) {
    bool ok = in["age"].is_array() && !in["age"].empty();
    if (ok) {
      for (const json& line : in["age"]) ok = ok && line.is_string();
    }
    if (!ok) c.Fail(w + " input", "age is not a non-empty list of strings");
  }
  c.Keys(w + " expect", ex, {"valid", "inserted_at_s", "age_at_store"},
         {"valid"});
  c.Bool(w + " expect", ex, "valid");
  c.Uint(w + " expect", ex, "inserted_at_s");
  c.Uint(w + " expect", ex, "age_at_store");
  if (ex.value("valid", false) != ex.contains("inserted_at_s")) {
    c.Fail(w + " expect", "inserted_at_s is asserted exactly when valid");
  }
}

void CheckNotify(Checker& c, const std::string& w, const json& in,
                 const json& ex) {
  c.Keys(w + " input", in,
         {"content", "flags", "site", "agent_request", "mask"},
         {"content", "flags", "site", "agent_request"});
  c.OneOf(w + " input", in, "content", kContents);
  c.Bool(w + " input", in, "agent_request");
  c.OneOf(w + " input", in, "mask", {"llms_txt"});
  if (in.contains("flags")) {
    static const std::regex kFlag(
        "from_module|host_vouched|html_stored|reserved_0x[0-9a-f]+");
    bool ok = in["flags"].is_array();
    if (ok) {
      for (const json& f : in["flags"]) {
        ok = ok && f.is_string() &&
             std::regex_match(f.get<std::string>(), kFlag);
      }
    }
    if (!ok) c.Fail(w + " input", "flags has a value outside its set");
  }
  if (in.contains("site") && !in["site"].is_null()) {
    // PS_SITE_ID_CHARS: a shorter or longer site is refused by the library,
    // so a row with one would fail for the wrong reason.
    static const std::regex kSite("[0-9a-f]{64}");
    if (!in["site"].is_string() ||
        !std::regex_match(in["site"].get<std::string>(), kSite)) {
      c.Fail(w + " input", "site is not 64 lowercase hex characters or null");
    }
  }
  c.Keys(w + " expect", ex, {"valid"}, {"valid"});
  c.Bool(w + " expect", ex, "valid");
}

// A label names an RFC section or a short rule; it never carries a project
// tracking identifier.
bool LabelIsPublicSafe(const std::string& why) {
  static const std::regex kForbidden(
      "(^|[^A-Za-z0-9])([SVGDCKFO][0-9]+|[A-Z]{2,6}-[0-9]+|[A-Z]-[A-Z]|"
      "ADR|[Cc]onstraint|[Rr]uling|[Cc]ard)([^A-Za-z0-9]|$)");
  return !why.empty() && why.size() <= 120 &&
         !std::regex_search(why, kForbidden);
}

TEST(DaemonHtmlPolicyCasesTest, FileMatchesItsHashPin) {
  const std::string contents = ReadDaemonHtmlPolicyCases();
  ASSERT_FALSE(contents.empty());
  EXPECT_EQ(PolicyCasesFnv1a64(contents), kDaemonHtmlPolicyCasesFnv1a64)
      << "The policy cases changed without a pin update. If the change is "
         "intended, set kDaemonHtmlPolicyCasesFnv1a64 in "
         "test/lib/cache/daemon_html_policy_cases.h to 0x"
      << std::hex << PolicyCasesFnv1a64(contents);
}

TEST(DaemonHtmlPolicyCasesTest, PinDetectsAOneByteEdit) {
  std::string contents = ReadDaemonHtmlPolicyCases();
  ASSERT_FALSE(contents.empty());
  const size_t at = contents.find("\"refuse\"");
  ASSERT_NE(at, std::string::npos);
  contents[at + 1] = 'R';
  EXPECT_NE(PolicyCasesFnv1a64(contents), kDaemonHtmlPolicyCasesFnv1a64);
  // Carriage returns are not part of the pinned bytes.
  std::string with_cr = ReadDaemonHtmlPolicyCases();
  with_cr.insert(with_cr.find('\n'), "\r");
  with_cr.erase(std::remove(with_cr.begin(), with_cr.end(), '\r'),
                with_cr.end());
  EXPECT_EQ(PolicyCasesFnv1a64(with_cr), kDaemonHtmlPolicyCasesFnv1a64);
}

TEST(DaemonHtmlPolicyCasesTest, EveryRowHasTheDocumentedShape) {
  const json doc = LoadCases();
  ASSERT_FALSE(doc.is_discarded()) << "not valid JSON";
  Checker c;
  c.Keys("file", doc, {"format", "version", "about", "rows"},
         {"format", "version", "about", "rows"});
  EXPECT_EQ(doc.value("format", ""), "daemon-html-policy-cases");
  EXPECT_EQ(doc.value("version", 0), 1);
  ASSERT_TRUE(doc["about"].is_array());
  for (const json& line : doc["about"]) EXPECT_TRUE(line.is_string());
  ASSERT_TRUE(doc["rows"].is_array());
  ASSERT_FALSE(doc["rows"].empty());

  static const std::regex kId("[a-z0-9][a-z0-9-]*");
  StringSet ids;
  std::map<std::string, int> per_call;
  for (const json& row : doc["rows"]) {
    const std::string id = row.value("id", "");
    const std::string w = "row '" + id + "'";
    c.Keys(w, row, {"id", "runners", "why", "policy", "pending"},
           {"id", "runners", "why", "policy"});
    if (row.contains("pending")) {
      c.Fail(w,
             "pending, but no library surface is pending: the runner "
             "runs every row");
    }
    // Every key, call and value of the 1.15 surface is open to every row.
    const bool pending = true;
    if (!std::regex_match(id, kId)) c.Fail(w, "id is not [a-z0-9-]");
    if (!ids.insert(id).second) c.Fail(w, "duplicate id");
    if (row.value("runners", json::array()) != json::array({"policy"})) {
      c.Fail(w, "runners must be exactly [\"policy\"] in this file");
    }
    if (!row.contains("why") || !row["why"].is_string() ||
        !LabelIsPublicSafe(row["why"].get<std::string>())) {
      c.Fail(w, "why is missing, too long, or not a public label");
    }
    if (!row.contains("policy")) continue;
    const json& p = row["policy"];
    c.Keys(w, p, {"call", "input", "expect"}, {"call", "input", "expect"});
    const std::string call = p.value("call", "");
    const json in = p.value("input", json::object());
    const json ex = p.value("expect", json::object());
    ++per_call[call];
    if (call == "store") {
      CheckStore(c, w, in, ex, pending, /*write=*/false);
    } else if (call == "store_write") {
      CheckStore(c, w, in, ex, pending, /*write=*/true);
    } else if (call == "serve") {
      CheckServe(c, w, in, ex, pending);
    } else if (call == "facts") {
      CheckFacts(c, w, in, ex, pending);
    } else if (call == "guard") {
      CheckGuard(c, w, in, ex);
    } else if (call == "cookies") {
      CheckCookies(c, w, in, ex);
    } else if (call == "ignore_guard") {
      CheckIgnoreGuard(c, w, in, ex);
    } else if (call == "stamp") {
      CheckStampCall(c, w, in, ex);
    } else if (call == "invalidate") {
      CheckInvalidate(c, w, in, ex);
    } else if (call == "format") {
      CheckFormat(c, w, in, ex);
    } else if (call == "etag") {
      CheckEtag(c, w, in, ex);
    } else if (call == "inm") {
      CheckInm(c, w, in, ex);
    } else if (call == "notify") {
      CheckNotify(c, w, in, ex);
    } else if (call == "write_params") {
      CheckWriteParams(c, w, in, ex);
    } else {
      c.Fail(w, "call '" + call + "' is not one of the documented calls");
    }
  }
  for (const std::string& e : c.errors()) ADD_FAILURE() << e;
  for (const char* call :
       {"store", "serve", "facts", "etag", "inm", "notify", "write_params"}) {
    EXPECT_GT(per_call[call], 0) << "no " << call << " rows";
  }
  for (const std::string& call : kCalls115) {
    EXPECT_GT(per_call[call], 0) << "no " << call << " rows";
  }
}

// The notify argument rule, restated as the list of rules a notification
// breaks. Used only to check the rows: each expected verdict must follow from
// the rule, and a row expected invalid must break exactly one rule, so it
// cannot pass for a reason other than the one it is about.
std::vector<std::string> NotifyRulesBroken(const json& in) {
  std::vector<std::string> broken;
  StringSet flags;
  for (const json& f : in["flags"]) flags.insert(f.get<std::string>());
  const bool from_module = flags.count("from_module") != 0;
  const bool vouched = flags.count("host_vouched") != 0;
  const bool stored = flags.count("html_stored") != 0;
  const bool html = in["content"] == "html";
  const bool agent = in["agent_request"].get<bool>();
  for (const std::string& f : flags) {
    if (f.rfind("reserved_", 0) == 0) broken.push_back("reserved flag");
  }
  if (stored && !from_module) broken.push_back("stored without module");
  if (from_module) {
    if (in["site"].is_null()) broken.push_back("module without site");
    if (in.contains("mask")) broken.push_back("module with mask");
    if (html && !(vouched && stored)) broken.push_back("module html flags");
    if (!html && stored) broken.push_back("stored on non-html");
    if (!html && vouched) broken.push_back("vouched on non-html");
    if (!html && agent) broken.push_back("agent on non-html");
  }
  return broken;
}

TEST(DaemonHtmlPolicyCasesTest, EachInvalidNotifyRowBreaksOneRule) {
  const json doc = LoadCases();
  ASSERT_FALSE(doc.is_discarded());
  int rows = 0;
  for (const json& row : doc["rows"]) {
    const json& p = row["policy"];
    if (p.value("call", "") != "notify") continue;
    ++rows;
    const std::vector<std::string> broken = NotifyRulesBroken(p["input"]);
    const bool valid = p["expect"]["valid"].get<bool>();
    EXPECT_EQ(valid, broken.empty()) << row["id"];
    if (!valid) {
      EXPECT_EQ(broken.size(), 1u) << row["id"];
    }
  }
  EXPECT_GT(rows, 0);
}

TEST(DaemonHtmlPolicyCasesTest, RejectsMalformedRows) {
  Checker c;
  CheckServe(c, "bad", json::parse(R"({"mode":"serve-variants",
      "request":{"method":"GET","now_s":1,"surprise":true},
      "stored":{}})"),
             json::parse(R"({"action":"serve_markdown","reason":"bogus"})"),
             /*pending=*/false);
  EXPECT_GE(c.errors().size(), 3u);  // unknown key, missing facts, bad reason
  EXPECT_FALSE(LabelIsPublicSafe("store: rule S99"));
  EXPECT_FALSE(LabelIsPublicSafe("per constraint 99"));
  EXPECT_FALSE(LabelIsPublicSafe("tracked as XYZ-99"));
  EXPECT_TRUE(LabelIsPublicSafe("store: private (RFC 9111 5.2.2.7)"));
}

// The 1.15 keys and values are accepted on every row now that the runner
// reads them; a key outside the sets is still refused.
TEST(DaemonHtmlPolicyCasesTest, EveryRowMayUseThe1_15Surface) {
  const json in = json::parse(R"({"mode":"serve-cached-html",
      "serve_scope":"agents","agent_class":3,"request_flags":["fresh_only"],
      "request":{"method":"GET","accept":["text/html"],"now_s":300},
      "stored":{"present":true,"stored_at":0,"age_at_store":0,
        "lifetime_s":3600,"needs_validation":false,"explicit_shared":false,
        "headers_not_reproducible":false,"sidecar_required":false,
        "sidecar_present":false,"purged":false,"cc_flags":[],
        "variant_present":true,"binding_live":true,"withheld":false,
        "render_ttl_s":86400,"store_v2":true,
        "binding_hash":"79d58b741cf7cd8208244aa41115a070a4a3c509571d4424c904e29188966c32",
        "markdown_hash":"558f11876c6b1b24ef8133ddceb169bc51e941da83cf589d2bdc88ef8724b41a"}})");
  const json ex = json::parse(R"({"action":"fall_through","reason":"scope"})");
  Checker c;
  CheckServe(c, "row", in, ex, /*pending=*/true);
  EXPECT_TRUE(c.errors().empty());
  for (const std::string& e : c.errors()) ADD_FAILURE() << e;
  json bad = in;
  bad["serve_scope_typo"] = "agents";
  Checker refused;
  CheckServe(refused, "row", bad, ex, /*pending=*/true);
  EXPECT_EQ(refused.errors().size(), 1u);
}

// The 1.15 surface is built: no row is pending any more, and the pin says so.
TEST(DaemonHtmlPolicyCasesTest, NoRowIsPending) {
  const json doc = LoadCases();
  ASSERT_FALSE(doc.is_discarded());
  int pending = 0;
  for (const json& row : doc["rows"]) {
    if (!row.contains("pending")) continue;
    ++pending;
    ADD_FAILURE() << row["id"] << " still carries pending "
                  << row["pending"].dump() << " (the " << kPendingSurface
                  << " surface is built)";
  }
  EXPECT_EQ(pending, 0);
  EXPECT_EQ(static_cast<size_t>(pending), kDaemonHtmlPolicyCasesPendingRows);
}

// Each Accept vector in the shared vector file has one serve row with the same
// Accept lines and the same verdict, so the two files cannot drift apart.
TEST(DaemonHtmlPolicyCasesTest, AcceptRowsMatchTheSharedVectorFile) {
  std::ifstream in(kVectorPath, std::ios::binary);
  ASSERT_TRUE(in.good()) << kVectorPath;
  std::map<int, std::pair<bool, std::vector<std::string>>> vectors;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    int id = 0;
    std::string verdict;
    fields >> id >> verdict;
    std::vector<std::string> lines;
    for (size_t open = line.find('<'); open != std::string::npos;
         open = line.find('<', open + 1)) {
      const size_t close = line.find('>', open);
      ASSERT_NE(close, std::string::npos) << line;
      lines.push_back(line.substr(open + 1, close - open - 1));
      open = close;
    }
    vectors[id] = {verdict == "MD", lines};
  }
  ASSERT_EQ(static_cast<int>(vectors.size()), kVectorCount);

  const json doc = LoadCases();
  ASSERT_FALSE(doc.is_discarded());
  std::map<int, const json*> rows;
  static const std::regex kVectorId("serve-accept-vector-([0-9]{2})");
  for (const json& row : doc["rows"]) {
    std::smatch m;
    const std::string id = row.value("id", "");
    if (std::regex_match(id, m, kVectorId)) rows[std::stoi(m[1])] = &row;
  }
  ASSERT_EQ(rows.size(), vectors.size());
  for (const auto& [id, vector] : vectors) {
    ASSERT_EQ(rows.count(id), 1u) << "vector " << id;
    const json& p = (*rows[id])["policy"];
    const json& request = p["input"]["request"];
    if (vector.second.empty()) {
      EXPECT_FALSE(request.contains("accept")) << "vector " << id;
    } else {
      EXPECT_EQ(request.value("accept", json::array()), json(vector.second))
          << "vector " << id;
    }
    EXPECT_EQ(p["expect"].value("wants_markdown", !vector.first), vector.first)
        << "vector " << id;
    EXPECT_EQ(p["expect"].value("action", ""),
              vector.first ? "serve_markdown" : "fall_through")
        << "vector " << id;
  }
}

}  // namespace
}  // namespace test
}  // namespace pagespeed
