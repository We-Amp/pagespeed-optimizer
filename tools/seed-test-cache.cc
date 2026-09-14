// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cache-seeding tool for test-nginx HIT-path tests.
//
// Usage:
//   seed_test_cache --cache /tmp/test.vol --url /style.css --host localhost \
//     --content "h1{color:red}" --content-type css --mask 0x08
//   seed_test_cache --cache /tmp/test.vol --create_only
//
// Creates a Cyclone cache volume and writes a single alternate with the given
// content, content type, and capability mask. Used by t/seed-*.sh scripts
// before running test-nginx HIT-path tests. --create_only creates the volume
// and writes nothing: the nginx module opens its cache resolve-only
// (volume_size = 0) and cannot cold-create a volume, so MISS-path tests run
// against pre-created empty volumes.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <span>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"

ABSL_FLAG(std::string, cache, "", "Path to cache volume file");
ABSL_FLAG(std::string, url, "", "URL path (e.g. /style.css)");
ABSL_FLAG(std::string, host, "localhost", "Hostname for cache key");
ABSL_FLAG(std::string, scheme, "https",
          "Scheme for cache key (must match the request's scheme: the key is "
          "scheme://host/url — seeding plain-http test requests needs "
          "--scheme http)");
ABSL_FLAG(std::string, content, "", "Content string to store");
ABSL_FLAG(std::string, content_file, "",
          "File to read content from (overrides --content)");
ABSL_FLAG(std::string, content_type, "css",
          "Content type: html, css, js, image, other");
ABSL_FLAG(std::string, mask, "0x08", "Capability mask as hex (e.g. 0x08)");
ABSL_FLAG(std::string, origin_ct, "",
          "Origin content-type string (e.g. text/css)");
ABSL_FLAG(std::string, origin_html_hash, "",
          "64 hex chars: v7 origin_html_hash content-identity stamp");
ABSL_FLAG(std::string, origin_etag, "",
          "Origin ETag stored verbatim (v5, e.g. '\"abc\"')");
ABSL_FLAG(uint32_t, origin_last_modified, 0,
          "Origin Last-Modified as unix timestamp (v5, 0 = absent)");
ABSL_FLAG(bool, create_only, false,
          "Create the volume and exit without writing an entry. Used to "
          "pre-create empty volumes for the resolve-only nginx module "
          "(volume_size = 0 cannot cold-create a volume).");

