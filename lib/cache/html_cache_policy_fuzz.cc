// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Fuzz harness for the HTML cache policy (lib/cache/html_cache_policy.h) and
// its decision line formatter (lib/cache/html_cache_decision.h).  Both run on
// untrusted input: response headers and a body prefix an origin chose, and a
// path a client chose.  Build optimized with a sanitizer so release behaviour
// is what gets exercised and AddressSanitizer is the memory-safety oracle:
//
//     bazel build -c opt --config=asan //lib/cache:html_cache_policy_fuzz
//
// One source, two build modes (mirrors lib/css/css_minify_fuzz.cc):
//   * default                       -> deterministic main() that replays the
//                                      embedded seeds and then every file
//                                      named on the command line.  Hermetic;
//                                      CI-friendly.  `--dump_corpus=DIR`
//                                      instead writes the embedded seed set
//                                      as seed-NN files into DIR (created if
//                                      missing) and exits.
//   * -DHTML_CACHE_POLICY_LIBFUZZER -> exposes LLVMFuzzerTestOneInput for
//                                      coverage-guided discovery when linked
//                                      with -fsanitize=fuzzer.
// A third mode, -DHTML_CACHE_POLICY_FUZZ_NO_MAIN, emits neither entry point:
// it is how the seed-replay test links the same oracle without libFuzzer.
//
// INPUT FORMAT.  One blob drives all the parsers.  It is split at the first
// blank line (`\n\n`) into a header block and a body prefix.  The header block
// is lines of `name: value` (a trailing CR is dropped; a line without a colon
// or with an empty name is dropped).  A line whose name starts with `:` is a
// harness control line, never a response header: `:method`, `:path`,
// `:status`, `:authorization`, `:authenticated`, `:cookie`, `:incomplete`,
// `:body-bytes` and `:now` set the request, status, body facts and receipt
// time that a header block cannot carry.
//
// WHAT RUNS.  (1) ClassifyHtmlForStore over the parsed request, headers and
// body prefix.  (2) FormatHtmlCacheDecisionLine over the header lines taken
// as key/value pairs (unknown keys are expected to be refused, not to crash)
// and then over a canonical field set built from the store verdict.  (3)
// ParseHttpDateSeconds over every header value.  (4) ClassifyHtmlForServe,
// in every mode, over facts derived from the store verdict.
//
// ORACLES.  Crash-free, with the sanitizer as the memory-safety backstop, and
// these invariants, each of which aborts on violation:
//   * a verdict that stores has an explicit lifetime or a validator, no
//     private/no-store flag, and only Accept-Encoding in its Vary tokens;
//   * a verdict's reason and detail are in the closed, printable set;
//   * no formatted line contains a line break, and a canonical field set
//     always formats;
//   * a stale entry is never served, in any mode, and report-only never
//     enforces.

