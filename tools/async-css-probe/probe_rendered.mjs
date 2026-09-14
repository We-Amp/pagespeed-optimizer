// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Rendered lane (Lane B): does the fold look finished before the deferred
// stylesheet applies?
//
// The markup lane proves the transform emitted the right bytes. It cannot prove
// the page LOOKS right while the deferred stylesheet is still in flight, which
// is the failure this whole effort exists to prevent. This lane renders the
// served page in a real browser and measures it.
//
// METHOD, per viewport:
//   1. Load the served page normally and let it finish. The loader has run, the
//      full stylesheet applies. Screenshot: this is the reference, and the
//      committed golden.
//   2. Put the deferred <link>s back to the non-applying primitive — undoing
//      exactly what the loader did — and screenshot again. The DOM is identical,
//      every subresource is in the same state, and the only difference is that
//      the deferred stylesheet does not apply. That is the CSS state of the
//      flash window.
//   3. Diff. A page whose critical CSS really covers the fold looks the same in
//      both; a difference is the flash.
//
// WHY NOT HOLD THE RESPONSE. The obvious method — intercept the stylesheet and
// hold it, so the browser genuinely sits in the flash window — was implemented
// and does not work here: enabling route interception at all changes the request
// the front end sees (Vary: Accept-Encoding, User-Agent), so the intercepted
// navigation is answered by a different cached variant — the origin-bytes one —
// and never converges on the deferred page no matter how long it is warmed.
// Everything measured under interception was therefore measured on the wrong
// document.
//
// Step 2 reconstructs the flash window's CSS state instead of observing it. The
// reconstruction is exact in the dimension being measured and free of the
// variant divergence that makes the observed version unusable.
//
// FONTS AND FOLD IMAGES ARE PRESENT, and that is on purpose. The fixture
// capture carries the three web fonts and the two logos the fold renders with
// (fixtures/modpagespeed-com/CAPTURE.md), so the deferred stylesheet's
// @font-face rules resolve against real files and the reconstructed flash
// window shows what a visitor would see: text falling back to system metrics,
// not text that was never webfont-styled in either screenshot. This lane is the
// ONLY place that class of flash is ever covered — the in-product validation
// gate blocks every subresource in its render as an SSRF defence, so it is
// blind to it permanently and by design.
//
// Two things are pinned below because those assets brought their own
// nondeterminism, not because the lane wanted more emulation: `reducedMotion`
// (the logo SVG animates a blinking cursor, and honours the preference itself)
// and a wait on `document.fonts.ready` before each screenshot (a shot taken
// mid-swap is a fallback-font render that reads as a flash).
//
// NAMED LIMITATIONS. What a green run here does NOT establish:
//
//   * Only the stylesheet's absence is reconstructed. A flash caused by ordering,
//     or by anything that happens only in the real timing window, is out of
//     scope by construction. In particular the fonts are already in the browser
//     cache when the flash state is reconstructed, so this measures the fold
//     losing its @font-face DECLARATIONS, not a font still on the wire.
//   * No @import in the fixture. The captured stylesheet has none, so a flash
//     caused by a nested import chain is not exercised here.
//   * Layout established by JS is not covered. The page's islands do not run
//     against the fixture (their bundles are deliberately not captured), so
//     anything the fold's geometry owes to script is absent from both
//     screenshots equally.
//   * The measured configuration is not one that ships (see WORKER_ARGV below):
//     deferral is forced past the sufficiency gate so that there is deferred
//     markup to render at all.
//
// The comparison mirrors VisualRegressionGate::CompareScreenshots
// (src/browser/visual_regression_gate.{h,cc}) exactly: a pixel differs when any
// channel differs by more than kChannelTolerance = 2, and the verdict is the
// differing-pixel ratio against kDefaultThreshold = 0.005. Lane and product
// agree by construction; when PR-C settles on a measured threshold, THRESHOLD
// below moves with it.
//
// Node rather than Python because the pinned Playwright image ships Node and its
// browsers but no pip — see README.md.
//
// ADVISORY: this never gates a merge. A cross-Chrome-version pixel diff on a
// required check is a flake generator. Its output is an artifact plus a findings
// file the workflow turns into one tracking issue.

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { chromium } from "playwright";
import { PNG } from "pngjs";

