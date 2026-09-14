# Nginx Module

Built as a dynamic `.so` via Bazel. Targets tagged `manual` (skipped by
`bazel build //...`).

```bash
NGINX_PATH=/path/to/nginx-source bazel build //src/nginx:ngx_pagespeed_module.so
```

`bazel/nginx.bzl` auto-detects nginx source from: `NGINX_PATH` env var,
`third_party/nginx` submodule, or `/usr/local/src/nginx` fallback. Generates
stub headers if nginx hasn't been `./configure`d.

## Response Header Handling

Headers inspected before recording to cache:

| Header | Behavior |
|--------|----------|
| `Cache-Control: no-store/private` | NOT cached, no worker notification |
| Request carries `Authorization` + response CC lacks `public`/`must-revalidate`/`s-maxage` | NOT cached, no worker notification (RFC 9111 §3.5) |
| `Cache-Control: no-transform` | Cached but NO worker notification |
| `Set-Cookie` | NOT cached (implicitly private) |
| `Vary: *` or unsupported Vary | NOT cached |
| `Vary: Accept-Encoding, User-Agent, Accept, Save-Data` | Allowed (handled via capability mask) |
| HEAD requests | NOT cached |

Flags set in header filter, checked in body filter. HIT path unaffected —
except by the Authorization gate, which also gates the HIT path (see below).

## HIT Path

**Content-Type**: Uses `AlternateMetadata.origin_content_type` (preserves charset).
Transcoded images use format-specific MIME. Falls back to `MimeFromUrl`.

**Content-Encoding**: When serving a pre-compressed variant (gzip/brotli),
decodes `meta.full_mask` to extract `TransferEncoding` and sets
`Content-Encoding: gzip` or `Content-Encoding: br`. Also assigns
`r->headers_out.content_encoding` to prevent nginx's built-in gzip and brotli
filters from double-compressing the already-compressed content.

**Authorization (RFC 9111 §3.5)**: Authorization-bearing requests are served
from cache only when the stored entry's origin Cache-Control carries a §3.5
permit (`public`, `must-revalidate`, or `s-maxage`) — else treated as MISS.
Once stale, the two stale serves — the bounded coalesced-stale serve and the
stale-if-error serve — additionally require `public`: each consults
`AuthzCacheGateAllowsStale` at its own serve site (the stash-time gate only
proves the STORE permit). Early Hints on MISS are suppressed for
Authorization-bearing requests (see Early Hints below).
RSL-CAP exemption: with `rsl_cap_enforcement` on, `Authorization: License`
requests are exempt — derived at gate time from live enforcement state plus
the header's scheme (PREACCESS already validated-or-401'd them; no
per-request state). Main external requests only: never on internal requests
(`r->internal` — error_page/index/try_files redirect targets, subrequests),
which PREACCESS declines and therefore never License-validated; those are
gated as ordinary Authorization. Deliberate fail-closed trade-off: under
enforcement, topologies where every request reaching the module is internal
(`try_files ... @proxy` named-location proxying, `rewrite ... last`
front-controllers, X-Accel-Redirect targets) lose the License exemption, so
licensed-agent requests there fall to ordinary §3.5 gating and get the
cached lane only when the origin's cache directives permit it
(`public`/`must-revalidate`/`s-maxage`). A validated-flag design that would
restore the exemption for those topologies is tracked in a follow-up issue.
Canonical rationale: `src/nginx/authz_cache_gate.h`.

**ETag**: Cache HITs include a weak ETag in one of two shapes —
`W/"ps-<mask8><flags2>-<identity16>-<length16>"` when the entry carries a
content-identity signal (stored origin hash, or FNV over origin
ETag/Last-Modified), else the legacy `W/"ps-<mask8><flags2>-<length16>"`.
Canonical format description and rationale: `src/nginx/etag_util.h`.
Nginx's `not_modified` filter handles `If-None-Match` → 304 automatically.
Range requests enabled via `r->allow_ranges`. Weak ETags cause `If-Range` to
fall back to a full 200 (correct per RFC 9110).

**Cache-Control** on HIT uses the origin's Cache-Control directives stored in
`AlternateMetadata` (v3). The serving algorithm (RFC 9111 compliant):

1. **Recording** (MISS path): Parses all 10 CC directives (no-cache,
   must-revalidate, no-store, private, public, immutable, s-maxage,
   proxy-revalidate, no-transform, max-age) + Expires fallback into
   `AlternateMetadata` fields. Adjusts `cache_inserted_at` by inbound `Age`.
