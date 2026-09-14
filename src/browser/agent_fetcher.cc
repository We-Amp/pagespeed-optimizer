// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/browser/agent_fetcher.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#else
// clang-format off
// winsock2.h MUST precede windows.h (else the legacy winsock.h pulled by
// windows.h collides with winsock2). Keep this order — clang-format would
// otherwise sort windows.h first. Needed for getaddrinfo / inet_ntop /
// inet_addr and the CreateProcess curl spawn below.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on
#pragma comment(lib, "ws2_32.lib")
#endif

#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "lib/net/curl_fetcher.h"
#include "lib/net/fetch_policy.h"
#include "lib/net/ssrf_guard.h"
#include "lib/net/upstream_pin.h"

#ifndef _WIN32
extern char** environ;
#endif

namespace pagespeed {

namespace {

// Strip a single trailing '\r' (so "\r\n"-delimited header lines parse cleanly).
std::string_view StripCr(std::string_view s) {
  if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
  return s;
}

// Parse the HTTP status code out of a status line ("HTTP/1.1 200 OK" / "HTTP/2 204").
// Rejects anything that does not start with "HTTP/" or lacks a 3-digit code.
std::optional<int> ParseStatusCode(std::string_view status_line) {
  if (!absl::StartsWith(status_line, "HTTP/")) return std::nullopt;
  std::size_t sp = status_line.find(' ');
  if (sp == std::string_view::npos) return std::nullopt;
  std::size_t i = sp;
  while (i < status_line.size() && status_line[i] == ' ') ++i;
  if (i + 3 > status_line.size()) return std::nullopt;
  int code = 0;
  for (int k = 0; k < 3; ++k) {
    char c = status_line[i + k];
    if (c < '0' || c > '9') return std::nullopt;
    code = code * 10 + (c - '0');
  }
  // The character after the 3 digits must be a space or end-of-line (reject "2000").
  std::size_t after = i + 3;
  if (after < status_line.size() && status_line[after] != ' ')
    return std::nullopt;
  return code;
}

// A forwardable header NAME must be a non-empty RFC7230 token (no controls, no
// separators) — anything else can corrupt the response Chrome synthesizes from our
// fulfill. Conservative: any rejection just drops the header (fail-safe).
bool IsTokenName(std::string_view name) {
  if (name.empty()) return false;
  for (unsigned char c : name) {
    if (c <= 0x20 || c >= 0x7f) return false;
    switch (c) {  // RFC7230 separators
      case '(':
      case ')':
      case '<':
      case '>':
      case '@':
      case ',':
      case ';':
      case ':':
      case '\\':
      case '"':
      case '/':
      case '[':
      case ']':
      case '?':
      case '=':
      case '{':
      case '}':
        return false;
      default:
        break;
    }
  }
  return true;
}

// A forwardable header VALUE must carry no control bytes (CR/LF/NUL/other C0 or
// DEL) that could split/corrupt the synthesized response. HTAB is permitted.
bool IsCleanValue(std::string_view value) {
  for (unsigned char c : value) {
    if (c == '\t') continue;
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

}  // namespace

std::optional<ParsedHttpResponse> ParseCurlResponse(std::string_view raw) {
  std::string_view rest = raw;
  // Loop only to skip leading 1xx interim blocks (100 Continue / 103 Early Hints).
  for (;;) {
    std::size_t pos_crlf = rest.find("\r\n\r\n");
    std::size_t pos_lf = rest.find("\n\n");
    std::size_t hdr_end;
    std::size_t sep_len;
    if (pos_crlf == std::string_view::npos &&
        pos_lf == std::string_view::npos) {
      return std::nullopt;  // no header terminator => malformed (fail-closed)
    }
    if (pos_crlf != std::string_view::npos &&
        (pos_lf == std::string_view::npos || pos_crlf <= pos_lf)) {
      hdr_end = pos_crlf;
      sep_len = 4;
    } else {
      hdr_end = pos_lf;
      sep_len = 2;
    }
    std::string_view header_block = rest.substr(0, hdr_end);
    std::string_view body = rest.substr(hdr_end + sep_len);

    std::vector<std::string_view> lines = absl::StrSplit(header_block, '\n');
    if (lines.empty()) return std::nullopt;

    std::optional<int> status = ParseStatusCode(StripCr(lines.front()));
    if (!status.has_value()) return std::nullopt;

    if (*status >= 100 && *status < 200) {
      rest = body;  // interim response — parse the next block
      continue;
    }

    ParsedHttpResponse resp;
    resp.status = *status;
    resp.body = std::string(body);
    for (std::size_t i = 1; i < lines.size(); ++i) {
      std::string_view line = StripCr(lines[i]);
      if (line.empty()) continue;
      std::size_t colon = line.find(':');
      if (colon == std::string_view::npos) continue;  // not a header line
      std::string_view name = absl::StripAsciiWhitespace(line.substr(0, colon));
      std::string_view value =
          absl::StripAsciiWhitespace(line.substr(colon + 1));
      if (name.empty()) continue;
      resp.headers.push_back({std::string(name), std::string(value)});
      if (resp.location.empty() && absl::EqualsIgnoreCase(name, "location")) {
        resp.location = std::string(value);
      }
    }
    return resp;
  }
}

std::vector<HttpHeader> SanitizeResponseHeaders(
    const std::vector<HttpHeader>& headers) {
  // Hop-by-hop, framing, integrity/encoding, and stateful headers that must NOT be
  // forwarded verbatim into Fetch.fulfillRequest. The fetcher serves the literal
  // bytes curl returned and applied NO transform, so any header describing a
  // transform/length/integrity it did not perform — transfer-encoding,
  // content-encoding, content-length, content-md5, content-range — must be dropped,
  // or Chrome would re-decode/re-frame attacker-chosen bytes (compression bomb /
  // framing desync). Set-Cookie is dropped for the anonymous view. (P1.4 review SF-1
  // + the existing hop-by-hop set.)
  static constexpr std::string_view kStrip[] = {
      "connection",
      "keep-alive",
      "proxy-connection",
      "transfer-encoding",
      "te",
      "trailer",
      "upgrade",
      "content-length",
      "content-encoding",
      "content-md5",
      "content-range",
      "set-cookie",
      "set-cookie2",
  };
  std::vector<HttpHeader> out;
  out.reserve(headers.size());
  for (const HttpHeader& h : headers) {
    std::string lname =
        absl::AsciiStrToLower(absl::StripAsciiWhitespace(h.name));
    // SF-3: drop any header with a non-token name or a control-bearing value — it
    // could corrupt/split the response Chrome synthesizes from our fulfill.
    if (!IsTokenName(lname) || !IsCleanValue(h.value)) continue;
    bool strip = false;
    for (std::string_view s : kStrip) {
      if (lname == s) {
        strip = true;
        break;
      }
    }
    if (!strip) out.push_back(h);
  }
  return out;
}

std::string ResolveRedirectTarget(std::string_view base_url,
                                  std::string_view location) {
  std::string_view loc = absl::StripAsciiWhitespace(location);
  if (loc.empty()) return "";

  std::string result;
  if (absl::StartsWithIgnoreCase(loc, "http://") ||
      absl::StartsWithIgnoreCase(loc, "https://")) {
    result = std::string(loc);  // absolute
  } else {
    // Reconstruct the base scheme + authority span verbatim from base_url.
    std::size_t scheme_sep = base_url.find("://");
    if (scheme_sep == std::string_view::npos) return "";
    std::string_view scheme = base_url.substr(0, scheme_sep);
    std::size_t authority_start = scheme_sep + 3;
    std::size_t authority_end = base_url.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos)
      authority_end = base_url.size();
    std::string_view origin = base_url.substr(0, authority_end);

    if (absl::StartsWith(loc, "//")) {
      result = absl::StrCat(scheme, ":", loc);  // scheme-relative //host/path
    } else if (absl::StartsWith(loc, "/")) {
      result = absl::StrCat(origin, loc);  // root-relative /path
    } else if (absl::StartsWith(loc, "#")) {
      return "";  // fragment-only — not a navigation we follow
    } else {
      // Query-only (?q) or path-relative (foo, ../foo): resolve against the base
      // path's directory. The host is unchanged, and the result is re-adjudicated.
      std::string_view path = base_url.substr(authority_end);
      std::size_t cut = path.find_first_of("?#");
      if (cut != std::string_view::npos) path = path.substr(0, cut);
      if (absl::StartsWith(loc, "?")) {
        result = absl::StrCat(origin, path, loc);
      } else {
        std::size_t last_slash = path.rfind('/');
        std::string_view dir = (last_slash == std::string_view::npos)
                                   ? std::string_view("/")
                                   : path.substr(0, last_slash + 1);
        if (dir.empty()) dir = "/";
        result = absl::StrCat(origin, dir, loc);
      }
    }
  }

  // Accept only a result that re-parses as an http(s) absolute URL. (The full
  // G1/G2 gate re-runs on it anyway; this just rejects junk early, fail-closed.)
  std::optional<UrlAuthority> auth = ParseHttpUrl(result);
  if (!auth.has_value() || !IsAllowedScheme(auth->scheme)) return "";
  return result;
}

std::vector<std::string> BuildScrubbedCurlEnv(
    const std::vector<std::string>& source_env) {
  // Allowlist (NOT a denylist): only these may reach curl. Everything else —
  // HOME, CURL_HOME, NETRC, http_proxy/https_proxy/all_proxy, CURLOPT_* — is
  // dropped so the host environment can neither change curl's behavior nor tunnel
  // the request past the IP pin. PATH lets posix_spawnp find curl; the CA vars keep
  // TLS verification working against the system trust store.
  static constexpr std::string_view kAllow[] = {
      "PATH",
      "SSL_CERT_FILE",
      "SSL_CERT_DIR",
      "CURL_CA_BUNDLE",
  };
  std::vector<std::string> out;
  for (const std::string& entry : source_env) {
    std::size_t eq = entry.find('=');
    if (eq == std::string::npos) continue;
    std::string_view key(entry.data(), eq);
    for (std::string_view a : kAllow) {
      if (key == a) {
        out.push_back(entry);
        break;
      }
    }
  }
  return out;
}

AgentFetchOutcome FetchSubresource(std::string_view url_in, ResourceClass rc,
                                   const FetchPolicy& policy,
                                   const AgentFetcherDeps& deps) {
  auto deny = [](FetchAction action, std::string_view why) {
    AgentFetchOutcome o;
    o.action = action;
    o.fulfilled = false;
    o.diagnostic = std::string(why);
    return o;
  };
  if (!deps.spawn || !deps.resolve) {
    return deny(FetchAction::kDenyBlockedByClient, "fetcher misconfigured");
  }

  std::string url(url_in);
  for (int hop = 0;; ++hop) {
    if (hop > deps.max_redirect_hops) {
      return deny(FetchAction::kDenyBlockedByClient, "too many redirects");
    }

    // Re-adjudicate policy on EVERY hop (the keystone: a redirect target is a fresh
    // request and must clear G1/G2 on its own merits).
    FetchAction action = DecideFetch(url, rc, policy);
    if (action == FetchAction::kDenyKeepElement) {
      return deny(FetchAction::kDenyKeepElement, "keep-element (image/media)");
    }
    if (action != FetchAction::kProxyUpstream &&
        action != FetchAction::kFetchAllowlisted) {
      return deny(FetchAction::kDenyBlockedByClient, "policy deny");
    }

    std::optional<UrlAuthority> auth = ParseHttpUrl(url);
    if (!auth.has_value()) {
      return deny(FetchAction::kDenyBlockedByClient, "unparseable url");
    }
    // Per-hop scheme floor for trust-anchor fetches: every hop (initial URL
    // and each redirect target) must be https, or the whole fetch is denied
    // before any egress. ParseHttpUrl lower-cases the scheme.
    if (deps.require_https && auth->scheme != "https") {
      return deny(FetchAction::kDenyBlockedByClient, "https required");
    }
    if (auth->port <= 0 || auth->port > 65535) {
      return deny(FetchAction::kDenyBlockedByClient, "bad port");
    }

    std::vector<std::string> addrs;
    if (auth->host_is_ip_literal) {
      addrs = {auth->host};
    } else {
      addrs = deps.resolve(auth->host);
    }
    if (addrs.empty()) {
      return deny(FetchAction::kDenyBlockedByClient, "dns resolve failed");
    }

    std::string pinned_ip;
    if (action == FetchAction::kFetchAllowlisted) {
      // G2: EVERY resolved address must be public (defeats a rebind to a private
      // target); pin a validated public IP.
      if (!IsPublicUrl(url, addrs)) {
        return deny(FetchAction::kDenyBlockedByClient,
                    "ssrf: non-public address");
      }
      pinned_ip = auth->host_is_ip_literal ? auth->host : addrs.front();
    } else {
      // G1: the host byte-equals the configured upstream (loopback/RFC1918 backends
      // are allowed by config — no public requirement). But NEVER pin a link-local /
      // cloud-metadata address (169.254.0.0/16, fe80::/10) even for the render's own
      // origin: a hostname that (mis)resolves there — split-horizon or attacker DNS
      // for their own rendered domain — is never a legitimate backend and would
      // exfiltrate the host's cloud-credential endpoint.
      // Reject if ANY resolved address is link-local, not just the pinned one
      // (matches G2's all-addresses stance and is robust to DNS reordering or
      // future code that tries fallback addresses).
      for (const std::string& a : addrs) {
        if (IsLinkLocalIp(a)) {
          return deny(FetchAction::kDenyBlockedByClient,
                      "g1: link-local/metadata ip");
        }
      }
      pinned_ip = auth->host_is_ip_literal ? auth->host : addrs.front();
    }

    std::vector<std::string> argv = BuildPinnedGetArgv(
        url, auth->host, auth->port, pinned_ip, deps.curl_opts);
    if (argv.empty()) {
      return deny(FetchAction::kDenyBlockedByClient, "argv build failed");
    }

    CurlSpawnOutcome spawned =
        deps.spawn(argv, deps.curl_opts.max_response_bytes);
    if (!spawned.spawned || !spawned.exit_code.has_value() ||
        *spawned.exit_code != 0) {
      return deny(FetchAction::kDenyBlockedByClient, "curl transport failure");
    }

    std::optional<ParsedHttpResponse> resp = ParseCurlResponse(spawned.output);
    if (!resp.has_value()) {
      return deny(FetchAction::kDenyBlockedByClient, "unparseable response");
    }

    if (resp->status >= 300 && resp->status < 400 && !resp->location.empty()) {
      std::string next = ResolveRedirectTarget(url, resp->location);
      if (next.empty()) {
        return deny(FetchAction::kDenyBlockedByClient, "unresolvable redirect");
      }
      url = std::move(next);
      continue;  // re-adjudicate the new hop from the top
    }

    AgentFetchOutcome out;
    out.action = action;
    out.fulfilled = true;
    out.status = resp->status;
    out.headers = SanitizeResponseHeaders(resp->headers);
    out.body = std::move(resp->body);
    out.final_url = url;
    return out;
  }
}

// --------------------------------------------------------------------------
// Production effects. POSIX spawns curl via posix_spawn; Windows via
// CreateProcess (below). DNS resolution uses getaddrinfo on both. The IP-pin
// SSRF defense lives in the curl argv, so it is identical across platforms.
// --------------------------------------------------------------------------

#ifndef _WIN32

namespace {

CurlSpawnOutcome PosixCurlSpawn(const std::vector<std::string>& argv,
                                std::size_t max_bytes) {
  CurlSpawnOutcome out;
  if (argv.empty()) {
    out.error = "empty argv";
    return out;
  }

  int pipefd[2];
  if (pipe(pipefd) < 0) {
    out.error = "pipe() failed";
    return out;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);

  std::vector<char*> c_argv;
  c_argv.reserve(argv.size() + 1);
  for (const std::string& a : argv) {
    c_argv.push_back(const_cast<char*>(a.c_str()));
  }
  c_argv.push_back(nullptr);

  // Minimal, scrubbed environment (H2 + proxy-pin defense in depth).
  std::vector<std::string> source_env;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    source_env.emplace_back(*e);
  }
  std::vector<std::string> scrubbed = BuildScrubbedCurlEnv(source_env);
  std::vector<char*> c_env;
  c_env.reserve(scrubbed.size() + 1);
  for (std::string& s : scrubbed) {
    c_env.push_back(const_cast<char*>(s.c_str()));
  }
  c_env.push_back(nullptr);

