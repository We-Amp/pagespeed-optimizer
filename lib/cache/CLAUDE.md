# Cache Library

Cyclone cache wrapper providing variant-aware storage with capability-based keys.

## Key Files
- `cache.h` -- public API: `PageSpeedCache`, `PageSpeedCacheConfig`, `ReadResult`, `WriteResult`
- `cache.cc` -- implementation (Cyclone C API calls, SHA-256 key composition)

## Architecture

`PageSpeedCache` wraps Cyclone's C library with a C++ API for variant storage.
Cache keys are `SHA-256(URL, hostname)`. Multiple variants per URL are stored as
alternates, each identified by an `AlternateId` (low 8 bits of `CapabilityMask`).

Key methods:
- `Create(config, handler)` -- factory, opens/creates Cyclone volume
- `ReadBestAlternate(url, hostname, mask)` -- single-pass alternate selection via `PageSpeedSelector`
- `WriteAlternate(url, hostname, id, metadata, data)` -- write variant
- `WriteSentinel(url, hostname, sentinel_id, data)` -- write non-variant data (e.g. Early Hints)
- `WriteOriginalAlternate(url, hostname, scheme, len, metadata)` / `ReadOriginalAlternate(...)`
  -- the durable original class (`0x0C`): the origin's own bytes, written with a
  metadata prefix, read by EXACT ID, never selectable
- `WriteHeadersSidecar(url, hostname, scheme, headers)` / `ReadHeadersSidecar(...)`
  -- the response-header sidecar (`0x6C`): one entry per URL holding the curated,
  verbatim block of request-independent response headers. The write entry point
  IS the admission gate (`headers_sidecar.h`), read by EXACT ID, never selectable
- `RemoveAlternatesExcept(url, hostname, scheme, preserve)` -- the origin-refresh
  purge: bounded passes over (capped) listings; escalates to a whole-key drop
  only for a chain the storage layer cannot unlink from -- a cyclic chain,
  which its read and removal walks do not guard against and whose write-time
  single-id reset does not cover a key that also holds an original -- never for
  a failed removal or an unreadable listing
- `ListAlternates(url, hostname)` -- enumerate all variants for a URL

## Testing
```bash
bazel test //test/lib/cache/...
```

## Gotchas
- Write invariant: nginx writes the default alternate (`CapabilityMask()` = 0x08);
  the worker writes the others. No write contention by design. The ids are NOT
  disjoint: `0x08` holds origin bytes only until an optimized variant for that
  same mask overwrites it, so "the original" durably means the `0x0C` class,
  which has exactly one writer and is never overwritten by an optimizer.
- EVERY write path here is alternate-scoped, and must stay that way. A plain
  whole-key Cyclone write on a key that carries an alternate chain repoints the
  directory head at a document with no alternate id, ORPHANING the whole chain —
  silently, totally, and looking exactly like a cold cache. Proven both ways in
  `test/lib/cache/durable_originals_test.cc`. The class does not publish
  `cyclone::Cache&` for that reason: production gets `VolumeCapacityBytes()`,
  which is all it ever needed, and tests reach the raw handle through
  `//test/test_util:cache_test_peer` (a `testonly` target, so a production
  target cannot depend on it). `WriteResult::handle_` is private for the same
  reason one level down — it is the bypass around the streaming content cap.
- The durable original carries a content cap (`max_original_content_length`,
  0 = 16 MiB default) enforced WHILE STREAMING: Cyclone buffers a value's whole
  content in process heap until `close()`, so measuring after the fact has
  already cost what the cap prevents. Over-cap is a SKIPPED STORE, counted in
  `OriginalsOverCapSkipped()` — never a startup refusal, and a peer configured
  differently costs a skipped entry, not a split view.
- The headers sidecar's write path is its GATE, not a store call with a check in
  front of it: it takes the response's whole header set and decides. Two things
  follow. A refusal is a normal outcome (`stored == false`, not an error) — most
  responses carry something the entry cannot reproduce, and those are served
  plain. And the allowlist is a closed set, not a policy knob: a header whose
  value depends on the request must never be replayed to the next client, which
  is why the class has one writer and why the generic sentinel surface refuses
  `0x6C`. A `'nonce-'`-bearing CSP is refused outright — never "store the rest
  and drop that one".
