# The `ps_*` C ABI: stability rules and how they are enforced

`lib/pagespeed` is a **shipped, public** library. It is loaded by front ends and
by the .NET package, and those consumers are built at a different time, by a
different toolchain, against a different copy of `pagespeed.h` than the one in
this tree. Everything below exists because of that gap: a change that is
obviously fine when you can see both sides at once is not fine when one side
was compiled a year ago.

There is no "unstable until 1.0" window left to spend. The rules apply now.

## What is part of the ABI

- Every function declared `PS_EXPORT` in `pagespeed.h`, and its exact signature.
- Every published `struct`: its size, and the offset of every field in it.
- Every published `enum` value, and every `#define`d constant that crosses the
  boundary (`PS_CC_ORIGIN_*`, `PS_FLAG_*`, `PS_SENTINEL_*`).
- The three export lists — `symbols.lds` (Linux), `symbols.exp` (macOS),
  `symbols.def` (Windows). A function declared in the header but missing from
  an export list **does not exist** on that platform: it links, it ships, and
  it fails at `dlsym` time on the consumer's machine.

## Versioning

`PS_API_VERSION_{MAJOR,MINOR,PATCH}` in `pagespeed.h`.

| Change | Bump |
|---|---|
| Remove or rename an exported symbol | MAJOR |
| Change an exported function's signature | MAJOR |
| Move, resize or reinterpret a field in a published struct | MAJOR |
| Change a published constant's value | MAJOR |
| Add an exported symbol | MINOR |
| Append a field to a struct that carries `struct_size` | MINOR |
| Anything invisible from outside | PATCH |

On Linux the export list is a versioned symbol map: symbols first exported at
1.2 live in the `PAGESPEED_1.2` node, which inherits `PAGESPEED_1.0`. New nodes
are additive — a consumer linked against the older node keeps resolving to
exactly the symbols it was built against.

`DT_SONAME` is `libpagespeed.so` for the whole 1.x series. A MAJOR bump changes
it, which is what allows a 1.x and a 2.x consumer to coexist on one host
instead of one of them silently getting the wrong library.

## Growing a struct

Published structs carry `struct_size` as their first field. The **caller** sets
it to `sizeof` as the caller's build sees it, and the library reads only that
many bytes. That is what makes both directions work: an old caller against a
new library gets zeros for fields it never heard of, and a new caller against
an old library has its tail ignored.

Two rules make it hold:

1. **Append only.** Never insert, never reorder, never widen in place. A field
   added at the end costs nothing; a field added in the middle silently
   re-points every field after it for every consumer that has not recompiled.
2. **Zero means "not supplied", and must be safe.** A caller that never heard
   of a field passes zero for it, so zero has to mean the old behaviour. If the
   natural zero would mean something dangerous, the field needs a different
   encoding — not a comment telling people to set it.

A related trap, worth stating because it is easy to walk into: an
`..._init(&s)` function that clears `sizeof(*s)` writes the **library's** idea
of the size into the **caller's** buffer. The moment the struct grows, that
initializer runs off the end of every consumer that has not been rebuilt.
Take the size as an argument instead — `ps_write_params_init_sized`,
`ps_freshness_config_init_sized` — and use the `_auto` macros at the call site
so the size comes from the caller's own type and cannot drift:

```c
ps_write_params_init_auto(&params);       /* not ps_write_params_init */
ps_freshness_config_init_auto(&config);
```

`ps_write_params_init` stays because binaries are already built against it. It
clears only the prefix that existed when it was the only option, which is why
it is safe for those callers and why it silently ignores every field added
since — see its header comment.

**Resolution (PS_API 1.9).** The three legacy initializers now follow the rule.
`ps_cache_config_init`, `ps_html_config_init` and `ps_critical_css_config_init`
write their PS_API 1.8 prefix (48, 128, and 88 bytes respectively), pinned by
constants so future struct growth forces a developer decision. Sized entry
points `ps_html_config_init_sized` and `ps_critical_css_config_init_sized`
complete the set alongside `ps_cache_config_init_sized`. Any NEW struct takes
the size as an argument from the start.

## Constraints that are part of the contract, not just the signature

A signature says what you may call. It does not say what the library is
serialising against, and an embedder cannot infer that from a header. Where an
entry point has a concurrency or lifetime constraint that the caller has to
honour, it is documented **on that entry point** in `pagespeed.h`, in the same
place a reader is already looking, and it is treated as part of the ABI: it may
not be weakened without the same care as a signature change, because a caller
built against the promise is exactly as broken by removing it.

The first example is `ps_cache_write_original`, which carries two such
constraints. The engine's purge fencing is
internal to the optimizer process — it stops the optimizer's own tasks from
writing across a purge, and it cannot see an embedder's write at all. So an
embedder-written durable original is not fenced against a concurrent purge of
the same URL, and the header says so rather than leaving the caller to assume
the engine's guarantee extends to them. Anything that later closes that gap is
a *strengthening*, which is always safe to ship.