  pid_t pid = -1;
  int r = posix_spawnp(&pid, "curl", &actions, nullptr, c_argv.data(),
                       c_env.data());
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);

  if (r != 0) {
    close(pipefd[0]);
    out.error = "failed to spawn curl";
    return out;
  }
  out.spawned = true;

  char buf[8192];
  ssize_t n;
  bool oversize = false;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
    if (out.output.size() + static_cast<std::size_t>(n) > max_bytes) {
      oversize = true;
      break;  // do not append the overflowing chunk
    }
    out.output.append(buf, static_cast<std::size_t>(n));
  }
  close(pipefd[0]);

  if (oversize) {
    kill(pid, SIGKILL);
  }
  int wstatus = 0;
  waitpid(pid, &wstatus, 0);

  if (oversize) {
    out.error = "response too large";
    // exit_code left unset => the caller treats this as did-not-complete (deny).
    return out;
  }
  out.exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
  if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
    out.error = "curl non-zero exit";
  }
  return out;
}

std::vector<std::string> PosixResolve(std::string_view host) {
  std::vector<std::string> out;
  std::string host_str(host);

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = nullptr;
  int rc = getaddrinfo(host_str.c_str(), nullptr, &hints, &res);
  if (rc != 0 || res == nullptr) {
    if (res != nullptr) freeaddrinfo(res);
    return out;
  }
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    char buf[INET6_ADDRSTRLEN];
    buf[0] = '\0';
    const void* addr_ptr = nullptr;
    if (ai->ai_family == AF_INET) {
      addr_ptr = &reinterpret_cast<struct sockaddr_in*>(ai->ai_addr)->sin_addr;
    } else if (ai->ai_family == AF_INET6) {
      addr_ptr =
          &reinterpret_cast<struct sockaddr_in6*>(ai->ai_addr)->sin6_addr;
    } else {
      continue;
    }
    if (inet_ntop(ai->ai_family, addr_ptr, buf, sizeof(buf)) != nullptr) {
      out.emplace_back(buf);
    }
  }
  freeaddrinfo(res);
  return out;
}

}  // namespace