const BASE_URL = process.env.PROBE_BASE_URL ?? "http://nginx:8080";
const ORIGIN_URL = process.env.PROBE_ORIGIN_URL ?? "http://origin:8081";
const GOLDEN_DIR = process.env.PROBE_GOLDEN_DIR ?? "goldens";
const OUT_DIR = process.env.PROBE_OUT_DIR ?? "/tmp/async-css-probe-out";
const FINDINGS = process.env.PROBE_FINDINGS ?? join(OUT_DIR, "findings.md");
const REGENERATE = process.env.ASYNC_CSS_PROBE_REGENERATE_GOLDENS === "1";
// Recorded verbatim in the findings file so the auto-filed issue says which
// worker configuration produced the numbers. This lane deliberately measures a
// configuration that never ships.
const WORKER_ARGV =
  process.env.PROBE_WORKER_ARGV ??
  "--unsafe-force-async-css --async-css-min-coverage 0.95";

// The deferral primitive, shared with the markup lane. PR-E edits that one file
// and both lanes record the swap.
const PRIMITIVE_PATH =
  process.env.PROBE_PRIMITIVE ?? join(GOLDEN_DIR, "..", "primitive.json");
const PRIMITIVE = JSON.parse(readFileSync(PRIMITIVE_PATH, "utf-8"));
const DEFERRED_LINK_SELECTOR = `link[${PRIMITIVE.deferred_link_marker}]`;
const PRIMITIVE_ATTR = PRIMITIVE.primitive_attr;
const PRIMITIVE_VALUE = PRIMITIVE.primitive_value;
const SHEET_PATH = PRIMITIVE.fixture_stylesheet_path;

// src/browser/browser_analysis_manager.h — AnalysisContext::kViewportWidths /
// kViewportHeights. The profile is stored per viewport, so the fold has to be
// checked at each one.
const VIEWPORTS = [
  { name: "mobile", width: 375, height: 667 },
  { name: "tablet", width: 768, height: 1024 },
  { name: "desktop", width: 1440, height: 900 },
];

// src/browser/visual_regression_gate.h.
const CHANNEL_TOLERANCE = 2;
const THRESHOLD = 0.005;

// Cold-stack convergence was measured at ~60s on an idle machine, and the
// nightly always starts cold. 180s is the same margin the workflows give
// readiness — generous on purpose, because the alternative is a false finding.
const WARM_BUDGET_MS = Number(process.env.PROBE_WARM_BUDGET_MS ?? 180_000);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Raised for anything that means "this lane did not measure the fold". Kept
// distinct from a product finding all the way to the findings file: an
// infrastructure failure is not evidence about the fold, and reporting it as one
// sends the reader hunting a rendering bug that was never measured.
class InfraError extends Error {}

function compare(goldenPath, candidatePath) {
  const a = PNG.sync.read(readFileSync(goldenPath));
  const b = PNG.sync.read(readFileSync(candidatePath));
  const w = Math.min(a.width, b.width);
  const h = Math.min(a.height, b.height);
  // A zero-size image means a screenshot or a golden failed to capture. Ratio 0
  // would read as a perfect match and quietly clear the lane.
  if (w === 0 || h === 0) {
    throw new InfraError(
      `refusing to compare a zero-size image: ${goldenPath} is ` +
        `${a.width}x${a.height}, ${candidatePath} is ${b.width}x${b.height}`,
    );
  }
  let diff = 0;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const ia = (y * a.width + x) * 4;
      const ib = (y * b.width + x) * 4;
      for (let c = 0; c < 4; c++) {
        if (Math.abs(a.data[ia + c] - b.data[ib + c]) > CHANNEL_TOLERANCE) {
          diff++;
          break;
        }
      }
    }
  }
  return { ratio: diff / (w * h), diff, total: w * h };
}

