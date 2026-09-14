// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_HTML_SAFE_ENTITY_DECODE_H_
#define PAGESPEED_LIB_HTML_SAFE_ENTITY_DECODE_H_

#include <string>
#include <string_view>

// UTF-8-safe HTML entity decode for the agent_optimize extraction surface.
// This is the SINGLE source of truth for "decode &amp;/&lt; to human
// glyphs without ever erasing a non-ASCII byte or shipping invalid UTF-8."
//
// It exists because HtmlKeywords::Unescape (the engine entity decoder) sets
// decoding_error=true and returns EMPTY for ANY input byte > 127 or any entity
// that decodes to more than one Latin-1 byte. Routing agent-facing text through
// Unescape naively erases every non-ASCII text node (em-dashes, curly quotes,
// accents are literal multibyte UTF-8 in Chrome's rendered outerHTML) — a net
// regression. Several extraction paths (body text, inline-code spans, the
// <meta description> attribute) each need this same guard; sharing it here
// prevents the "fixed in one path, missed in another" drift the v2.0.27 audit
// found between the body-text path (Issue F) and the inline-code / metadata
// paths.

namespace net_instaweb {

// True iff `s` is well-formed UTF-8. Rejects lone continuation bytes, invalid
// lead bytes (e.g. a lone Latin-1 0xB7 from &middot;), truncated/overlong
// sequences, surrogates, and out-of-range code points.
bool IsWellFormedUtf8(std::string_view s);

// Fold every Latin-1 NBSP (0xA0) — the one non-ASCII byte HtmlKeywords::Unescape
// emits for &nbsp; — to an ASCII space, in place. A lone 0xA0 is invalid UTF-8,
// so it must never reach the agent feed raw.
void FoldLatin1Nbsp(std::string* s);

// Entity-decode `s` for an agent-facing feed:
//   1. Fast path: no '&' -> return the input verbatim (preserves pure UTF-8,
//      and avoids the Unescape scan).
//   2. On decoding_error -> return the ORIGINAL input verbatim (never the empty
//      Unescape result), preserving the original UTF-8 bytes.
//   3. On success -> fold 0xA0 (NBSP) to a space, then require the result to be
//      well-formed UTF-8. A named/numeric entity that decodes to a single
//      Latin-1 byte 0x80-0xFF (&middot;=0xB7, &copy;=0xA9, &eacute;=0xE9) is a
//      lone invalid byte; in that case fall back to the original entity text
//      rather than ship an invalid byte. Otherwise return the decoded string.
//
// The result is always well-formed UTF-8 (or the verbatim original input, which
// the callers treat as already-UTF-8 rendered DOM). Decoding is intentionally
// done BEFORE any markdown-structural escaping so a decoded '<script>' stays
// inert text downstream.
std::string SafeDecodeHtmlEntities(std::string_view s);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_SAFE_ENTITY_DECODE_H_
