// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_SITE_PURGE_EPOCH_H_
#define PAGESPEED_LIB_CACHE_SITE_PURGE_EPOCH_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace pagespeed {

// The per-site purge epoch: the time, in Unix seconds, of the last global
// purge one site's front end observed.  A front end treats an entry it stored
// at or before the epoch as purged.
//
// WHERE IT LIVES, and why there.  One small file per site BESIDE the cache
// volume, never inside it: the volume is a set of circular logs whose
// reclamation (wrap, full directory buckets, chain carry-forward) has no
// ordering against the entries the epoch fences, so a record in the volume
// can be reclaimed while older entries survive.  A file is not reclaimed,
// and it outlives a front end's own caches and processes.
//
//   <volume_path>.purge-epoch.<site>       the value
//   <volume_path>.purge-epoch.<site>.lock  the writers' lock (never renamed)
//   <volume_path>.purge-epoch.<site>.tmp   a write in progress
//
// `site` is exactly 64 lowercase hex characters (it becomes part of a file
// name; anything else is refused before any path is built).
//
// FORMAT: the ASCII line "pspe1 <decimal seconds>\n" and nothing else.  Any
// other content — truncated, extra bytes, a format this build does not know —
// is kCorrupted, never 0: a reader that cannot tell what the epoch is must
// fence everything rather than un-purge anything.
//
// WRITE: a max-merge under an exclusive OS advisory lock on the .lock file
// (fcntl F_SETLKW on POSIX, LockFileEx on Windows) plus a process-wide mutex
// (fcntl locks are per process, so threads of one process are serialised
// here): read the current value, and only when the new one is larger write it
// to the .tmp file, flush it to disk, and rename it over the value file.  A
// crash before the rename leaves the old value in place; the next writer
// overwrites the stale .tmp.  A value at or below the stored one is a
// successful no-op.  A value later than `now_seconds` +
// kSitePurgeEpochSkewSeconds is kInvalidArgument.
//
// THE VOLUME GENERATION.  The file is not keyed by the volume's generation,
// on purpose.  When the volume is recreated it holds no entry from before,
// and the epoch only ever fences entries stored at or before it, so a file
// that outlives a recreate can only fence MORE than needed (fail closed,
// healed by the next store), never resurrect an entry; a file that is
// missing after a recreate fences nothing the new volume holds.  Keying by
// generation would instead drop the epoch whenever the generation moves for
// a reason other than a recreate.
enum class SitePurgeEpochError {
  kInvalidArgument,  // bad site, or a value too far in the future
  kIo,               // the file or its lock could not be read or written
  kCorrupted,        // the file exists and is not a value this build wrote
};

inline constexpr uint32_t kSitePurgeEpochSkewSeconds = 5;

// True for exactly 64 characters of [0-9a-f].
bool IsSitePurgeEpochSite(std::string_view site);

// The value file's path; empty when `site` is not a site id or
// `volume_path` is empty.
std::string SitePurgeEpochPath(std::string_view volume_path,
                               std::string_view site);

// 0 when the file does not exist.
std::expected<uint64_t, SitePurgeEpochError> ReadSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site);

std::expected<void, SitePurgeEpochError> WriteSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site, uint64_t epoch_seconds,
    uint64_t now_seconds);

// Parse the file's content; exposed for tests.
std::expected<uint64_t, SitePurgeEpochError> ParseSitePurgeEpoch(
    std::string_view content);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_SITE_PURGE_EPOCH_H_