// Warm ONCE, before any viewport is measured. Without it the FIRST viewport
// spends its whole budget warming and reports "no deferred stylesheet" — on the
// cold stack the nightly always produces, a false finding every night.
async function prewarm(browser) {
  const deadline = Date.now() + WARM_BUDGET_MS;

  // Warm over plain HTTP, in the markup lane's order and for its reasons
  // (conftest.py): the stylesheet must be cached before the page is first
  // optimized, and "X-PageSpeed: HIT" is not "the optimizer has run" — the
  // front end serves the origin's bytes from cache while the worker optimizes
  // out of band, so wait for output that is not the origin's bytes.
  //
  // Plain HTTP rather than the browser, which is the whole finding here: the
  // optimizer produces one variant per capability mask, and on a cold stack the
  // browser's own repeated navigation does NOT converge — measured flat at
  // origin bytes for a full 180s. Warm this way first and the browser is served
  // the deferred page on its FIRST navigation, in under two seconds. Warming
  // through the browser alone would file a false finding every night.
  let sheetHit = false;
  while (Date.now() < deadline) {
    const r = await fetch(`${BASE_URL}${SHEET_PATH}`);
    await r.arrayBuffer();
    if (r.headers.get("x-pagespeed") === "HIT") {
      sheetHit = true;
      break;
    }
    await sleep(250);
  }
  if (!sheetHit) {
    throw new InfraError(
      `the stylesheet was never served from cache within ${WARM_BUDGET_MS / 1000}s.`,
    );
  }

  const originHtml = Buffer.from(
    await (await fetch(`${ORIGIN_URL}/`)).arrayBuffer(),
  );
  let served = null;
  while (Date.now() < deadline) {
    const r = await fetch(`${BASE_URL}/`);
    const body = Buffer.from(await r.arrayBuffer());
    if (r.headers.get("x-pagespeed") === "HIT" && !body.equals(originHtml)) {
      served = body;
      break;
    }
    await sleep(250);
  }
  if (served === null) {
    throw new InfraError(
      "the page was never served as optimizer output within " +
        `${WARM_BUDGET_MS / 1000}s.`,
    );
  }
  if (!served.includes(PRIMITIVE.deferred_link_marker)) {
    throw new InfraError(
      "the optimized page carries no deferred stylesheet. The stack is not in " +
        "the forced mode (README.md).",
    );
  }

  // Confirm through the browser before any measurement: the mask the browser
  // is served must be the deferred one too.
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  try {
    while (Date.now() < deadline) {
      await page.goto(`${BASE_URL}/`, { waitUntil: "load" });
      const deferred = await page.evaluate(
        (sel) => document.querySelectorAll(sel).length,
        DEFERRED_LINK_SELECTOR,
      );
      if (deferred > 0) return;
      await sleep(1000);
    }
  } finally {
    await page.close();
  }
  throw new InfraError(
    "the front end serves a deferred page over plain HTTP but not to the " +
      `browser within ${WARM_BUDGET_MS / 1000}s — a capability-mask variant the ` +
      "optimizer never produced.",
  );
}

// Now that the fixture serves real fonts, a screenshot can land mid-swap: the
// fold drawn in fallback metrics, one frame before the webfont applies. Against
// a golden taken after the swap that is a large diff and reads as exactly the
// flash this lane hunts — a false finding manufactured by timing. Waiting on
// document.fonts.ready removes the race in the only dimension it exists.
//
// Bounded and best-effort. document.fonts.ready settles on its own in every
// state this lane produces, including the reconstructed one where the
// @font-face rules went away with the sheet — but page.evaluate carries no
// timeout of its own, and a lane that can hang forever is worse than one that
// screenshots a frame early. The cap is the whole reason this is a helper and
// not an inline await.
const FONT_SETTLE_CAP_MS = 5_000;