namespace {

pagespeed::ContentType ParseContentType(const std::string& s) {
  if (s == "html") return pagespeed::ContentType::kHtml;
  if (s == "css") return pagespeed::ContentType::kCss;
  if (s == "js") return pagespeed::ContentType::kJs;
  if (s == "image") return pagespeed::ContentType::kImage;
  return pagespeed::ContentType::kOther;
}

std::string DefaultOriginCt(pagespeed::ContentType ct) {
  switch (ct) {
    case pagespeed::ContentType::kHtml:
      return "text/html";
    case pagespeed::ContentType::kCss:
      return "text/css";
    case pagespeed::ContentType::kJs:
      return "application/javascript";
    case pagespeed::ContentType::kImage:
      return "image/jpeg";
    default:
      return "application/octet-stream";
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  const std::string cache_path = absl::GetFlag(FLAGS_cache);
  const std::string url = absl::GetFlag(FLAGS_url);
  const std::string host = absl::GetFlag(FLAGS_host);
  const std::string mask_str = absl::GetFlag(FLAGS_mask);
  const bool create_only = absl::GetFlag(FLAGS_create_only);

  if (cache_path.empty() || (!create_only && url.empty())) {
    std::cerr << "Usage: seed_test_cache --cache <path> --url <url> "
              << "[--host <host>] [--content <str>] [--content-file <file>] "
              << "[--content-type css|html|js|image|other] [--mask 0x08]\n"
              << "   or: seed_test_cache --cache <path> --create_only\n";
    return 1;
  }

  // Parse mask.
  char* endptr = nullptr;
  errno = 0;
  unsigned long parsed = std::strtoul(mask_str.c_str(), &endptr, 0);
  if (errno == ERANGE || endptr == mask_str.c_str() || *endptr != '\0') {
    std::cerr << "Error: invalid mask value: " << mask_str << "\n";
    return 1;
  }
  uint32_t mask_val = static_cast<uint32_t>(parsed);

  // Read content from file or flag.
  std::string content;
  const std::string content_file = absl::GetFlag(FLAGS_content_file);
  if (!content_file.empty()) {
    std::ifstream ifs(content_file, std::ios::binary);
    if (!ifs) {
      std::cerr << "Error: cannot open content file: " << content_file << "\n";
      return 1;
    }
    content.assign(std::istreambuf_iterator<char>(ifs),
                   std::istreambuf_iterator<char>());
  } else {
    content = absl::GetFlag(FLAGS_content);
  }

  if (!create_only && content.empty()) {
    std::cerr
        << "Error: no content specified (use --content or --content-file)\n";
    return 1;
  }

  // Create cache. Use default 1GB volume size to match nginx module's default
  // PageSpeedCacheConfig. Cyclone re-stripes on open if sizes differ, making
  // seeded data invisible.
  pagespeed::PageSpeedCacheConfig config;
  config.volume_path = cache_path;
  config.enable_checksum = true;

  auto cache_result = pagespeed::PageSpeedCache::Create(config);
  if (!cache_result.has_value()) {
    std::cerr << "Error: failed to create cache at " << cache_path
              << " err=" << static_cast<int>(cache_result.error()) << "\n";
    return 1;
  }
  auto& cache = *cache_result;

  if (create_only) {
    std::cout << "Created empty volume at " << cache_path << "\n";
    return 0;
  }

  // Build metadata.
  auto ct = ParseContentType(absl::GetFlag(FLAGS_content_type));
  std::string origin_ct = absl::GetFlag(FLAGS_origin_ct);
  if (origin_ct.empty()) {
    origin_ct = DefaultOriginCt(ct);
  }

  pagespeed::AlternateId id =
      pagespeed::MaskToAlternateId(static_cast<uint8_t>(mask_val & 0xFF));

  pagespeed::AlternateMetadata meta;
  meta.full_mask = mask_val;
  meta.content_type = ct;
  meta.origin_content_type = origin_ct;
  meta.cache_inserted_at = static_cast<uint32_t>(std::time(nullptr));
  meta.origin_etag = absl::GetFlag(FLAGS_origin_etag);
  meta.origin_last_modified = absl::GetFlag(FLAGS_origin_last_modified);

  // Optional v7 content-identity stamp (64 hex chars = 32 bytes).
  const std::string hash_hex = absl::GetFlag(FLAGS_origin_html_hash);
  if (!hash_hex.empty()) {
    if (hash_hex.size() != 2 * pagespeed::AlternateMetadata::kHashSize) {
      std::cerr << "Error: --origin_html_hash must be exactly "
                << 2 * pagespeed::AlternateMetadata::kHashSize
                << " hex chars\n";
      return 1;
    }
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i < pagespeed::AlternateMetadata::kHashSize; ++i) {
      const int hi = nibble(hash_hex[2 * i]);
      const int lo = nibble(hash_hex[2 * i + 1]);
      if (hi < 0 || lo < 0) {
        std::cerr << "Error: --origin_html_hash contains non-hex char\n";
        return 1;
      }
      meta.origin_html_hash[i] = static_cast<std::byte>((hi << 4) | lo);
    }
  }

  // Write alternate.
  auto wh = cache->WriteAlternate(url, host, absl::GetFlag(FLAGS_scheme), id,
                                  content.size(), meta);
  if (!wh.has_value()) {
    std::cerr << "Error: WriteAlternate failed err="
              << static_cast<int>(wh.error()) << "\n";
    return 1;
  }

  auto bytes = std::as_bytes(std::span(content));
  auto written = wh->write_sync(bytes);
  if (!written.has_value()) {
    std::cerr << "Error: write_sync failed err="
              << static_cast<int>(written.error()) << "\n";
    return 1;
  }

  auto closed = wh->close_sync();
  if (!closed.has_value()) {
    std::cerr << "Error: close_sync failed err="
              << static_cast<int>(closed.error()) << "\n";
    return 1;
  }

  std::cout << "Seeded " << content.size() << " bytes at " << url << " (mask=0x"
            << std::hex << mask_val << ") in " << cache_path << "\n";
  return 0;
}