#include "lib/cache/html_cache_policy_fuzz.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/cache/html_cache_decision.h"
#include "lib/cache/html_cache_policy.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed::fuzz {
namespace {

[[noreturn]] void Violation(const char* what) {
  std::fprintf(stderr, "html cache policy fuzz oracle violation: %s\n", what);
  std::abort();
}

struct ParsedBlob {
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string method = "GET";
  std::string path = "/index.html";
  int status = 200;
  bool authorization = false;
  bool authenticated = false;
  bool cookie = false;
  bool incomplete = false;
  uint64_t body_bytes = 1000;
  uint32_t now = 784111777;
};

uint64_t ParseUnsigned(std::string_view v) {
  uint64_t out = 0;
  for (char c : v) {
    if (c < '0' || c > '9') break;
    if (out > (UINT64_MAX - 9) / 10) return UINT64_MAX;
    out = out * 10 + static_cast<uint64_t>(c - '0');
  }
  return out;
}

ParsedBlob ParseBlob(std::string_view blob) {
  ParsedBlob parsed;
  std::string_view head = blob;
  const size_t split = blob.find("\n\n");
  if (split != std::string_view::npos) {
    head = blob.substr(0, split);
    parsed.body = std::string(blob.substr(split + 2));
  }
  while (!head.empty()) {
    const size_t eol = head.find('\n');
    std::string_view line = head.substr(0, eol);
    head = eol == std::string_view::npos ? std::string_view()
                                         : head.substr(eol + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const bool control = !line.empty() && line.front() == ':';
    const size_t colon = line.find(':', control ? 1 : 0);
    if (colon == std::string_view::npos || colon == 0 ||
        (control && colon == 1)) {
      continue;
    }
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    if (!control) {
      parsed.headers.emplace_back(std::string(name), std::string(value));
    } else if (name == ":method") {
      parsed.method = std::string(value);
    } else if (name == ":path") {
      parsed.path = std::string(value);
    } else if (name == ":status") {
      parsed.status = static_cast<int>(ParseUnsigned(value) & 0xFFFF);
    } else if (name == ":authorization") {
      parsed.authorization = true;
    } else if (name == ":authenticated") {
      parsed.authenticated = true;
    } else if (name == ":cookie") {
      parsed.cookie = true;
    } else if (name == ":incomplete") {
      parsed.incomplete = true;
    } else if (name == ":body-bytes") {
      parsed.body_bytes = ParseUnsigned(value);
    } else if (name == ":now") {
      parsed.now = static_cast<uint32_t>(ParseUnsigned(value));
    }
  }
  return parsed;
}

HtmlStoreVerdict Classify(const ParsedBlob& parsed) {
  std::vector<HeaderField> fields;
  fields.reserve(parsed.headers.size());
  for (const auto& [name, value] : parsed.headers) {
    fields.push_back({name, value});
  }
  HtmlStoreRequest request;
  request.method = parsed.method;
  request.url_path = parsed.path;
  request.has_authorization = parsed.authorization;
  request.authenticated_user = parsed.authenticated;
  request.has_cookie = parsed.cookie;
  HtmlStoreResponse response;
  response.status = parsed.status;
  response.headers = fields;
  response.body_bytes = parsed.body_bytes;
  response.body_complete = !parsed.incomplete;
  response.body_prefix = parsed.body;
  HtmlStorePolicy policy;
  policy.now_seconds = parsed.now;
  return ClassifyHtmlForStore(request, response, policy);
}

void CheckStoreVerdict(const HtmlStoreVerdict& v) {
  if (static_cast<uint8_t>(v.reason) >= kHtmlStoreReasonCount) {
    Violation("store reason out of range");
  }
  if (v.detail.size() > kMaxHtmlStoreDetailBytes) Violation("detail too long");
  for (char c : v.detail) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x21 || u > 0x7E) Violation("detail not printable ASCII");
  }
  if (v.stores()) {
    if (!v.lifetime_explicit && !v.has_validators) {
      Violation("stored with neither a lifetime nor a validator");
    }
    if (v.needs_validation && !v.has_validators) {
      Violation("stored a must-revalidate response with no validator");
    }
    if ((v.cc_flags & (AlternateMetadata::kCCOriginPrivate |
                       AlternateMetadata::kCCOriginNoStore)) != 0) {
      Violation("stored a private or no-store response");
    }
    for (const std::string& token : v.vary_tokens) {
      if (token != "accept-encoding") Violation("stored a varying response");
    }
    if (!v.sidecar.swap_eligible()) Violation("stored a non-reproducible set");
  }
}