async function settleFonts(page) {
  let timer;
  try {
    await Promise.race([
      page
        .evaluate(() => document.fonts.ready.then(() => undefined))
        .catch(() => {}),
      // Cleared below rather than left to fire: an un-cleared timer holds the
      // event loop open, and this runs six times per lane.
      new Promise((r) => {
        timer = setTimeout(r, FONT_SETTLE_CAP_MS);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function runViewport(browser, vp) {
  const golden = join(GOLDEN_DIR, `fold-${vp.name}.png`);
  const during = join(OUT_DIR, `critical-only-${vp.name}.png`);
  const after = join(OUT_DIR, `full-css-${vp.name}.png`);

  const page = await browser.newPage({
    viewport: { width: vp.width, height: vp.height },
    deviceScaleFactor: 1,
    // The capture is a dark-mode-only site; pin the preference so a runner
    // default can never move the fold's colours out from under the golden.
    colorScheme: "dark",
    // The captured logo SVG animates a blinking cursor (1.2s, steps(1)) and
    // runs it inside <img>. Left free it lands on or off per screenshot, so
    // every diff here carries a few dozen pixels of coin-flip. The SVG honours
    // prefers-reduced-motion itself — animation: none, opacity fixed — so
    // pinning the preference makes the fold deterministic rather than
    // approximately deterministic. Both screenshots and the golden are taken
    // under it, so nothing about the comparison is one-sided.
    reducedMotion: "reduce",
  });

  try {
    // Count on the measured navigation only. A preload whose `as` does not match
    // its consumer downloads the sheet twice; that is the risk PR-E takes on,
    // and this is where it would show.
    let sheetRequests = 0;
    page.on("request", (r) => {
      if (r.url().includes(SHEET_PATH)) sheetRequests++;
    });
    await page.goto(`${BASE_URL}/`, { waitUntil: "load" });
    await page.waitForTimeout(500);
    await settleFonts(page);

    const deferred = await page.evaluate(
      (sel) => document.querySelectorAll(sel).length,
      DEFERRED_LINK_SELECTOR,
    );
    if (deferred === 0) {
      throw new InfraError(
        `${vp.name}: the served page carried no deferred stylesheet even after ` +
          "the pre-warm succeeded — the variant served here differs.",
      );
    }

    // Reference: the loader has run and the full stylesheet applies.
    if (REGENERATE) {
      mkdirSync(GOLDEN_DIR, { recursive: true });
      await page.screenshot({ path: golden });
      return { name: vp.name, regenerated: golden };
    }
    await page.screenshot({ path: after });

    // Candidate: the same document with the deferred sheet not applying.
    // The return value is the self-check: this reconstruction only means
    // anything if setting the primitive actually DETACHES the sheet. Should a
    // future primitive stop doing that, the mutation becomes a no-op and this
    // lane silently degrades into comparing the golden against an identical
    // render — permanently, quietly green. Count the applied sheets either
    // side of the mutation and refuse to report on a no-op.
    const sheetsBefore = await page.evaluate(() => document.styleSheets.length);
    await page.evaluate(
      ({ sel, attr, value }) => {
        for (const l of document.querySelectorAll(sel)) l.setAttribute(attr, value);
      },
      { sel: DEFERRED_LINK_SELECTOR, attr: PRIMITIVE_ATTR, value: PRIMITIVE_VALUE },
    );
    const sheetsAfter = await page.evaluate(() => document.styleSheets.length);
    if (sheetsAfter >= sheetsBefore) {
      throw new InfraError(
        `${vp.name}: setting ${PRIMITIVE_ATTR}="${PRIMITIVE_VALUE}" did not ` +
          `detach any stylesheet (${sheetsBefore} -> ${sheetsAfter}). The ` +
          'un-applying reconstruction is a no-op, so "during" would be the ' +
          'same render as "after" and this lane would pass vacuously. Update ' +
          'primitive.json / this reconstruction for the current primitive.',
      );
    }
    await page.waitForTimeout(300);
    await settleFonts(page);
    await page.screenshot({ path: during });

    if (!existsSync(golden)) {
      throw new InfraError(
        `${vp.name}: missing golden ${golden} — regenerate inside the pinned ` +
          'image (README.md, "Golden screenshots").',
      );
    }

    const findings = [];
    const full = compare(golden, after);
    const crit = compare(golden, during);

    // Checked first: a stale golden makes the flash number meaningless, and
    // reporting it as a flash sends someone hunting a product bug that is
    // really a fixture refresh.
    if (full.ratio > THRESHOLD) {
      findings.push(
        `**${vp.name}**: the fully-styled render diverges from the golden ` +
          `(${full.ratio.toFixed(5)} > ${THRESHOLD}) — the golden is stale, or ` +
          "the page changed. Fix that before reading the flash result.",
      );
    } else if (crit.ratio > THRESHOLD) {
      findings.push(
        `**${vp.name}**: flash of unstyled content — ${crit.diff}/${crit.total} ` +
          `pixels (${crit.ratio.toFixed(5)}) differ from the fully-styled fold ` +
          `while the deferred stylesheet has not applied (threshold ${THRESHOLD}).`,
      );
    }

    if (sheetRequests !== 1) {
      findings.push(
        `**${vp.name}**: the deferred stylesheet was requested ${sheetRequests} ` +
          "times (expected exactly 1).",
      );
    }

    return { name: vp.name, full, crit, sheetRequests, findings };
  } finally {
    await page.close();
  }
}

function renderFindings(product, infra) {
  if (!product.length && !infra.length) return "";
  let out = "";
  if (infra.length) {
    out +=
      "### Rendered probe did not reach a verdict (infrastructure)\n\n" +
      infra.map((f) => `- ${f}`).join("\n") +
      "\n\n";
  }
  if (product.length) {
    out +=
      "### Async-CSS rendered probe findings\n\n" +
      `Measured against a worker started with \`${WORKER_ARGV}\` — a ` +
      "configuration that never ships. Deferral is forced past the sufficiency " +
      "gate so that there is deferred markup to render at all. The fixture " +
      "origin serves the fold's web fonts and logos, so a flash caused by the " +
      "deferred sheet taking its @font-face rules with it DOES show up in " +
      "these numbers — read them with that in mind rather than as a pure " +
      "layout diff.\n\n" +
      product.map((f) => `- ${f}`).join("\n") +
      "\n";
  }
  return out;
}

async function main() {
  mkdirSync(OUT_DIR, { recursive: true });
  const product = [];
  const infra = [];
  const results = [];

  const browser = await chromium.launch({ args: ["--force-color-profile=srgb"] });
  try {
    await prewarm(browser);
    for (const vp of VIEWPORTS) {
      try {
        results.push(await runViewport(browser, vp));
      } catch (e) {
        if (!(e instanceof InfraError)) throw e;
        infra.push(e.message);
      }
    }
  } catch (e) {
    if (!(e instanceof InfraError)) throw e;
    infra.push(e.message);
  } finally {
    await browser.close();
  }

  if (REGENERATE) {
    for (const r of results) console.log(`regenerated ${r.regenerated}`);
    for (const f of infra) console.error(`INFRA: ${f}`);
    process.exitCode = infra.length ? 1 : 0;
    return;
  }

  for (const r of results) {
    if (!r.crit) continue;
    console.log(
      `${r.name}: critical-only=${r.crit.ratio.toFixed(5)} ` +
        `full-css=${r.full.ratio.toFixed(5)} sheet-requests=${r.sheetRequests} ` +
        `(threshold ${THRESHOLD})`,
    );
    product.push(...r.findings);
  }
  for (const f of infra) console.error(`INFRA: ${f}`);

  writeFileSync(FINDINGS, renderFindings(product, infra));
  if (product.length || infra.length) {
    console.error(
      `\n${product.length} finding(s), ${infra.length} infrastructure ` +
        `failure(s) written to ${FINDINGS}`,
    );
    // Non-zero so a local run is obvious. The nightly workflow captures this
    // rather than failing on it: the tracking issue is the signal.
    process.exitCode = 1;
  }
}

await main();
