# Async-CSS probe

Does deferring a stylesheet leave the page looking right?

The optimizer can make a stylesheet non-render-blocking and inline the CSS the
fold needs, so the page paints immediately and the full sheet arrives later. When
the inlined block really does cover the fold, that is free speed. When it does
not, the visitor sees a flash of unstyled content, and nothing in the existing
test suite notices: the unit tests assert on extraction, and the HTTP compliance
suite asserts on protocol behaviour. Neither asks the question this harness
exists for.

Two lanes, one fixture, one compose stack:

| Lane | What it asks | Where it runs | Blocking |
|---|---|---|---|
| **Markup** (`test_async_css_markup.py`) | Did the optimizer emit correct deferral markup? | `async-css-probe-markup` job in `ci.yml`, every PR | No — soaking |
| **Rendered** (`probe_rendered.mjs`) | Does the fold *look* the same before the sheet applies? | `async-css-probe-nightly.yml`, 05:05 daily | No — advisory |

The stack is a sibling of `tools/http-compliance/`, not a modification of it: the
same origin / worker / nginx trio over a shared volume, but serving the
byte-frozen `modpagespeed.com` capture in `fixtures/` and keeping the worker's
critical-CSS pass, which the compliance worker deliberately disables.

## Running it

```bash
# All three modes, building the worker and module from source
./tools/async-css-probe/run_probe.sh

# One mode, against pre-built CI artifacts (much faster)
./tools/async-css-probe/run_probe.sh --prebuilt --mode forced

# Pass anything else through to pytest
./tools/async-css-probe/run_probe.sh --prebuilt -k loader
```

## The deferral primitive lives in one file

`primitive.json` holds how the optimizer makes a stylesheet non-render-blocking:
`rel="preload" as="style"`, with the real media parked in
`data-pagespeed-media` and a loader that flips `rel` back to `stylesheet`.
**Both** lanes read it — the markup lane through `conftest.py`, the rendered
lane through `probe_rendered.mjs` — so a change to the primitive is one edit
here and both lanes record the swap instead of being rewritten around it.
`primitive_attr`/`primitive_value` is the single attribute the rendered lane
sets to put an applied sheet back to not-applying; `primitive_as` is the
companion the markup lane asserts, and is what makes the preload match the
consumer the loader creates (a wrong `as` downloads the sheet twice).
`early_hint_promoted` is `true`: the primitive is itself a preload, so an Early
Hint promotes exactly what the transform promoted.

## The three modes

Both knobs are startup flags, so each mode is a separate stack:

| Mode | Worker argv | Asserts |
|---|---|---|
| `gated` | coverage floor raised to 0.95 | Nothing is deferred; the loader is not injected; critical CSS is still inlined |
| `forced` | floor 0.95 **+** `--unsafe-force-async-css` | It defers, and every part of the deferred markup is correct |
| `shipped` | stock | Records what the product does on this page today |

`gated` and `forced` differ by exactly one flag. That pair is what makes the
diagnostic switch's effect provable, and what shows the gate — not luck — is
what suppresses deferral in `gated`. A single mode would show neither.

Since deferral started requiring a validated profile, the raised floor in
`gated` is no longer the *only* thing refusing that page — this stack has no
browser, so no page is ever validated. The mode is kept anyway: it is the
control that keeps `forced` meaningful (identical argv apart from the switch),
and it stays honest the day a validated profile does reach this fixture.

One assertion in the markup lane is carried but **not exercised** by this
fixture: the `<noscript>` twin must preserve `integrity` and `crossorigin`, and
the captured page's stylesheet has neither, so that branch never runs here. It is
covered at unit level (`html_transform_filter_test.cc`, including the bare
valueless `crossorigin` case). Exercising it end to end needs a second,
SRI-bearing stylesheet in the fixture — worth adding whenever the capture is next
refreshed.

`shipped` is a characterization test, not a wish. This page used to defer under
stock flags: the extractor produces ~25 KB of critical CSS against the fixture's
~115 KB sheet, which clears the 0.10 byte-ratio floor, so deferral was permitted
although nothing had checked that the inlined block covers the fold. That was the
defect the wider effort is about. Deferral now additionally requires a validated
profile whose stylesheet hash still matches the sheet being served; this fixture
never acquires one, so the stock-flag answer is "render-blocking, critical CSS
still inlined". `DEFERS_WITHOUT_A_VALIDATED_PROFILE` in `conftest.py` records
which of the two is expected, and the shipped-mode test pins it either way.

