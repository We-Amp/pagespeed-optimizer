// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_MARKDOWN_CODING_H_
#define PAGESPEED_LIB_CACHE_MARKDOWN_CODING_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "lib/classify/alternate_id.h"

namespace pagespeed {

// The content codings a stored Markdown copy exists in.  The plain copy is
// kIdentity (SentinelId::kAgentMarkdown); the worker compresses it once into
// a gzip copy (kAgentMarkdownGzip) and a brotli copy (kAgentMarkdownBrotli).
// The numeric values are the C API's PS_MARKDOWN_ENCODING_* values: append
// only.
enum class MarkdownCoding : uint8_t {
  kIdentity = 0,
  kGzip = 1,
  kBrotli = 2,
};

inline constexpr size_t kMarkdownCodingCount = 3;

// "identity", "gzip" or "br": the Content-Encoding token, and the name mixed
// into a coded copy's entity tag.
std::string_view MarkdownCodingName(MarkdownCoding coding);

// The stored id of the copy in `coding`.
inline constexpr AlternateId MarkdownCopyId(MarkdownCoding coding) {
  switch (coding) {
    case MarkdownCoding::kGzip:
      return static_cast<AlternateId>(SentinelId::kAgentMarkdownGzip);
    case MarkdownCoding::kBrotli:
      return static_cast<AlternateId>(SentinelId::kAgentMarkdownBrotli);
    case MarkdownCoding::kIdentity:
      break;
  }
  return static_cast<AlternateId>(SentinelId::kAgentMarkdown);
}

// The coding a coded copy's id names; nullopt for any other id (the plain
// copy included).
std::optional<MarkdownCoding> MarkdownCodingOfCodedId(AlternateId id);

// The codings a request accepts, in the order it prefers them.
struct MarkdownCodingOrder {
  size_t count = 0;  // 0: no coding is acceptable
  std::array<MarkdownCoding, kMarkdownCodingCount> order{};
};

// The acceptable codings among br, gzip and identity for a request's
// Accept-Encoding field lines (RFC 9110 section 12.5.3), in preference order:
// q descending, ties br, then gzip, then identity.
//
//   - tokens compare case-insensitively, and x-gzip is gzip (section 8.4.1.3);
//   - a q-value of 0 (q=0, q=0.0, q=0.000) excludes a coding;
//   - `*` gives its q to every coding the field does not name, identity
//     included;
//   - identity is acceptable unless identity;q=0, or *;q=0 with identity not
//     named; identity that is not named and not covered by `*` ranks last;
//   - an element whose q-value does not parse (qvalue grammar, section
//     12.4.2) is dropped, as if it were not there;
//   - a coding named more than once takes the lowest q it was given;
//   - no lines, or lines with no element, mean identity only.  That is
//     stricter than section 12.5.3, which reads an absent field as "any
//     coding"; it is deliberate for a copy that is never compressed on the
//     fly.
MarkdownCodingOrder SelectMarkdownCoding(
    std::span<const std::string_view> accept_encoding_lines);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_MARKDOWN_CODING_H_
