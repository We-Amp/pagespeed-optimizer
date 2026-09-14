# JavaScript Library (VENDORED — single-writer)

JS tokenization and minification. Namespace: `pagespeed::js`.

## This code is vendored — never hand-edit it

The JS kernel has ONE canonical source: the mod_pagespeed
1.15 repository (`pagespeed/kernel/js/` there). The files in this directory
are generated from it:

- **Vendored (single-writer, generated):** `js_tokenizer.{h,cc}`,
  `js_minify.{h,cc}`, `js_keywords.{h,cc}`, `source_map.h`, everything
  under `corpus/`, and the ledger `SYNCED_FILES.txt` (the sorted list of
  every file the sync produced). Sources carry a `GENERATED — DO NOT EDIT
  BY HAND` banner. Any fix belongs in the canonical 1.15 tree first; it
  reaches this repo by re-pinning + re-syncing. Hand edits are never
  sanctioned — the drift workflow rejects them.
- **2.0-owned (editable):** `compat/` (adaptation headers: StringPiece /
  GoogleString aliases, DCHECK/LOG(DFATAL), RE2 glue), `BUILD`,
  `corpus/BUILD`, `JS_KERNEL_PIN`, `LICENSE.apache-2.0`, this file, and
  `js_minify_fuzz.cc` — the dual-oracle fuzz harness, an optimizer-native
  MANUAL MIRROR of the canonical `pagespeed/kernel/js/js_minify_fuzz.cc`
  (mirrored at mpp commit `3938c4352`, mpp #688; the
  `lib/html/html_fuzz.cc` precedent). It is NOT in `SYNCED_FILES.txt` and
  the sync tool does not carry it; the sync's wholesale install step
  excludes it explicitly by name (its `js_*.cc` removal glob would
  otherwise delete it — the mirror is the one 2.0-owned file matching the
  vendored name shape). Divergence from canonical is limited to
  the include lines (the sync tool's rewrite table) and the header's repo
  references. Logic changes land canonically first, then are re-mirrored.
- **Membership is explicit:** `tools/check-js-kernel-membership.sh` (run by
  the `js-kernel-differential` CI job on every PR and by the sync tool's
  `--check`) fails if any file exists under `lib/js/` that is neither
  listed in `SYNCED_FILES.txt` nor in the declared 2.0-owned set — a
  planted file named into the vendored shape cannot ride the carve-outs.

## How the sync works

- **Pin:** `lib/js/JS_KERNEL_PIN` holds the canonical 1.15 commit SHA
  (first token of the first non-comment line — that contract is load-bearing
  for every consumer of the file). A second, keyword-keyed line
  `floor <sha>` records the pin-downgrade ratchet floor (see below).
- **Sync:** `tools/sync-js-kernel.sh --source <path-to-1.15-checkout>`
  regenerates the vendored set from the pin (include-path rewrites to
  `lib/js/` + `lib/js/compat/`, provenance banner; corpus files
  byte-for-byte; no other source rewrites — a residue check fails the sync
  if the canonical source reintroduces the retired `AppendToString`
  member call). Idempotent; `--check` mode byte-compares without writing
  (used by CI).
- **To update:** bump the SHA in `JS_KERNEL_PIN`, move the `floor` line to
  the same new SHA, re-run the sync tool, and commit it all together (plus
  any golden diff that came with the canonical change).
- **Pin-downgrade ratchet (#1104):** the ancestry gate alone accepts any
  ancestor of canonical master, so a pin downgrade to an older-but-complete
  commit would still produce a self-consistent tree. The `floor <sha>` line
  in `JS_KERNEL_PIN` closes that window. Exactly what is enforced where:
  - the sync tool fails (exit 6) unless the pin equals the floor or has it
    as an ancestor (and requires exactly one floor line);
  - the REQUIRED `js-kernel-differential` job runs
    `tools/ci/check-floor-ratchet.sh`: floor validity (exactly one valid
    line — deletion/duplication cannot merge) and forward-only enforcement
    vs origin/main; equality needs no canonical access, an actual floor
    MOVE verifies ancestry in the canonical repo and fails closed if it is
    unreachable;
  - the drift workflow's RATCHET step runs the same script as the
    advisory, path-filtered full diagnosis.
  Availability dependency (#1117): a floor MOVE fails closed when the
  canonical 1.15 repo is unreachable from the runner, so a pin bump adds
  a hard availability dependency — with a broken deploy key or an
  upstream outage, pin-bump PRs are unmergeable until reachability is
  restored. Equality (no-bump PRs) needs no canonical access and is
  unaffected. Scope: the required layer verifies floor/pin LINEAGE only,
  never the vendored bytes; byte-vs-canonical verification lives in the
  advisory drift workflow.
  This holds for changes that arrive via PR under the required check. It
  does NOT constrain an actor who edits the workflows themselves or
  `tools/ci/check-floor-ratchet.sh` itself (the required job executes the
  PR's own copy of that script via the `_ci-tools` checkout, so it sits in
  the same trust class as the workflow files), or an admin pushing directly
  to main; repo-settings hardening (CODEOWNERS on `.github/` +
  `lib/js/JS_KERNEL_PIN` + `tools/ci/check-floor-ratchet.sh`, strict
  required checks including admins) is a separate operator decision,
  deliberately out of scope here.
  Convention: routine pin bump → floor moves to the new pin in the same
  commit; hotfix-valve pin (unmerged 1.15 branch) → floor STAYS at the
  last merged pin (the unmerged SHA gets orphaned by the 1.15
  squash-merge and must never become the floor) and moves at
  reconciliation.
- **Drift gate:** CI's JS-kernel drift check re-runs the
  sync on PRs touching `lib/js/**`, pushes to main, weekly, and on
  dispatch. Any byte difference is RED.
- **Behavior gate:** the `js-kernel-differential` job in `ci.yml` builds
  `corpus/js_minify_probe` and verifies `corpus/goldens/manifest.json`
  (`gen_goldens.py --check`). The manifest is byte-shared with the 1.15
  repo — one expectation gates both.
- **Hotfix valve:** a pin on a pushed-but-unmerged 1.15 branch is allowed
  only via `SYNC_ALLOW_UNMERGED_PIN` (drift workflow honors it on
  pull_request events when `JS_KERNEL_PIN` carries an `# allow-unmerged`
  marker line). 1.15 squash-merges orphan such SHAs: reconciliation is
  ALWAYS re-pin to the squashed master commit + an expected no-op re-sync.

## Carve-outs (and their limits)

Vendored files keep canonical 1.15 idiom/formatting: they are excluded from
clang-tidy header diagnostics, clang-format, and the pre-commit fixers
(exclusions keyed on the name shape `js_*` / `source_map.h` / `corpus/`,
which the sync tool asserts and the membership gate makes non-spoofable).
They are NOT excluded from the license gate
(the banner carries SPDX; the one documented exemption is `*.js`/`*.mjs`
directly in the two corpus fixture dirs — their sha256 is pinned in the
goldens manifest), nor from
sanitizers, tests, or the differential job. `compat/` is normal 2.0 code:
fully linted, formatted, and reviewed — `test/lib/js/compat_headers_test.cc`
includes every compat header directly so clang-tidy coverage does not
depend on vendored include chains (`compat/logging.h` previously reached
no linted TU at all) (#1104). `js_minify_fuzz.cc` matches the `js_*` name
shape deliberately: it rides the same clang-format/clang-tidy carve-outs
so the manual mirror stays byte-similar to the canonical 1.15 harness
(canonical formatting), but it IS 2.0-owned — membership-gate declared,
license-gated by its own SPDX header.

## Licensing

Vendored kernel files retain their original Apache-2.0 headers beneath the
banner; full license text in `LICENSE.apache-2.0` (hand-maintained, not
sync-generated). That notice deliberately omits the upstream LICENSE's
third-party stanzas (Expat/Zlib/BSD/libbz2/dtoa/ICU): no vendored lib/js
file derives from those components (#1104). If a re-sync ever vendors code
that does, restore the matching stanza.

## Key API

- `js_minify.h` — `MinifyUtf8Js(patterns, input, output)` and
  `MinifyUtf8JsWithSourceMap(patterns, input, output, mappings)` (UTF-8
  aware; declines rather than risk altering ambiguous constructs).
- `js_tokenizer.h` — `JsTokenizer`: streaming token-level JS lexer.
- `js_keywords.h` — reserved-word lookup.
- `source_map.h` — mapping structs only; `source_map::Encode` is
  declared-but-undefined here on purpose (its canonical .cc drags jsoncpp
  and has no 2.0 callers — a link error on first use is the documentation).

## Testing

```bash
bazel test //test/lib/js/...
```

`test/lib/js/` (2.0-owned, editable) includes the ported source-map
mapping-tuple tests — the consumer golden for `MinifyUtf8JsWithSourceMap`.