SpawnCurlFn RealCurlSpawn() { return PosixCurlSpawn; }
ResolveHostFn RealHostResolver() { return PosixResolve; }

#else  // _WIN32

namespace {

// Ensure Winsock is initialized exactly once before getaddrinfo. libuv usually
// does this already, but the fetcher must not depend on call order; WSAStartup
// is refcounted, so a one-time init here is safe.
void EnsureWinsock() {
  static const bool kInit = [] {
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
  }();
  (void)kInit;
}

// Windows DNS resolution — same getaddrinfo logic as the POSIX path. Winsock's
// getaddrinfo / inet_ntop live in ws2_32 and accept the identical struct shapes.
std::vector<std::string> WindowsResolve(std::string_view host) {
  EnsureWinsock();
  std::vector<std::string> out;
  std::string host_str(host);

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = nullptr;
  if (getaddrinfo(host_str.c_str(), nullptr, &hints, &res) != 0 ||
      res == nullptr) {
    if (res != nullptr) freeaddrinfo(res);
    return out;
  }
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    char buf[INET6_ADDRSTRLEN];
    buf[0] = '\0';
    const void* addr_ptr = nullptr;
    if (ai->ai_family == AF_INET) {
      addr_ptr = &reinterpret_cast<struct sockaddr_in*>(ai->ai_addr)->sin_addr;
    } else if (ai->ai_family == AF_INET6) {
      addr_ptr =
          &reinterpret_cast<struct sockaddr_in6*>(ai->ai_addr)->sin6_addr;
    } else {
      continue;
    }
    if (inet_ntop(ai->ai_family, addr_ptr, buf, sizeof(buf)) != nullptr) {
      out.emplace_back(buf);
    }
  }
  freeaddrinfo(res);
  return out;
}