## `--unsafe-force-async-css`

CLI-only, undocumented, and never to be used on production traffic. It exists so
this harness can exercise deferred markup while the sufficiency gate is, quite
correctly, refusing to produce any. See `src/worker/unsafe_force_async_css.h` for
why it is not a config-file key, and expect a loud warning on stderr whenever a
worker starts with it.

## Warm-up, and why it is not incidental

Two ordering facts decide whether this harness measures anything real:

1. **The stylesheet must be cached before the page is first optimized.** Until it
   is, the worker derives its critical block from the page's own inline CSS
   alone and stores that. The result reads like a product bug and is a warm-up
   race. So readiness is probed on the stylesheet, never on the page under test.
2. **`X-PageSpeed: HIT` does not mean "the optimizer has run".** The front end
   stores and serves the origin bytes while the worker optimizes out of band, so
   a HIT arriving 0.3s in is routinely the unmodified page. Asserting on it reads
   as "the optimizer did nothing" — the most misleading way this harness can
   fail. The optimizer's output is by definition not the origin's bytes, so the
   fixtures wait for exactly that.

The rendered lane has a third: the front end stores one variant per request shape
(`Vary: Accept-Encoding, User-Agent`), so the browser must warm its *own* variant
by navigating repeatedly. Warming with `fetch()` alone warms a variant nobody
measures. See "Warm-up (rendered lane)" below for what that costs if skipped.

## Golden screenshots

`goldens/fold-{mobile,tablet,desktop}.png` are the fully-styled renders at the
three profile viewports (`browser_analysis_manager.h`,
`AnalysisContext::kViewportWidths` / `kViewportHeights`). The rendered lane diffs
the critical-CSS-only render against them, using the product's own comparison:
a pixel differs when any channel differs by more than `kChannelTolerance = 2`,
and the verdict is the differing-pixel ratio against `kDefaultThreshold = 0.005`
(`src/browser/visual_regression_gate.h`). Lane and product agree by construction.

**Regenerate only inside the pinned image**, or the goldens encode a different
Chrome's rasterization:

```bash
# with the stack already up in forced mode; NET is the compose network of that
# stack (run_probe.sh names the project async-css-probe-$$)
NET=$(docker network ls --filter name=async-css-probe --format '{{.Name}}' | head -1)
docker run --rm --network="$NET" \
  --user "$(id -u):$(id -g)" -e HOME=/tmp -e npm_config_cache=/tmp/.npm \
  -v "$PWD/tools/async-css-probe":/probe -w /tmp \
  -e PROBE_BASE_URL=http://nginx:8080 \
  -e PROBE_GOLDEN_DIR=/probe/goldens \
  -e PROBE_PRIMITIVE=/probe/primitive.json \
  -e ASYNC_CSS_PROBE_REGENERATE_GOLDENS=1 \
  mcr.microsoft.com/playwright:v1.58.2-noble \
  bash -c 'npm install --no-save --no-audit --no-fund --silent playwright@1.58.2 pngjs@7 >/dev/null 2>&1 \
           && cp /probe/probe_rendered.mjs /tmp/ && node /tmp/probe_rendered.mjs'
```

The image tag is pinned in `async-css-probe-nightly.yml` and must match the one
the goldens were generated in. Regenerating is a deliberate, reviewable change:
a golden refreshed to match a regression hides the regression.

## Promotion criteria

Both lanes land non-blocking on purpose, and each has an explicit bar to clear.

**Markup lane → the required-checks gate**: 20 consecutive green runs on
unrelated PRs, p95 runtime under 8 minutes, and zero infra-only reds. Promotion
is one line added to `required-checks-gate.needs` in `ci.yml`. The lane already
runs unfiltered on every PR, because the gate fails on `skipped` — path-filtering
and gate membership are mutually exclusive.

**Rendered lane → blocking**: the Playwright image pinned **by digest**, the
goldens regenerated inside that pinned image, and 30 consecutive clean nightlies.
Until then a finding opens or refreshes one tracking issue
(`async-css-probe-finding`) and uploads the `async-css-probe-evidence` artifact;
the next clean run closes it. A cross-Chrome-version pixel diff on a blocking
check is a flake generator.

## Rendered lane: how the flash window is reconstructed