// A stored-entry view of a store verdict, aged past its lifetime.
void CheckServe(const HtmlStoreVerdict& v) {
  StoredHtmlFacts facts;
  facts.present = true;
  facts.stored_at = 1000;
  facts.lifetime_s = v.lifetime_s;
  facts.needs_validation = v.needs_validation;
  facts.explicit_shared = v.explicit_shared;
  facts.cc_flags = v.cc_flags;
  facts.variant_present = true;
  facts.binding_live = true;
  for (HtmlCacheMode mode :
       {HtmlCacheMode::kOff, HtmlCacheMode::kReportOnly,
        HtmlCacheMode::kServeVariants, HtmlCacheMode::kServeCachedHtml}) {
    for (bool markdown : {false, true}) {
      HtmlServeRequest request;
      request.mode = mode;
      request.wants_markdown = markdown;
      // One second past expiry (or simply later, for lifetime 0).
      request.now_seconds = facts.stored_at + v.lifetime_s + 1;
      const HtmlServeVerdict stale = ClassifyHtmlForServe(request, facts);
      if (stale.action == HtmlServeAction::kServeHtml ||
          stale.action == HtmlServeAction::kServeMarkdown) {
        Violation("served a stale entry");
      }
      if ((mode == HtmlCacheMode::kOff || mode == HtmlCacheMode::kReportOnly) &&
          stale.enforce) {
        Violation("a non-enforcing mode enforced");
      }
      request.now_seconds = facts.stored_at;
      const HtmlServeVerdict fresh = ClassifyHtmlForServe(request, facts);
      if (static_cast<uint8_t>(fresh.reason) >= kHtmlServeReasonCount) {
        Violation("serve reason out of range");
      }
      if ((mode == HtmlCacheMode::kOff || mode == HtmlCacheMode::kReportOnly) &&
          fresh.enforce) {
        Violation("a non-enforcing mode enforced");
      }
    }
  }
}

void CheckLine(const std::optional<std::string>& line) {
  if (!line.has_value()) return;
  if (line->empty() || line->front() != '{' || line->back() != '}') {
    Violation("decision line is not an object");
  }
  if (line->find_first_of("\r\n") != std::string::npos) {
    Violation("decision line contains a line break");
  }
}

void FormatOne(const ParsedBlob& parsed, const HtmlStoreVerdict& v) {
  // The header lines as key/value pairs: mostly unknown keys, which must be
  // refused cleanly.
  std::vector<HtmlCacheDecisionField> raw;
  for (const auto& [name, value] : parsed.headers) raw.push_back({name, value});
  CheckLine(FormatHtmlCacheDecisionLine(raw));
  (void)HtmlCacheDecisionDedupKey(raw);

  // The canonical field set for this verdict: must always format.
  const std::string status = std::to_string(parsed.status);
  const std::string size = std::to_string(parsed.body_bytes);
  const std::string lifetime = std::to_string(v.lifetime_s);
  const std::string reason = v.reason_code();
  const std::string before = std::to_string(HtmlCacheFnv1a64(parsed.body));
  const std::string cc = std::to_string(v.cc_flags);
  std::string vary;
  for (const std::string& token : v.vary_tokens) {
    if (!vary.empty()) vary.push_back(',');
    // Vary tokens are origin-controlled and may carry any byte but LF; a
    // caller that logs them replaces the ones the formatter refuses.
    for (char c : token) {
      vary.push_back(c == '\r' || c == '\n' || c == '\0' ? '?' : c);
    }
  }
  const std::vector<HtmlCacheDecisionField> canonical = {
      {"ts_ms", "1700000000000"},
      {"host", "fuzz.example"},
      {"path", std::string_view(parsed.path).substr(0, 300)},
      {"mode", HtmlCacheModeName(HtmlCacheMode::kReportOnly)},
      {"action", HtmlStoreActionName(v.action)},
      {"reason", reason},
      {"status", status},
      {"size", size},
      {"lifetime_s", lifetime},
      {"cc", cc},
      {"vary", vary},
      {"personalisation", v.reason == HtmlStoreReason::kPersonalisationSignal
                              ? std::string_view(v.detail)
                              : std::string_view()},
      {"before", before},
  };
  const std::optional<std::string> line =
      FormatHtmlCacheDecisionLine(canonical);
  // A path with a line break is the one thing a canonical set can carry that
  // the formatter refuses; everything else must format.
  const bool path_ok = parsed.path.find_first_of(
                           std::string_view("\r\n\0", 3)) == std::string::npos;
  if (path_ok && !line.has_value()) {
    Violation("canonical decision fields were refused");
  }
  CheckLine(line);
  (void)HtmlCacheDecisionDedupKey(canonical);
}

