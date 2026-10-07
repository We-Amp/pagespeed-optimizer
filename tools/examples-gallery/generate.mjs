#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Build-time generator for the /examples/ optimization gallery.
//
// For every entry in website/src/data/examples.ts it fetches the demo page from
// the live Apache + mod_pagespeed 1.1 backend twice — once with optimization off
// (the "before") and once with only that filter enabled (the "after") — captures
// a source diff, and measures the byte/request impact. The result is written to
// website/src/data/examples-data.json, which the Astro pages read at build time.
//
// Resource-rewriting filters are ASYNC on a cold cache: the first request for a
// novel filter set returns pass-through HTML and the rewritten variant only
// appears once the worker has processed it. We therefore poll the "after" URL
// until the body both differs from the "before" body and is stable across two
// consecutive fetches. This run also serves as a cache warm-up for those URLs.
//
// Usage (Node >= 22, run after the demo backend is deployed):
//   node tools/examples-gallery/generate.mjs
//   node tools/examples-gallery/generate.mjs --only combine_css,rewrite_images
//   DEMO_ORIGIN=https://demo-httpd-1.1.modpagespeed.com node tools/examples-gallery/generate.mjs

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { writeFileSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const WEBSITE = resolve(__dirname, "../../website");

const { EXAMPLES, DEMO_ORIGIN, beforeUrl, afterUrl } = await import(
  resolve(WEBSITE, "src/data/examples.ts")
);

const OUT = resolve(WEBSITE, "src/data/examples-data.json");

// ---- tunables --------------------------------------------------------------
const POLL_ATTEMPTS = 20; // max polls waiting for async rewrite to settle
const POLL_INTERVAL = 1500; // ms between polls
const MAX_DIFF_LINES = 120; // cap source-diff length stored per example
const MAX_BODY_CHARS = 8000; // cap raw HTML stored per side
const MAX_RESOURCE_BYTES = 8 * 1024 * 1024; // skip absurdly large subresources

const ORIGIN = process.env.DEMO_ORIGIN || DEMO_ORIGIN;
const onlyArg = process.argv.indexOf("--only");
const ONLY =
  onlyArg !== -1 ? new Set(process.argv[onlyArg + 1].split(",")) : null;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function fetchText(url) {
  const res = await fetch(url, {
    headers: { "Accept-Encoding": "identity", "Accept": "image/avif,image/webp,*/*" },
    redirect: "follow",
  });
  const body = await res.text();
  return { status: res.status, headers: res.headers, body };
}

async function fetchBytes(url) {
  try {
    const res = await fetch(url, {
      headers: { "Accept-Encoding": "identity", "Accept": "image/avif,image/webp,*/*" },
    });
    if (!res.ok) return null;
    const len = res.headers.get("content-length");
    if (len && Number(len) > MAX_RESOURCE_BYTES) return Number(len);
    const buf = await res.arrayBuffer();
    return buf.byteLength;
  } catch {
    return null;
  }
}

// Extract subresource URLs (stylesheets, scripts, images) as absolute URLs.
function extractResources(html, pageUrl) {
  const urls = new Set();
  const add = (raw) => {
    if (!raw || raw.startsWith("data:") || raw.startsWith("javascript:"))
      return;
    try {
      urls.add(new URL(raw, pageUrl).href);
    } catch {
      /* ignore unparseable */
    }
  };
  const re =
    /<(?:link[^>]+href|script[^>]+src|img[^>]+src)\s*=\s*["']([^"']+)["']/gi;
  let m;
  while ((m = re.exec(html)) !== null) add(m[1]);
  return [...urls];
}

async function measure(html, pageUrl) {
  const resources = extractResources(html, pageUrl);
  let resourceBytes = 0;
  let counted = 0;
  for (const u of resources) {
    const b = await fetchBytes(u);
    if (b != null) {
      resourceBytes += b;
      counted++;
    }
  }
  return {
    htmlBytes: Buffer.byteLength(html, "utf8"),
    requestCount: 1 + resources.length, // the document + its subresources
    resourceCount: resources.length,
    resourceBytes,
    resourcesMeasured: counted,
  };
}

// Minimal LCS line diff. Returns [{ t: ' '|'-'|'+', s }]. Inputs capped upstream.
function lineDiff(a, b) {
  const A = a.split("\n");
  const B = b.split("\n");
  const n = A.length;
  const mm = B.length;
  const dp = Array.from({ length: n + 1 }, () => new Int32Array(mm + 1));
  for (let i = n - 1; i >= 0; i--) {
    for (let j = mm - 1; j >= 0; j--) {
      dp[i][j] =
        A[i] === B[j]
          ? dp[i + 1][j + 1] + 1
          : Math.max(dp[i + 1][j], dp[i][j + 1]);
    }
  }
  const out = [];
  let i = 0;
  let j = 0;
  while (i < n && j < mm) {
    if (A[i] === B[j]) {
      out.push({ t: " ", s: A[i] });
      i++;
      j++;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      out.push({ t: "-", s: A[i] });
      i++;
    } else {
      out.push({ t: "+", s: B[j] });
      j++;
    }
  }
  while (i < n) out.push({ t: "-", s: A[i++] });
  while (j < mm) out.push({ t: "+", s: B[j++] });
  return out;
}

// Collapse long unchanged runs so the stored diff stays focused on the change.
function condenseDiff(diff) {
  const CTX = 3;
  const keep = new Array(diff.length).fill(false);
  for (let i = 0; i < diff.length; i++) {
    if (diff[i].t !== " ") {
      for (
        let k = Math.max(0, i - CTX);
        k <= Math.min(diff.length - 1, i + CTX);
        k++
      )
        keep[k] = true;
    }
  }
  const out = [];
  let skipped = 0;
  for (let i = 0; i < diff.length; i++) {
    if (keep[i]) {
      if (skipped > 0) {
        out.push({
          t: "@",
          s: `… ${skipped} unchanged line${skipped === 1 ? "" : "s"} …`,
        });
        skipped = 0;
      }
      out.push(diff[i]);
    } else {
      skipped++;
    }
  }
  return out.slice(0, MAX_DIFF_LINES);
}

// Canonicalize attribute reflow — FOR DIFF PURPOSES ONLY.
//
// mod_pagespeed re-serializes a tag whose attributes were authored across
// several lines onto a single line. That reformatting is purely cosmetic, but a
// line-based diff reports every wrapped line as a -/+ pair, drowning the real
// change (a rewritten `src`, a dropped attribute) in reflow noise. Before
// diffing we collapse each tag's interior whitespace runs — spaces AND newlines
// — to a single space on BOTH sides, so a tag that was merely reflowed onto one
// line compares equal to its multi-line original. We also fold the rewriter's
// cosmetic self-close spacing (`attr/>` vs `attr />`) and a stray space before a
// tag's closing `>` to one canonical form.
//
// This runs ONLY for the stored `diff` (and therefore the material-change
// signature, which is derived from `diff`). Byte/request MEASUREMENTS and the
// stored `before.html` / `after.html` are computed from the RAW body and are
// untouched — the detail page still shows verbatim source.
//
// This is a display heuristic, not an HTML parser: we track a coarse in-tag
// state and skip over quoted attribute values so a `>` inside a value does not
// prematurely close the tag. Text between tags is left verbatim, preserving the
// line structure where real content changes live. Limitation: unbalanced quotes
// or a `<`/`>` inside an UNQUOTED attribute value fall back to naive behaviour —
// acceptable for a cosmetic diff normalizer.
function canonicalizeForDiff(html) {
  let out = "";
  let i = 0;
  const n = html.length;
  while (i < n) {
    if (html[i] !== "<") {
      out += html[i++];
      continue;
    }
    // Consume a whole tag, collapsing interior whitespace runs to one space.
    let tag = "<";
    i++;
    let quote = null;
    while (i < n) {
      const d = html[i];
      if (quote) {
        tag += d;
        if (d === quote) quote = null;
        i++;
      } else if (d === '"' || d === "'") {
        quote = d;
        tag += d;
        i++;
      } else if (d === ">") {
        tag += ">";
        i++;
        break;
      } else if (/\s/.test(d)) {
        while (i < n && /\s/.test(html[i])) i++; // skip the whole run
        tag += " ";
      } else {
        tag += d;
        i++;
      }
    }
    // Fold cosmetic closing spacing: `attr />` / `attr/>` → `/>`, `attr >` → `>`.
    tag = tag.replace(/\s*\/>$/, "/>").replace(/\s+>$/, ">");
    out += tag;
  }
  return out;
}

async function pollAfter(beforeBody, url) {
  let last = null;
  let lastDiffered = false;
  for (let attempt = 1; attempt <= POLL_ATTEMPTS; attempt++) {
    const { status, headers, body } = await fetchText(url);
    const psHeader =
      headers.get("x-mod-pagespeed") || headers.get("x-page-speed") || "";
    const differs = body !== beforeBody;
    const stable = last !== null && body === last;
    // Settle once the optimized body has differed from the original for two
    // consecutive polls. `stable` (byte-identical) catches resource-rewriting
    // filters once the async cache warms; `lastDiffered` catches HTML-injection
    // filters (lazyload, defer_javascript, instrumentation, GA, dns_prefetch)
    // whose injected markup carries a per-request nonce/beacon, so it never
    // byte-stabilizes but is unambiguously applied after the first differing poll.
    if (differs && (stable || lastDiffered)) {
      return { status, body, psHeader, attempts: attempt, settled: true };
    }
    last = body;
    lastDiffered = differs;
    if (attempt < POLL_ATTEMPTS) await sleep(POLL_INTERVAL);
  }
  // Did not settle into a stable, different state; return the last we saw.
  return {
    status: 200,
    body: last ?? beforeBody,
    psHeader: "",
    attempts: POLL_ATTEMPTS,
    settled: false,
  };
}

function trim(s) {
  return s.length > MAX_BODY_CHARS ? s.slice(0, MAX_BODY_CHARS) + "\n…" : s;
}

function pct(before, after) {
  if (!before) return 0;
  return Math.round(((before - after) / before) * 100);
}

async function run() {
  const targets = EXAMPLES.filter((e) => !ONLY || ONLY.has(e.slug));
  console.log(
    `Generating gallery data for ${targets.length} example(s) against ${ORIGIN}\n`,
  );

  const out = {
    generated_at: new Date().toISOString(),
    origin: ORIGIN,
    examples: {},
  };

  for (const ex of targets) {
    const bUrl = beforeUrl(ex).replace(DEMO_ORIGIN, ORIGIN);
    const aUrl = afterUrl(ex).replace(DEMO_ORIGIN, ORIGIN);
    process.stdout.write(`• ${ex.slug} … `);
    try {
      const before = await fetchText(bUrl);
      if (before.status !== 200) {
        console.log(`SKIP (before HTTP ${before.status})`);
        out.examples[ex.slug] = { error: `before HTTP ${before.status}` };
        continue;
      }
      const after = await pollAfter(before.body, aUrl);

      const beforeMetrics = await measure(before.body, bUrl);
      const afterMetrics = await measure(after.body, aUrl);

      // Diff over reflow-canonicalized bodies so cosmetic attribute reflow does
      // not surface as -/+ pairs. Measurements + stored html stay raw (above).
      const diff = condenseDiff(
        lineDiff(
          canonicalizeForDiff(trim(before.body)),
          canonicalizeForDiff(trim(after.body)),
        ),
      );

      out.examples[ex.slug] = {
        settled: after.settled,
        attempts: after.attempts,
        psHeader: after.psHeader,
        before: { html: trim(before.body), ...beforeMetrics },
        after: { html: trim(after.body), ...afterMetrics },
        savings: {
          htmlBytesPct: pct(beforeMetrics.htmlBytes, afterMetrics.htmlBytes),
          requests: beforeMetrics.requestCount - afterMetrics.requestCount,
          resourceBytesPct: pct(
            beforeMetrics.resourceBytes,
            afterMetrics.resourceBytes,
          ),
          totalBytesPct: pct(
            beforeMetrics.htmlBytes + beforeMetrics.resourceBytes,
            afterMetrics.htmlBytes + afterMetrics.resourceBytes,
          ),
        },
        diff,
      };
      const s = out.examples[ex.slug].savings;
      console.log(
        `${after.settled ? "ok" : "UNSETTLED"} (${after.attempts}x)  ` +
          `reqs ${s.requests >= 0 ? "-" : "+"}${Math.abs(s.requests)}  bytes -${s.totalBytesPct}%`,
      );
    } catch (err) {
      console.log(`ERROR ${err.message}`);
      out.examples[ex.slug] = { error: err.message };
    }
  }

  // Read the prior file (for the --only merge below, and the no-churn check).
  let prev = null;
  try {
    prev = JSON.parse(
      await (await import("node:fs/promises")).readFile(OUT, "utf8"),
    );
  } catch {
    /* no prior file */
  }

  // Merge with existing file when running with --only so partial runs don't wipe data.
  if (ONLY && prev) {
    out.examples = { ...prev.examples, ...out.examples };
  }

  // Measuring a live, async, cache-stateful backend is jittery: poll `attempts`
  // and the summed subresource byte counts vary run-to-run even when nothing
  // meaningful changed. The stable, displayed signals are the source `diff`,
  // `settled`, and the `requests` delta (all deterministic from the HTML). If
  // none of those changed, leave the file byte-identical so the scheduled refresh
  // (refresh-examples-gallery.yml) only opens a PR on a real change — not on
  // measurement noise. (cf. the dep-scan daily cron, fixed to stop timestamp churn.)
  const materialSig = (examples) =>
    JSON.stringify(
      Object.fromEntries(
        Object.entries(examples)
          .sort(([a], [b]) => a.localeCompare(b))
          .map(([k, v]) => [
            k,
            v.error
              ? { error: v.error }
              : {
                  settled: v.settled,
                  requests: v.savings?.requests,
                  diff: v.diff,
                },
          ]),
      ),
    );
  if (
    prev &&
    prev.origin === out.origin &&
    materialSig(prev.examples) === materialSig(out.examples)
  ) {
    console.log(
      "No material change (diff / settled / requests) — leaving examples-data.json as-is.",
    );
    return;
  }

  writeFileSync(OUT, JSON.stringify(out, null, 2) + "\n");
  const ok = Object.values(out.examples).filter(
    (e) => !e.error && e.settled,
  ).length;
  const unsettled = Object.values(out.examples).filter(
    (e) => !e.error && !e.settled,
  ).length;
  const errs = Object.values(out.examples).filter((e) => e.error).length;
  console.log(`\nWrote ${OUT}`);
  console.log(`  settled: ${ok}  unsettled: ${unsettled}  errors: ${errs}`);
  if (unsettled || errs) {
    console.log(
      "  (unsettled/errored entries still render from whatever was captured)",
    );
  }
}

await run();
