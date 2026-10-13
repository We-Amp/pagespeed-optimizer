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

The HTML cacheability policy entry points (`ps_html_store_classify`,
`ps_html_serve_classify`, `ps_html_decision_format`,
`ps_html_store_reason_name`, `ps_html_serve_reason_name`, PS_API 1.12) are pure
functions of their arguments, and that is part of the contract: they take no
cache handle, perform no I/O, read no clock (the caller passes the time), keep
no state, and never touch the last-error message. No struct crosses the
boundary, so there is no layout to pin: inputs are scalars and the same
parallel `names`/`values` arrays the sidecar takes, outputs are out-parameters
and caller buffers.

**The numeric values are the contract.** The action returned (0 refuse or fall
through, 1 store or serve HTML, 2 store without notify or serve Markdown, 3
validate), the reason integers (the closed name sets returned by
`ps_html_store_reason_name` and `ps_html_serve_reason_name`), the flag bits on
both sides, and the mode values 0 to 3 are all part of the ABI: a caller
compiled against them reads them forever. A new reason is appended at the end
of its set and is a MINOR change; a caller must treat a reason it does not know
as "refused" or "fell through", which is how the library itself treats an
out-of-range value it is handed (`NULL` from the name functions). Reordering or
renumbering is a MAJOR change.

**The policy's strictness is the library's, not the caller's.** A page is stored
only on an explicit lifetime or a validator, never on a heuristic one; a
request with credentials, or with `Cache-Control: no-store`, is never stored;
a request with a cookie is stored only when the response is explicitly
`public` or carries `s-maxage`; a response that sets a cookie, `private` or
`no-store`, a `Vary` beyond `Accept-Encoding` and a body carrying a
per-client marker are never stored; two `max-age` (or two `s-maxage`)
directives count as lifetime 0; a stale entry is never served. The
`request_flags` word of `ps_html_store_classify` carries the request facts
(1 has authorization, 2 authenticated user, 4 has cookie, 8 request
no-store), as the serve side's word does. None of that
is configurable per call, for the reason the sidecar's allowlist is not: what
makes a page personal is not safe to re-decide per consumer. Narrowing it later
is observable and is treated as a contract change. `enforce` in the serve
verdict is likewise the library's statement of whether a caller is meant to act
on the verdict at all: it is 0 in the off and report-only modes, where the
action is only what would happen.

**The decision line's grammar is the contract too.** `ps_html_decision_format`
takes a closed key set and, per key, a closed value grammar (names, counts,
RFC 9110 tokens for a header name or Vary token; the header documents the
table) and refuses anything else with -2, so a caller can rely on the line
never carrying a header value or a cookie whatever it was handed. Widening a
grammar (a new action or reason name) is a MINOR change; narrowing one is
observable and is treated as a contract change.

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