std::string Blob(std::initializer_list<std::string_view> lines,
                 std::string_view body = "<html><body>ok</body></html>") {
  std::string out;
  for (std::string_view line : lines) {
    out.append(line);
    out.push_back('\n');
  }
  out.push_back('\n');
  out.append(body);
  return out;
}

std::vector<std::string> BuildSeeds() {
  constexpr std::string_view kType = "Content-Type: text/html; charset=utf-8";
  constexpr std::string_view kAge = "Cache-Control: max-age=300";
  std::vector<std::string> seeds;
  // A storable response.
  seeds.push_back(Blob({kType, kAge, "ETag: \"v1\""}));
  // One per refusal reason, in rule order.
  seeds.push_back(Blob({":method: HEAD", kType, kAge}));
  seeds.push_back(Blob({":status: 404", kType, kAge}));
  seeds.push_back(Blob({":incomplete: 1", kType, kAge}));
  seeds.push_back(Blob({kType, kAge, "Content-Range: bytes 0-9/100"}));
  seeds.push_back(Blob({":path: /search?q=1", kType, kAge}));
  seeds.push_back(Blob({"Content-Type: application/xhtml+xml", kAge}));
  seeds.push_back(Blob({kType, kAge, "Content-Encoding: gzip"}));
  seeds.push_back(Blob({":authorization: 1", kType, kAge}));
  seeds.push_back(Blob({kType, kAge, "Set-Cookie: sid=1; HttpOnly"}));
  seeds.push_back(
      Blob({kType, "Cache-Control: private=\"Set-Cookie\", max-age=60"}));
  seeds.push_back(Blob({kType, "Cache-Control: no-store"}));
  seeds.push_back(Blob({":cookie: 1", kType, kAge}));
  seeds.push_back(Blob({kType, kAge, "Vary: *"}));
  seeds.push_back(Blob({kType, kAge, "Vary: Accept-Encoding, Cookie"}));
  seeds.push_back(Blob({kType, kAge, "X-Powered-By: PHP/8"}));
  seeds.push_back(Blob(
      {kType, kAge, "Content-Security-Policy: script-src 'nonce-r4nd0m'"}));
  seeds.push_back(Blob({kType}));
  seeds.push_back(Blob({kType, "Cache-Control: no-cache"}));
  seeds.push_back(Blob({":body-bytes: 99999999999", kType, kAge}));
  // Lifetime sources.
  seeds.push_back(Blob({kType, "Date: Sun, 06 Nov 1994 08:49:37 GMT",
                        "Expires: Sun, 06 Nov 1994 08:50:37 GMT"}));
  seeds.push_back(Blob(
      {kType, "Expires: 0", "Last-Modified: Sun, 06 Nov 1994 08:49:37 GMT"}));
  seeds.push_back(
      Blob({kType, "Cache-Control: s-maxage=60, max-age=0", "ETag: W/\"x\""}));
  seeds.push_back(Blob({kType, "Cache-Control: max-age=60, no-transform"}));
  // One per personalisation signal.
  for (std::string_view body : {
           "<meta name=\"csrf-token\" content=\"x\">",
           "<meta name=\"csrf-param\" content=\"y\">",
           "<input type=\"hidden\" name=\"_token\" value=\"x\">",
           "<input type=\"hidden\" name=\"csrf_value\" value=\"x\">",
           "<input type=\"hidden\" name=\"authenticity_token\" value=\"x\">",
           "<input type=\"hidden\" name=\"__RequestVerificationToken\">",
           "<input name=\"csrfmiddlewaretoken\" type=\"hidden\" value=\"x\">",
           "<form method=\"post\" action=\"/account/login\">",
           "<FORM METHOD=POST ACTION=/shop/Checkout>",
       }) {
    seeds.push_back(Blob({kType, kAge}, body));
  }
  // Hostile shapes.
  seeds.push_back(Blob({kType, kAge, "X-Big: " + std::string(70000, 'a')}));
  seeds.push_back(Blob({kType, kAge, std::string(300, 'N') + ": v"}));
  seeds.push_back(Blob({kType, kAge}, std::string(70000, '<')));
  seeds.push_back(
      std::string("Date: Sun, 06 Nov 1994 08:49:37 GMT\r\nExpires:\tMon, 07 "
                  "Nov 1994 08:49:37 GMT\r\n\r\n"));
  seeds.push_back(
      std::string("\x01\x02: \xff\xfe\n:status\n: x\n::\nhost: \n\n"));
  seeds.push_back(std::string());
  return seeds;
}

}  // namespace