// Quote one argv element using the MSVC / CommandLineToArgvW rules so the
// pinned curl argv (the --resolve IP pin, --noproxy "*") reaches curl.exe
// byte-for-byte — the SSRF defense lives in these args and must survive intact.
std::string WinQuoteArg(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
    return arg;
  }
  std::string out = "\"";
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');  // before closing quote: escape all
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');  // escape backslashes + the quote
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}

// CreateProcess-based curl spawn — the Windows analogue of PosixCurlSpawn.
// stdout is captured through an anonymous pipe and capped at max_bytes; the
// child is killed if it overflows. The scrubbed environment (proxy-pin defense
// in depth) mirrors POSIX, and the IP pin is carried verbatim in the argv.
CurlSpawnOutcome WindowsCurlSpawn(const std::vector<std::string>& argv,
                                  std::size_t max_bytes) {
  CurlSpawnOutcome out;
  if (argv.empty()) {
    out.error = "empty argv";
    return out;
  }

  std::string cmdline;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) cmdline.push_back(' ');
    cmdline += WinQuoteArg(argv[i]);
  }

  // Scrubbed environment block (double-NUL terminated). Drop the Windows
  // "=C:" drive pseudo-vars (leading '='); BuildScrubbedCurlEnv keeps only the
  // allowlist (PATH, SSL_CERT_FILE, …) and strips proxy vars.
  std::vector<std::string> source_env;
  if (LPCH env_strings = GetEnvironmentStringsA(); env_strings != nullptr) {
    for (LPCH p = env_strings; *p != '\0';) {
      std::string entry(p);
      if (!entry.empty() && entry[0] != '=') source_env.push_back(entry);
      p += entry.size() + 1;
    }
    FreeEnvironmentStringsA(env_strings);
  }
  std::vector<std::string> scrubbed = BuildScrubbedCurlEnv(source_env);
  std::string env_block;
  for (const std::string& e : scrubbed) {
    env_block.append(e);
    env_block.push_back('\0');
  }
  env_block.push_back('\0');  // terminate the block

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;
  HANDLE read_h = nullptr;
  HANDLE write_h = nullptr;
  if (!CreatePipe(&read_h, &write_h, &sa, 0)) {
    out.error = "CreatePipe failed";
    return out;
  }
  // The parent's read end must not be inherited by curl.
  SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si;
  std::memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = write_h;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi;
  std::memset(&pi, 0, sizeof(pi));

  // lpApplicationName=null => CreateProcess parses the first cmdline token and
  // searches PATH for curl.exe (Windows 10+ ships curl). The pinned argv is
  // unchanged, so the IP-pin SSRF defense is identical to the POSIX path.
  std::vector<char> cmdline_mut(cmdline.begin(), cmdline.end());
  cmdline_mut.push_back('\0');

  BOOL ok = CreateProcessA(
      /*lpApplicationName=*/nullptr, cmdline_mut.data(),
      /*lpProcessAttributes=*/nullptr, /*lpThreadAttributes=*/nullptr,
      /*bInheritHandles=*/TRUE, /*dwCreationFlags=*/0,
      /*lpEnvironment=*/env_block.data(), /*lpCurrentDirectory=*/nullptr, &si,
      &pi);
  CloseHandle(write_h);  // parent never writes; child holds the only writer
  if (!ok) {
    CloseHandle(read_h);
    out.error = "failed to spawn curl";
    return out;
  }
  out.spawned = true;

  char buf[8192];
  DWORD n = 0;
  bool oversize = false;
  while (ReadFile(read_h, buf, sizeof(buf), &n, nullptr) && n > 0) {
    if (out.output.size() + static_cast<std::size_t>(n) > max_bytes) {
      oversize = true;
      break;  // do not append the overflowing chunk
    }
    out.output.append(buf, static_cast<std::size_t>(n));
  }
  CloseHandle(read_h);

  if (oversize) {
    TerminateProcess(pi.hProcess, 1);
  }
  WaitForSingleObject(pi.hProcess, INFINITE);

  if (oversize) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    out.error = "response too large";
    return out;  // exit_code unset => caller treats as did-not-complete (deny)
  }
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  out.exit_code = static_cast<int>(exit_code);
  if (exit_code != 0) {
    out.error = "curl non-zero exit";
  }
  return out;
}

}  // namespace

SpawnCurlFn RealCurlSpawn() { return WindowsCurlSpawn; }
ResolveHostFn RealHostResolver() { return WindowsResolve; }

#endif  // _WIN32

}  // namespace pagespeed
