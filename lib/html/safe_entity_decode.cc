// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/safe_entity_decode.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "lib/html/html_keywords.h"

namespace net_instaweb {

namespace {

// Length (in bytes) of the longest well-formed-UTF-8 PREFIX of `s`. `s` is
// well-formed iff this equals s.size(); otherwise the returned length is the
// byte offset of the first invalid/incomplete sequence, so s.substr(0, len) is
// always well-formed. Rejects lone continuation bytes, invalid lead bytes (e.g.
// a lone Latin-1 0xB7 from &middot;), truncated/overlong sequences, surrogates,
// and out-of-range code points.
std::size_t WellFormedUtf8PrefixLen(std::string_view s) {
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t cont;  // number of trailing continuation bytes expected
    unsigned int cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      cont = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      cont = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      cont = 3;
      cp = c & 0x07;
    } else {
      return i;  // lone continuation byte or invalid lead byte.
    }
    if (i + cont >= n) return i;  // truncated sequence (e.g. byte-cap split).
    for (std::size_t k = 1; k <= cont; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return i;  // not a continuation byte.
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((cont == 1 && cp < 0x80) || (cont == 2 && cp < 0x800) ||
        (cont == 3 && cp < 0x10000)) {
      return i;  // overlong encoding.
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return i;  // out of range or UTF-16 surrogate.
    }
    i += cont + 1;
  }
  return n;
}

// Length of the well-formed UTF-8 sequence starting at s[i], 0 when s[i]
// does not start one. *truncated is set when the bytes from s[i] to the end
// are a correct but incomplete start of a sequence.
std::size_t Utf8SequenceLen(std::string_view s, std::size_t i,
                            bool* truncated) {
  *truncated = false;
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) return 1;
  std::size_t cont;
  unsigned int lo = 0x80;  // allowed range of the first continuation byte
  unsigned int hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) {
    cont = 1;
  } else if (c >= 0xE0 && c <= 0xEF) {
    cont = 2;
    if (c == 0xE0) lo = 0xA0;  // no overlong form
    if (c == 0xED) hi = 0x9F;  // no UTF-16 surrogate
  } else if (c >= 0xF0 && c <= 0xF4) {
    cont = 3;
    if (c == 0xF0) lo = 0x90;  // no overlong form
    if (c == 0xF4) hi = 0x8F;  // nothing above U+10FFFF
  } else {
    return 0;  // continuation byte, C0/C1 or F5..FF: never a lead byte
  }
  for (std::size_t k = 1; k <= cont; ++k) {
    if (i + k >= s.size()) {
      *truncated = true;
      return 0;
    }
    const unsigned int cc = static_cast<unsigned char>(s[i + k]);
    const unsigned int min = k == 1 ? lo : 0x80;
    const unsigned int max = k == 1 ? hi : 0xBF;
    if (cc < min || cc > max) return 0;
  }
  return cont + 1;
}

}  // namespace

bool IsWellFormedUtf8(std::string_view s) {
  return WellFormedUtf8PrefixLen(s) == s.size();
}

std::string ToWellFormedUtf8(std::string_view s) {
  if (IsWellFormedUtf8(s)) return std::string(s);
  std::string out;
  out.reserve(s.size() + 8);
  std::size_t i = 0;
  while (i < s.size()) {
    bool truncated = false;
    const std::size_t len = Utf8SequenceLen(s, i, &truncated);
    if (len > 0) {
      out.append(s.substr(i, len));
      i += len;
    } else if (truncated) {
      break;  // a sequence cut off at the end (a byte cap): dropped
    } else {
      out.append("\xEF\xBF\xBD");  // U+FFFD REPLACEMENT CHARACTER
      ++i;
    }
  }
  return out;
}

void FoldNbsp(std::string* s) {
  std::size_t w = 0;
  for (std::size_t r = 0; r < s->size(); ++r) {
    const unsigned char c = static_cast<unsigned char>((*s)[r]);
    if (c == 0xC2 && r + 1 < s->size() &&
        static_cast<unsigned char>((*s)[r + 1]) == 0xA0) {
      (*s)[w++] = ' ';
      ++r;
    } else {
      (*s)[w++] = static_cast<char>(c);
    }
  }
  s->resize(w);
}

void FoldLatin1Nbsp(std::string* s) {
  for (char& c : *s) {
    if (static_cast<unsigned char>(c) == 0xA0) c = ' ';
  }
}

std::string SafeDecodeHtmlEntities(std::string_view s) {
  // 1. Fast path: nothing to decode; preserve the bytes (still honoring the
  //    "result is always well-formed UTF-8" contract). Callers may byte-cap the
  //    raw value BEFORE this decode (e.g. ExtractHtmlMetadata's kMaxFieldBytes),
  //    which can split a trailing multibyte sequence; trim that incomplete tail
  //    so a lone invalid byte never ships to the agent feed / llms.txt.
  // Every result is well-formed UTF-8 with each no-break space folded to an
  //    ASCII space.
  const auto clean = [](std::string_view v) {
    std::string r = ToWellFormedUtf8(v);
    FoldNbsp(&r);
    return r;
  };
  if (s.find('&') == std::string_view::npos) return clean(s);
  std::string buf;
  bool decoding_error = false;
  std::string_view out = HtmlKeywords::Unescape(s, &buf, &decoding_error);
  // 2. Decode failed (non-ASCII byte / multi-byte entity): keep the original.
  if (decoding_error) return clean(s);
  // 3. Decode succeeded (the input was ASCII): fold the Latin-1 NBSP the
  //    decoder emits for &nbsp;, then validate UTF-8.
  std::string decoded(out);
  FoldLatin1Nbsp(&decoded);
  if (!IsWellFormedUtf8(decoded)) {
    // A lone Latin-1 byte (e.g. &middot; -> 0xB7) survived the decode. Never
    // ship an invalid byte: fall back to the original entity text.
    return clean(s);
  }
  return decoded;
}

}  // namespace net_instaweb