HtmlStoreVerdict ClassifyHtmlCachePolicyFuzzBlob(std::string_view blob) {
  return Classify(ParseBlob(blob));
}

void HtmlCachePolicyFuzzOne(std::string_view blob) {
  const ParsedBlob parsed = ParseBlob(blob);
  const HtmlStoreVerdict verdict = Classify(parsed);
  CheckStoreVerdict(verdict);
  CheckServe(verdict);
  FormatOne(parsed, verdict);
  for (const auto& [name, value] : parsed.headers) {
    (void)ParseHttpDateSeconds(value);
  }
  if (!parsed.headers.empty())
    (void)ParseHttpDateSeconds(parsed.headers[0].second);
  (void)FindPersonalisationSignal(parsed.body);
}

const std::vector<std::string>& HtmlCachePolicyFuzzSeeds() {
  static const std::vector<std::string> seeds = BuildSeeds();
  return seeds;
}

int DumpHtmlCachePolicyFuzzCorpus(const std::string& dir) {
  // Create the directory ourselves: a corpus-seeding step should not depend
  // on the caller having made it.
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "dump_corpus: cannot create %s: %s\n", dir.c_str(),
                 ec.message().c_str());
    return 1;
  }
  size_t written = 0;
  for (const std::string& seed : HtmlCachePolicyFuzzSeeds()) {
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/seed-%02zu", dir.c_str(), written);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
      std::fprintf(stderr, "dump_corpus: cannot write %s\n", path);
      return 1;
    }
    if (!seed.empty() &&
        std::fwrite(seed.data(), 1, seed.size(), f) != seed.size()) {
      std::fprintf(stderr, "dump_corpus: short write on %s\n", path);
      std::fclose(f);
      return 1;
    }
    if (std::fclose(f) != 0) {
      std::fprintf(stderr, "dump_corpus: close failed on %s\n", path);
      return 1;
    }
    ++written;
  }
  std::fprintf(stderr, "dump_corpus: %zu seeds written to %s\n", written,
               dir.c_str());
  return 0;
}

}  // namespace pagespeed::fuzz

#if defined(HTML_CACHE_POLICY_FUZZ_NO_MAIN)
// Linked into the seed-replay test, which supplies its own entry point.
#elif defined(HTML_CACHE_POLICY_LIBFUZZER)
// Coverage-guided entry point.  Link with -fsanitize=fuzzer,address.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  pagespeed::fuzz::HtmlCachePolicyFuzzOne(
      std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
#else
int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view arg1(argv[1]);
    constexpr std::string_view kDumpPrefix = "--dump_corpus=";
    if (arg1.substr(0, kDumpPrefix.size()) == kDumpPrefix) {
      return pagespeed::fuzz::DumpHtmlCachePolicyFuzzCorpus(
          std::string(arg1.substr(kDumpPrefix.size())));
    }
  }
  size_t n = 0;
  for (const std::string& seed : pagespeed::fuzz::HtmlCachePolicyFuzzSeeds()) {
    pagespeed::fuzz::HtmlCachePolicyFuzzOne(seed);
    ++n;
  }
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "skip: cannot open %s\n", argv[i]);
      continue;
    }
    std::string buf;
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.append(chunk, got);
    }
    std::fclose(f);
    pagespeed::fuzz::HtmlCachePolicyFuzzOne(buf);
    ++n;
  }
  std::fprintf(stderr,
               "html cache policy fuzz harness: %zu inputs processed clean\n",
               n);
  return 0;
}
#endif
