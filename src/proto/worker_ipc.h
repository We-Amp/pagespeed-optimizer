// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Worker IPC Protocol
//
// Lightweight notification protocol: the nginx proxy records origin
// responses into the Cyclone cache and sends a fire-and-forget
// notification to the worker.  The worker reads content from cache,
// processes it, and writes optimized variants back.  No response is
// sent.

#ifndef PAGESPEED_SRC_PROTO_WORKER_IPC_H_
#define PAGESPEED_SRC_PROTO_WORKER_IPC_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lib/classify/content_type.h"
#include "lib/classify/option_context.h"

namespace pagespeed {

// Maximum URL length accepted during deserialization (16 KB).
inline constexpr uint32_t kMaxUrlLength = 16384;

// Maximum hostname length accepted during deserialization.
inline constexpr uint32_t kMaxHostnameLength = 512;

// Wire protocol version.  v5 appends the per-request option context; v4
// appended the agent_request bit.
//
// WHY THE VERSION MOVED RATHER THAN THE FRAME JUST GETTING LONGER.  Appending
// a field without bumping the version looks free — a v4 reader would stop at
// the end of the fields it knows and ignore the tail.  It is not free, it is
// the exact failure the rule below forbids, wearing a friendlier hat: the v4
// reader would accept the frame, act on it, and never learn that the sender
// had attached an option context it was supposed to honour.  The result is a
// notification processed under the WRONG option context, which is a cache
// write for one configuration filed under another's name.  A version bump
// turns that same skew into a refusal, which costs one unoptimized response.
//
// TOLERANCE, stated as the rule rather than as an assumption about who is on
// the other end of the socket: this protocol has no version negotiation and
// never will, so the ONLY safe response to a version byte that is not
// kIpcVersion is to refuse the message whole.  A notification carrying any
// other version — an older peer, or a NEWER one, which is the case that
// actually arrives once modules ship out of tree on their own cadence — is
// rejected without reading a single field past the version byte.  It is never
// parsed best-effort, never partially applied, and never guessed at from its
// length.
//
// The cost of a refusal is bounded and known: the notification is dropped, so
// the resource it named is not optimized this time round and the response is
// served straight through.  Skew costs cache warmth, not correctness.  The
// cost of the alternative — decoding v5 bytes with a v4 reader because the
// frame happened to be long enough — is a notification whose url, mask or
// content type mean something other than what the peer said, which is a
// corrupt cache write.  That asymmetry is the whole argument.
//
// A refusal is NOT silent.  Deserialize reports why through the optional
// IpcRejection out-param below, and the receive path counts and logs a
// version-skewed peer separately from a corrupt frame: the two look identical
// from the outside (nothing gets optimized) and have completely different
// fixes, so an operator who cannot tell them apart is left staring at a cache
// that never warms.
inline constexpr uint8_t kIpcVersion = 5;

// Why a notification was refused.  Deliberately distinguishes skew from
// damage; see the kIpcVersion note above.
enum class IpcRejectReason : uint8_t {
  kNone = 0,             // Accepted.
  kTruncated = 1,        // Frame shorter than its own length header claims.
  kVersionMismatch = 2,  // Peer speaks a wire version this build does not.
  kMalformed = 3,        // Version matched; a field failed validation.
};

// Out-param detail for a refused notification.
struct IpcRejection {
  IpcRejectReason reason = IpcRejectReason::kNone;
  // The version byte the peer sent.  Meaningful only for kVersionMismatch;
  // zero when the frame was too short to contain a version byte at all.
  uint8_t peer_version = 0;
};

// Scheme identifiers used on the wire (1 byte).
enum class IpcScheme : uint8_t {
  kHttp = 0x01,
  kHttps = 0x02,
};

// Validate a scheme string.  Returns true only for "http" and "https"
// (lowercase, exact match).
bool ValidateScheme(std::string_view scheme);

// Convert between wire enum and string representation.
IpcScheme SchemeToWire(std::string_view scheme);
std::string_view SchemeFromWire(IpcScheme wire);

// Fire-and-forget notification: tells the worker that new content has
// been recorded in the cache and is ready for optimization.
struct CacheNotification {
  std::string url;
  std::string hostname;
  std::string scheme;  // "http" or "https"
  ContentType content_type;
  uint32_t capability_mask;
  // Added in v4: the triggering request was an agent request with the
  // operator's agent_optimize flag on (Accept: text/markdown +
  // agent_optimize_entitled in the shared config).  Lets the worker
  // demand-gate the per-URL forced agent render to actual agent demand, instead
  // of force-rendering markdown for every browser-touched warm-template URL.
  bool agent_request = false;

  // The per-request option context (v5), or both empty when the sender has
  // none — which is the ordinary case for a sender that resolves no
  // per-request configuration of its own.
  //
  // `option_context` is the canonical payload and is OPAQUE here: this struct
  // carries it, the receive path checks it is self-consistent and bounded, and
  // nothing in this engine reads an option out of it.  `option_signature` is
  // the 64-character lowercase-hex name of that payload.  At dispatch the
  // signature is re-derived from the payload and compared byte for byte — that
  // check is what catches two implementations of the format drifting apart, and
  // a mismatch is a refusal (Worker::AcceptOptionContext).  A context that
  // passes is accepted whatever it names, and its work is stored under the
  // default context; see lib/classify/option_context.h for why that is the
  // right answer on this surface and what the unused context-keyed machinery is
  // held for.
  //
  // Both empty, or neither.  A payload without its signature cannot be checked
  // against anything; a signature without its payload names a context whose
  // content this side never saw.  Serialize refuses to emit either shape and
  // Deserialize refuses to accept one.
  std::string option_context;
  std::string option_signature;

  // Serialize to binary format (v5).
  // Returns empty vector if validation fails.
  std::vector<char> Serialize() const;

  // Deserialize from binary format.  Accepts kIpcVersion and nothing else:
  // any other version byte — lower OR higher — is refused outright, with no
  // field read past it (see the kIpcVersion note above).
  //
  // On failure, *rejection (when non-null) says why: a version-skewed peer
  // (kVersionMismatch, with the version it sent) or a truncated/corrupt frame.
  // Callers that do not care may omit it.
  static bool Deserialize(std::string_view data,
                          CacheNotification* notification,
                          IpcRejection* rejection = nullptr);
};

// Wire format (v5 — appends the option context; v4 appended the agent_request
// bit; v3 added version+scheme):
//
// The socket framing layer reads the first 4 bytes as total_length,
// then passes all bytes (including the 4-byte header) to Deserialize.
// The version byte follows total_length (inside the payload). Every writer
// MUST emit the current kIpcVersion; the reader rejects any other version
// outright and reports the skew rather than attempting the parse.
//
//   [4 bytes: total_length (big-endian, not including this field)]
//   [1 byte:  version (must be kIpcVersion)]
//   [4 bytes: url_length]
//   [url_length bytes: url]
//   [4 bytes: hostname_length]
//   [hostname_length bytes: hostname]
//   [1 byte: content_type]
//   [4 bytes: capability_mask (big-endian)]
//   [1 byte: scheme (IpcScheme enum)]
//   [1 byte: agent_request (0/1) — v4]
//   [4 bytes: option_context_length — v5]
//   [option_context_length bytes: option_context]
//   [1 byte: option_signature_length — v5, 0 or 64]
//   [option_signature_length bytes: option_signature]

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_PROTO_WORKER_IPC_H_