2. **Serving** (HIT path):
   - **Age** = `now - cache_inserted_at` (clamped to 0; corrupted → UINT32_MAX)
   - **Effective max-age** = `min(origin_max_age, pagespeed_max_age)` with
     immutable exception using `pagespeed_immutable_max_age`
   - **Stale + must-revalidate/proxy-revalidate/s-maxage** → falls through to
     MISS (origin re-fetch)
   - **no-store/private** → defensively served as `no-cache`
   - **no-cache** → passed through verbatim
   - **Type defaults** (no CC header): HTML=`no-cache`, CSS/JS=300s, Images=3600s
   - **SWR synthesis** (configurable): `stale-while-revalidate=min(eff_max_age, 86400)`
     suppressed when must-revalidate, proxy-revalidate, or s-maxage present
   - **Age header** emitted on all HIT responses

**Directives**:
- `pagespeed_max_age N;` — cap on origin max-age (default: 86400)
- `pagespeed_immutable_max_age N;` — cap for immutable content (default: 604800)
- `pagespeed_synthesize_swr on|off;` — SWR synthesis (default: on)
- `pagespeed_conditional_revalidation on|off;` — conditional revalidation (default: on)
- `pagespeed_html_max_age N;` — default max-age for HTML when origin sends no CC (default: 0)
- `pagespeed_css_max_age N;` — default max-age for CSS/JS when origin sends no CC (default: 300)
- `pagespeed_image_max_age N;` — default max-age for images when origin sends no CC (default: 3600)
- `pagespeed_force_refresh_html on|off;` — force revalidation on browser Ctrl+F5 for HTML (default: on)
- `pagespeed_force_refresh on|off;` — force revalidation on browser Ctrl+F5 for all non-HTML types (default: off)

**Vary headers** emitted on cache-served responses:
- Images: `Vary: Accept, Save-Data, User-Agent`
- HTML: `Vary: Accept-Encoding, User-Agent`
- CSS/JS: `Vary: Accept-Encoding`

## Conditional Revalidation

When a cached entry becomes stale and has stored ETag or Last-Modified tokens
(metadata v5), nginx sends `If-None-Match` / `If-Modified-Since` to origin
instead of a full re-fetch. On 304: refreshes metadata timestamps, copies
cached content to new alternate, evicts RAM cache, serves with
`X-PageSpeed: REVALIDATED`. On 200: discards stale, proceeds with normal MISS.