- "One entry per URL" used to hold only for a SINGLE writer. Unlink-then-write
  is not one operation, so two writers for the same URL could interleave into a
  chain of two — a fixed point, since unlink-one/write-one kept it there — and
  the ceiling that bounds it stopped the whole KEY from accepting alternate
  writes. That could not be closed from this side, and the substrate's own
  unlink-on-prepend closes it at the current Cyclone pin: the storage layer
  unlinks the superseded same-id node during the chain walk the write already
  performs, so depth stays at one even when this side's mitigation is bypassed
  entirely. The write-time reset at the ceiling is NARROW and must not be read
  as a general self-heal: it fires only when every id the walk saw is the id
  being written, so a key that also holds an original -- the ordinary shape for
  an optimized URL -- still gets `TooManyAlternates` and needs the
  origin-refresh clear instead.
  The writers keep measuring rather than assuming, and the mitigation below
  stays, because the substrate's splice is BEST-EFFORT by contract: one that
  cannot proceed safely is deferred rather than failing the write, so a
  superseded node can still be left behind under contention or a racing wrap.
  What changed is the expected reading — the `*SupersededObserved()` counters
  should now sit at zero, and a nonzero one means the substrate deferred a
  splice, not that the accumulation is unbounded. An unlink whose error means
  "still linked" (`UnlinkLeftEntryLinked`) is still counted and logged rather
  than discarded — the store proceeds, because a current entry beats none.
- Every writer whose entry means "the current one for this URL" carries the
  mitigation, and they share ONE implementation of it:
  `UnlinkSupersededAlternate` (evict RAM + unlink + classify the failure) and
  `CountChainNodes` (the measurement). That is the two dedicated writers
  (`0x6C`, `0x0C`) and the generic ones (`WriteSentinel`, `WriteAgentAlternate`
  — content hash, llms.txt, agent markdown, and the rest), the latter wired
  through by #1312. The writers differ only in WHEN they measure, and the
  difference is forced rather than chosen: the
  sidecar owns its whole write, so it counts after the commit
  (`HeadersSidecarSupersededObserved()`); the other three are STREAMING
  writes whose commit belongs to the caller, so they count what the previous
  store left, on arrival (`OriginalsSupersededObserved()`,
  `SentinelsSupersededObserved()`, `AgentMarkdownSupersededObserved()`). The
  unlink can never move after the commit for any of them: the substrate
  prepends and `remove_alternate_sync` unlinks the FIRST matching node, which
  post-commit is the one just written.
- The sidecar carries a payload FORMAT VERSION byte, not a metadata prefix. So
  `ReadResult::content()` is the wrong accessor for it (it strips a prefix this
  class does not have): use `PageSpeedCache::HeadersSidecarPayload(result)`,
  then `ParseHeadersSidecar`. A payload whose version this build does not know
  is a MISS, never a best-effort parse.
- **The option-context signature is the KEY dimension the design reserves — and
  the machinery is INERT today.** Nothing in the engine calls the 4-arg
  `ComposeKeyPreNormalized`: no optimized output varies with a resolved
  configuration, so the worker accepts a valid non-default context and stores
  the work under the DEFAULT context (`Worker::AcceptOptionContext`, and
  `lib/classify/CLAUDE.md`). The rest of this bullet is why separation, when it
  is needed, is keyed rather than filtered — which is the part that is
  expensive to get wrong later. **The alternate budget is why.** An alternate
  is addressed by `(CacheKey, one byte)`. All eight bits of
  that byte are the capability mask, and `PageSpeedSelector` deliberately reads
  nothing but that byte (it does not trust per-alternate headers). A key holds
  at most 64 alternates (`kMaxAlternatesPerKey`), of which the published budget
  already accounts ~44 for a busy image URL. So a per-alternate context
  dimension is not merely inelegant, it does not fit: two live contexts on one
  image URL would exceed the ceiling and the whole KEY would start refusing
  alternate writes. Keying instead costs **zero chain depth** — each context is
  its own key with its own independent budget — and it makes exact match
  structural rather than a rule to enforce. What it does cost is **key
  cardinality and volume**, proportional to the number of distinct live option
  contexts; a per-URL operation addressed through the default-context key
  (purge, remove, listing) therefore acts on the default context only, which is
  unreachable today because nothing in-tree writes a non-default context, and is
  a prerequisite of the change that starts to. `test/lib/cache/option_context_key_test.cc`
  asserts all of this, including that the default context yields the historical
  key BYTE FOR BYTE so that no existing cache goes cold.
- Both nginx and worker MUST set `enable_mmap_directory = true` for cross-process visibility.
- RAM cache defaults to 64MB (per-process CLFUS). Set `ram_cache_size=0` to disable.
- `ComposeKey()` is private -- keys are always composed from `(url, hostname)` pairs.
- `max_metadata_size` is DERIVED, not chosen: it defaults to
  `AlternateMetadata::kRecommendedMaxMetadataSize`, which comes from the metadata
  format's worst case plus a stated reserve (see the prefix-budget block in
  `lib/classify/alternate_metadata.h`, and the `static_assert` in `cache.h` that
  binds the two). A write whose metadata exceeds it is refused WHOLE — the entry is
  simply not stored, which in the field is indistinguishable from a cache that never
  warms. Lower it only with that failure mode in mind.