The second is its **replacement scope**, recorded for the same reason the
sidecar's is (below): re-recording replaces for a single writer per URL, a
read always returns the most recent original, and concurrent writers can
accumulate superseded originals bounded by the storage layer's chain ceiling.
It is stated narrow deliberately, so that the storage advance which makes
replacement unconditional is a strengthening. Two things are specific to this
class and belong in the contract rather than in a release note: a superseded
node here holds a whole response body, so accumulation costs volume as well as
depth — and the retained superseded copies are the OLDEST ones, reclaimed
only by normal volume eviction; and because the write is STREAMING, the previous original is removed
when the call is made while the new one exists only at close, so an abandoned
stream, one whose close the storage layer refuses (chain ceiling, out of
space), or a size-capped stream leaves the URL with no original. That window
is a miss, never wrong bytes — but a caller that would rather keep a stale original has
to check for one itself.

`ps_cache_write_headers_sidecar` carries the same purge constraint and three
of its own.

**The admission decision is the library's, not the caller's.** It takes the
response's complete header set and decides what may be stored, because the
thing being decided — whether a header's value depends on the request — is not
safe to re-decide per consumer. An embedder cannot widen it, and a future
version narrowing what it accepts is not a signature change but IS a behaviour
change a caller can observe, so it is treated as part of the contract.

**Replacement is scoped to a single writer per URL, and the scope is stated
because it can only ever be widened.** A read always returns the most recent
block; with one writer per URL a re-store replaces; with concurrent writers
superseded blocks can accumulate, bounded by the storage layer's chain
ceiling. This is the honest shape of what ships, and it is written that way
deliberately: a promise published as unconditional and narrowed later is
exactly what this document forbids, whereas the reverse — the next
storage-layer advance making replacement unconditional — is a strengthening
and is always safe to ship.

**The payload's format-version range is part of the ABI, not an internal
detail.** Embedders read this class with `ps_cache_read_alternate` +
`ps_read_content` and must receive the *whole* blob, first byte included. They
do, because entry metadata and this payload cannot be confused: entry-metadata
versions start at 3, this class's format version is constrained below it
(asserted at compile time in `lib/cache/headers_sidecar.cc`), so the metadata
parse never consumes a prefix of a sidecar payload. That constraint is the
reason the C surface needs no separate payload accessor — and therefore it may
not be relaxed without either giving the class one or breaking every consumer
that reads it the documented way. It costs the class one spare version value
(2); a class that needs a third takes a new id, which is cheaper than the
alternative.

## The gate

`tools/ci/check_abi.py`, run by the `abi-compat` CI job. It is checkout-only:
no build, no Bazel, no Docker.

It checks four things:

1. **Export-list consistency.** The set of `PS_EXPORT`ed functions in
   `pagespeed.h` equals the set in each of the three export lists. This is the
   check that would have caught four functions shipping unexported.
2. **The golden matches the header.** `abi/abi-golden.json` records the current
   symbol set **with each symbol's ELF version node**, the version triple, the
   `DT_SONAME`, the prefix `ps_write_params_init` clears, and every published
   struct's size **and every field's offset**. It is updated deliberately, in
   the same change that alters the ABI, with `tools/ci/check_abi.py --update`.
3. **Struct layouts are pinned.** The sizes *and offsets* in the golden must
   agree with the `static_assert`s in `test/lib/pagespeed/abi_layout_test.cc`,
   which are what actually fail the build — on every platform and every
   architecture CI compiles for, which is where an alignment difference would
   show up. Sizes alone are not enough: swapping two same-width fields leaves
   the size untouched and silently re-points every consumer's reads, so a
   struct pinned by size with no offset assertions fails the gate outright.
4. **The .NET mirrors still match.** `NativeStructs.cs` hand-mirrors a prefix of
   `ps_write_params_t` and all of `ps_cache_config_t` as explicit byte offsets,
   and nothing links the two files. The gate parses the C# and asserts every
   mirrored offset against the golden, and asserts `NativeWriteParams` is
   exactly the prefix `ps_write_params_init` clears — the number the whole
   prefix-only design rests on.
5. **Compatibility against the base branch.** Given the golden as it exists on
   the merge base (`--compat-base`): a removed symbol, a removed or **moved**
   field, a symbol changing version node, a shrunk struct or a changed soname
   demands a MAJOR bump; an added symbol or a grown struct demands a MINOR
   bump. Landing an incompatible change without moving the version is what
   fails.

To see it fail on purpose — worth doing after touching the gate itself — try
any of these and run `python3 tools/ci/check_abi.py`:

- delete or rename an exported symbol (surface check, then the compat check
  once the golden is regenerated);
- swap two same-size field offsets in `abi_layout_test.cc` (golden↔pin
  mismatch, then the compat check's moved-field rule);
- move a symbol between `PAGESPEED_1.0` and `PAGESPEED_1.2` in `symbols.lds`;
- change `Size = 48` on `NativeWriteParams`, or one of its `FieldOffset`s;
- drop `-Wl,-soname,` from `lib/pagespeed/BUILD`.

The self-test (`--self-test`) drives every decision branch and reports a
**counted** number of cases, not a literal — a hardcoded coverage count is the
thing most likely to rot into a comfortable lie.