**Storability is re-evaluated on the 304** (issue #1016) before the
write-back, over the *merged* headers, in
`src/nginx/revalidation_storability.{h,cc}` — the same flag vocabulary the
record path checks, so both agree on "storable". Three verdicts: `kEvict`
(304 says `no-store` or **bare** `private` — the stored entry is removed for
the whole URL), `kSkipRestamp` (still storable, but this exchange may not
extend its life: §3.5 permit withdrawn, or the 304 carried `Set-Cookie` —
the entry keeps its old stamp and simply revalidates again), `kRestamp`
(unchanged). Deliberately not `kEvict` on the §3.5 path: an inbound request
header must never be able to flush another client's lawfully stored entry.
This gates retention only — the current requester is still served, which is
this branch's pre-existing behavior and an open product question, not
something the commit settled.

**Qualified `private` / `no-cache`** (`private="Set-Cookie"`, RFC 9111
§5.2.2.7) restrict only the named field-names and permit storing the
remainder. The parser records the FORM of each occurrence —
`kCCOriginPrivateBare` / `kCCOriginPrivateQualified` (and the `NoCache`
pair) — alongside the base flag, never instead of it, so every existing
reader keeps its conservative behavior. **Both directions are tracked
on purpose:** `cc_flags` is OR-accumulated across every token of every
`Cache-Control` header line, so a "qualified" bit alone would only mean
"some occurrence was qualified", and

```
Cache-Control: private="Set-Cookie"
Cache-Control: private
```

would skip an eviction the bare directive plainly demands. *Bare-seen* is
monotone under OR and is the load-bearing bit. Decision rule for any
destructive consumer: bare set → blanket; qualified set and bare clear →
qualified only; neither (legacy metadata) → treat as blanket, fail safe.
Classification keys on the presence of `=`, not a non-empty value, so
`private=""` (a legal empty field-list) is qualified. The MISS path still
refuses to store either form — deliberately unchanged.

**The 304 branch emits Cache-Control through the canonical
`BuildCacheControlHeader`**, not a local emitter, and sets two independent
flags:

- `relay_origin_no_cache` — **always**, on every 304 outcome. `no-cache` is
  about revalidation, not storage permission; it applies to plain restamps
  too. Gating it on the eviction predicate strips it from every
  `no-cache, max-age=N` response, converting a revalidate-before-every-reuse
  resource into one reusable for the whole max-age. (That conflation was a
  real regression — the two concerns are not the same predicate.)
- `forward_origin_restrictions` — only when the entry was **evicted**.
  Forwards `no-store` / bare `private`, suppresses the safe-mode
  `must-revalidate` auto-add, drops `max-age` under `no-store`, and
  suppresses `stale-if-error` / `stale-while-revalidate` (both are
  permission to serve a stale *stored* copy, which presupposes the storage
  being forbidden).

Both default off, so HIT-path output is byte-identical.

**Dispatch lives in `ApplyRestampVerdict`** (a template over the cache type
in `revalidation_storability.h`), so the cache effects — eviction actually
removing, skip actually not writing — are asserted against a real
`PageSpeedCache` in `test/src/nginx/restamp_apply_test.cc`. Production
instantiates the same code with the same type; no indirection. `Remove()`
returning `NotFound` counts as success (a purge got there first; that IS the
desired end state) — only a genuine refusal such as `NotOwned` is an error,
and it is logged rather than discarded.

Client conditional headers (`If-None-Match` with PageSpeed's weak ETag) are
saved before injection and restored on all exit paths (304, 200, error).
Stats: `g_conditional_revalidations`, `g_conditional_304s`,
`g_conditional_200s`, `g_etag_too_long`.

## Hot URL Warmup

Tracks fallback-hit frequency per URL in fixed-size hash map (4096 entries,
FNV-1a hash, probabilistic eviction). When threshold exceeded (default: 5,
`pagespeed_hot_threshold N;`), sends warmup notification with sentinel mask
`0xFFFFFFFE`.

## Early Hints (103)

On cache MISS for HTML, reads `SentinelId::kEarlyHints` alternate and sends
`103 Early Hints` with Link headers before proxying. Suppressed for
§3.5-effective Authorization-bearing requests: hints derive from a stored
response, the sentinel carries no metadata prefix (no CC flags), and the
main entry's metadata is unavailable or gate-refused at that point — so no
permit can be established and the emit fails closed. Also adds Link headers
on cache HIT for HTML (allowed: that serve already passed the §3.5 gate).
Hint lines use prefixes to determine Link header type:
- No prefix → `rel=preload; as=style` (stylesheet)
- `image:` prefix → `rel=preload; as=image; fetchpriority=high` (LCP image)
- `font:` prefix → `rel=preload; as=font; crossorigin` (font)
- `preconnect:` prefix → `rel=preconnect` (no-cors third-party origin)
- `preconnect-cors:` prefix → `rel=preconnect; crossorigin` (CORS-mode
  third-party origin)

An unknown prefix falls back to the bare stylesheet preload, so a new prefix
must land in every sentinel consumer (`ParseHintLine` and the .NET
middleware's `BuildLinkHeader`) in the same release, with consumers deployed
no later than the emitting worker — and rollback across a sentinel-format
change requires a cache purge.

## Shared Config (`pagespeed-shared.conf`)

The nginx module reads `pagespeed-shared.conf` from `parent_of(cache_path)`
(i.e., the parent directory of the Cyclone cache file). The file is polled
every ~1 second for changes.

**Format:** `key=value` (one per line)

**Fields read by nginx:**
- `socket_path` — Unix socket for worker notifications
- `disable_html` — Whether HTML optimization is disabled (`true`/`false`);
  when `true`, nginx sets `pagespeed_critical_css off` behavior

This replaces the former `pagespeed_worker_socket` and `pagespeed_critical_css`
directives. Both are now configured on the worker side and shared automatically.
(There is no `license_key` any more: the license apparatus was removed at
2.1 GA.)

## `pagespeed_disallow` Directive

URL exclusion patterns (multiple allowed, first match wins):
- Prefix: `pagespeed_disallow /api/;`
- Suffix: `pagespeed_disallow *.woff2;`
- Substring: `pagespeed_disallow admin;`