The obvious method — intercept the deferred stylesheet, hold the response, and
screenshot while the browser genuinely sits in the flash window — was implemented
and does not work here, for one measured reason: **enabling route interception
changes which variant answers.** Responses vary on `Accept-Encoding` and
`User-Agent`, and the intercepted navigation is served the origin-bytes variant,
never converging on the deferred one however long it is warmed. Everything
measured under interception was measured on the wrong document.

So the lane loads the page normally, screenshots it fully styled, then puts the
deferred `<link>`s back to the non-applying primitive — undoing exactly what the
loader did — and screenshots again. Same DOM, same subresource state; the only
difference is that the deferred stylesheet does not apply. That is the CSS state
of the flash window, reconstructed rather than observed: exact in the dimension
being measured, and free of the variant divergence that makes the observed
version unusable.

### Fonts and fold images are in the fixture

They were not, and the gap was the lane's largest. The capture now carries the
three web fonts and the two logos the fold renders with, so the deferred
stylesheet's `@font-face` rules resolve against real files and the
reconstructed flash window shows text falling back to system metrics — the
resource-dependent flash, which **only this lane will ever cover**: the
in-product validation gate blocks every subresource in its render as an SSRF
defence, so it is blind to this class permanently and by design.

Two determinism pins came with those assets, and neither is emulation for its
own sake. `reducedMotion: "reduce"`, because the captured logo SVG animates a
blinking cursor inside `<img>` and would otherwise put a few dozen coin-flip
pixels into every diff; the SVG honours the preference itself. And a bounded
wait on `document.fonts.ready` before each screenshot, because a shot taken
mid-swap is a fallback-font render that reads as exactly the flash this lane
hunts.

### What a green rendered run does not establish

* **Only the stylesheet's absence is reconstructed.** A flash caused by ordering,
  or by anything that exists only in the real timing window, is out of scope by
  construction. The fonts are in the browser cache by the time the flash state
  is reconstructed, so what is measured is the fold losing its `@font-face`
  *declarations*, not a font still on the wire.
* **No `@import`.** The captured stylesheet has none, so a flash caused by a
  nested import chain is not exercised here.
* **Layout established by JS is not covered.** The page's island bundles are
  deliberately not in the capture (`/_astro/QuickMessage*.js` and `/api/geo`
  still 404 — behaviour and data, not fold paint), so anything the fold's
  geometry owes to script is absent from both screenshots equally.
* **The measured configuration never ships.** Deferral is forced past the
  sufficiency gate so there is deferred markup to render at all; the findings
  file records the worker argv it measured against.

> The nightly workflow's header comment still states the old "every font and
> fold image 404s" limitation. It is stale as of this change and corrected on
> the next PR that touches the nightly probe lane;
> this file and `probe_rendered.mjs` are the current text.

### Warm-up (rendered lane)

The lane pre-warms **once**, before the viewport loop, with a 180-second budget,
and it warms over **plain HTTP** — the markup lane's exact sequence — before
confirming through the browser.

That ordering is the finding, and it is not the obvious one. The optimizer
produces one variant per capability mask, and on a cold stack **the browser's own
repeated navigation does not converge**: measured flat at the origin's bytes for a
full 180 seconds, no deferred markup ever appearing. Warm over plain HTTP first
and the browser is served the deferred page on its *first* navigation, in under
two seconds. Warming through the browser alone would have filed a false finding
every night, since the nightly always starts from `down -v`.

A warm failure is an infrastructure outcome: it goes in its own section of the
findings file, never into the product list, because it is not evidence about the
fold.

## Fixture

`fixtures/modpagespeed-com/` is a byte-frozen capture — the HTML, its
stylesheet, and the fold's three web fonts and two logos. `CAPTURE.md` records
each file's URL, date, byte size, sha256, and the evidence that the capture is
origin-shaped rather than already-optimized. It is never fetched live: a live
fetch would make the lane's verdict a function of whatever shipped that morning.
Refreshing it is a deliberate, reviewable change — and one that moves the fold,
so it means regenerating the goldens in the same change.

The directory **mirrors the site's URL space**: a file at `<fixture>/a/b` is
served at `/a/b`, `index.html` at `/`, and anything not on disk is a 404.
`probe_origin.py` builds its route table from that walk at startup, so adding a
subresource to a future capture is a file drop, not a code change.
