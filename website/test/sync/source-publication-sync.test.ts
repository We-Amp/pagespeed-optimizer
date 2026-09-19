// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// source-publication-sync.test.ts — DRIFT GUARD for the software-license facts.
// SOURCE_PUBLICATION in product-facts.mjs is the single source of truth for the
// license the software is distributed under, mirrored by the machine-readable
// /api/product.json `license` + `source_publication` fields and the human
// /license/ page. This fails CI if any of them drifts:
//   1. the canonical record names the Apache License 2.0, says what is true
//      today (`published` since the source repositories went public on
//      2026-09-16) and carries no change-date / conversion fields
//      (those belonged to the retired source-available plan);
//   2. /api/product.json must serialize the canonical values verbatim;
//   3. /license/ must import the single source and must NOT re-hardcode the
//      drift-prone strings (license name / SPDX id) as literals;
//   4. no copy source on the site names the retired license or keeps the
//      source-available framing — the sweep that removed it missed one spelling
//      once, so every spelling is pinned here;
//   5. no non-content copy surface hard-codes a publication claim ("is open
//      source", "open source under ...") — those derive from
//      SOURCE_PUBLICATION.status via LICENSE_CLAUSE / FREE_TO_RUN_LINE, so the
//      claim flips with the fact, never by hand;
//   6. no non-content copy surface talks about license keys — neither the
//      retired key model nor reassurance about its absence (the site does not
//      mention keys at all);
//   7. no copy source describes the retired per-site / flat-rate commercial
//      model (per-site subscriptions, flat-rate pricing, "buy/purchase a
//      license", the retired Business-tier grant) — the earlier guards already
//      covered the key/warn class; this covers the commercial-model class
//      removed alongside it;
//   8. no copy source describes the retired license-activation mechanism
//      (license activation, token-based licensing, auto-renewal, or the
//      Ed25519-token-based licensing variant specifically) — guard 6 already
//      forbids "license activation" and "license token" but only scans the
//      non-content surfaces; this repeats that class over src/content too, so
//      a blog post can't carry it (that is exactly how one slipped through:
//      mod-pagespeed-alternatives.md's "Unified licensing... license
//      activation. Ed25519 token-based licensing with auto-renewal." bullet).
//      Scoped narrowly so it does not trip on RSL-CAP / Web Bot Auth copy,
//      which legitimately uses Ed25519 for request-signature verification
//      (a live, current, unrelated feature) and even says "Ed25519-signed
//      ... licensed identity" in the same sentence — the pattern below never
//      matches bare "Ed25519" or bare "licensed", only the compound retired
//      phrasing.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { describe, it, expect } from 'vitest';

import { GET as productJsonGet } from '../../src/pages/api/product.json.ts';
import { SOURCE_PUBLICATION } from '../../src/data/product-facts.mjs';

const ROOT = resolve(__dirname, '../..');
// 3b is about the page body: a future SPDX header line legitimately carries the
// SPDX id, so it is stripped before the literal check.
const licenseAstro = readFileSync(resolve(ROOT, 'src/pages/license.astro'), 'utf8').replace(
  /^\/\/ SPDX-License-Identifier:.*$/m,
  '',
);

// The legal pages (/terms/, /privacy/) were revised for the converged model and
// are scanned like every other copy surface.
const PENDING_LEGAL_REVISION = new Set<string>();
const PENDING_KEY_TALK = new Set<string>();
// The derivation itself lives here and legitimately spells out both forms.
const DERIVATION_SOURCE = 'src/data/product-facts.mjs';
// Comment-only mentions of the retired per-site/Business ladder as history or
// dead-code documentation — not rendered copy, so guard 7 does not chase them.
const PENDING_COMMERCIAL_MODEL_TALK = new Set([
  'src/data/offer-jsonld.ts', // "(The pre-GA AggregateOffer spanned the paid per-site license rungs; the license ladder is retired.)"
  'src/pages/calculator.astro', // "Per-site licensing: one Business license per site, servers don't count." — dead branch, mpsUnitPrice is always 0
]);

/** Every text source under `dir` (recursive), by extension. */
function copySources(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...copySources(p));
    else if (/\.(astro|md|mdx|ts|mjs|js|json|txt|tmpl)$/.test(name)) out.push(p);
  }
  return out;
}

describe('software-license single-source drift guard', () => {
  it('1. the canonical record is the Apache License 2.0, published, nothing pending', () => {
    expect(SOURCE_PUBLICATION).toEqual({
      status: 'published',
      license: 'Apache License 2.0',
      licenseId: 'Apache-2.0',
    });
  });

  it('2. /api/product.json serializes the canonical values and carries no evaluation story', async () => {
    const res = (productJsonGet as (ctx?: unknown) => Response)({});
    const data = JSON.parse(await res.text());
    expect(data.license).toBe(SOURCE_PUBLICATION.licenseId);
    expect(data.source_publication).toEqual({
      status: SOURCE_PUBLICATION.status,
      license: SOURCE_PUBLICATION.licenseId,
    });
    expect(data).not.toHaveProperty('evaluation');
  });

  it('3a. /license/ imports the single source', () => {
    expect(licenseAstro).toMatch(/from '\.\.\/data\/product-facts\.mjs'/);
    expect(licenseAstro).toContain('SOURCE_PUBLICATION');
  });

  it('3b. /license/ does not re-hardcode the drift-prone forms (must interpolate)', () => {
    expect(licenseAstro).not.toContain(SOURCE_PUBLICATION.license); // 'Apache License 2.0'
    expect(licenseAstro).not.toContain(SOURCE_PUBLICATION.licenseId); // 'Apache-2.0'
  });

  it('4. no copy source names the retired license or the source-available framing', () => {
    // The copy surfaces: pages, content collections, data files, components,
    // layouts, and the llms / ai-plugin templates that render into public/
    // (the rendered files themselves are pinned byte-for-byte by
    // llms-generated.test.ts).
    const RETIRED = /Business Source License|BUSL|\bBSL\b|[Ss]ource[- ][Aa]vailable/;
    const dirs = [
      'src/pages',
      'src/content',
      'src/data',
      'src/components',
      'src/layouts',
      'scripts/llms-templates',
    ];
    const offenders: string[] = [];
    for (const dir of dirs) {
      for (const file of copySources(resolve(ROOT, dir))) {
        const rel = file.slice(ROOT.length + 1);
        if (PENDING_LEGAL_REVISION.has(rel)) continue;
        if (RETIRED.test(readFileSync(file, 'utf8'))) offenders.push(rel);
      }
    }
    expect(offenders).toEqual([]);
  });

  // Copy surfaces other than the Markdown content collections. Content pages
  // that assert publication (the GA announcement post) are hard-coded by
  // nature and are listed in the PR body for the owner's gate instead.
  const NON_CONTENT_DIRS = [
    'src/pages',
    'src/data',
    'src/components',
    'src/layouts',
    'scripts/llms-templates',
  ];
  function offendersIn(pattern: RegExp, skip: Set<string>): string[] {
    const out: string[] = [];
    for (const dir of NON_CONTENT_DIRS) {
      for (const file of copySources(resolve(ROOT, dir))) {
        const rel = file.slice(ROOT.length + 1);
        if (skip.has(rel) || rel === DERIVATION_SOURCE) continue;
        if (pattern.test(readFileSync(file, 'utf8'))) out.push(rel);
      }
    }
    return out;
  }

  it('5. no non-content copy surface hard-codes a publication claim', () => {
    // Historical references to the original project ("the open-source
    // mod_pagespeed project", "Migrate from open source") do not match: the
    // pattern needs the claim form — a copula or "under", or the bare
    // "Open source." sentence — applied to the product itself.
    const CLAIM =
      /\b(is|are|now|be|being) open[- ]source\b|open[- ]source under\b|\bOpen source\.|free and open source|same open-source|the source is open|source is public/i;
    expect(offendersIn(CLAIM, new Set())).toEqual([]);
  });

  it('6. no non-content copy surface talks about license keys', () => {
    const KEY_TALK =
      /licen[cs]e[- ]key|license keys|--license-key|PAGESPEED_LICENSE|LicenseKey|licenseKey|unlicensed|X-PageSpeed-Warn|locks you out|license token|licen[cs]e activation|activat(e|ion|ing) a license|no license key|needs? no key|no keys?\b|key is ignored|leftover key/i;
    expect(offendersIn(KEY_TALK, PENDING_KEY_TALK)).toEqual([]);
  });

  it('7. no copy source describes the retired per-site / flat-rate commercial model', () => {
    // Same surfaces as guard 4 (content collections included, so a fixed blog
    // post cannot regress): pages, content, data, components, layouts, and
    // the llms templates. Competitor pricing facts ("Per-site SaaS
    // subscription (see vendor for tiers)") do not match: the pattern
    // requires the retired-model word directly after "per-site" / "flat", or
    // the "Business ... covers" grant phrasing, which only ever described
    // this product's retired ladder.
    const RETIRED_COMMERCIAL_MODEL =
      /\bper[- ]site (?:subscription|pricing|price|rate|licens\w*)\b|\bflat[, ]*(?:predictable )?per[- ]site (?:price|pricing|rate|cost|model)\b|\bflat rate per site\b|\bbills? per site\b|\bpriced? per[- ]site\b|\bbuy a licen[cs]e\b|\bpurchase a licen[cs]e\b|\bBusiness (?:licen[cs]e )?covers\b/i;
    const dirs = [
      'src/pages',
      'src/content',
      'src/data',
      'src/components',
      'src/layouts',
      'scripts/llms-templates',
    ];
    const offenders: string[] = [];
    for (const dir of dirs) {
      for (const file of copySources(resolve(ROOT, dir))) {
        const rel = file.slice(ROOT.length + 1);
        if (PENDING_LEGAL_REVISION.has(rel) || PENDING_COMMERCIAL_MODEL_TALK.has(rel)) continue;
        if (RETIRED_COMMERCIAL_MODEL.test(readFileSync(file, 'utf8'))) offenders.push(rel);
      }
    }
    expect(offenders).toEqual([]);
  });

  it('8. no copy source describes the retired license-activation mechanism', () => {
    // Same surfaces as guards 4 and 7 (content collections included). The
    // "Ed25519" alternative requires it to sit directly against "token-based"
    // — RSL-CAP's docs use Ed25519 for signature verification and even pair
    // it with "licensed identity" in prose (rsl-cap.md: "Ed25519-signed
    // statement of a licensed identity"), which must NOT trip this guard;
    // "Ed25519[- ]+token-based" cannot match that sentence shape.
    const LICENSE_MECHANISM_TALK =
      /licen[cs]e activation|token-based licens\w*|auto-renew(?:al)?|Ed25519[\s-]+token-based/i;
    const dirs = [
      'src/pages',
      'src/content',
      'src/data',
      'src/components',
      'src/layouts',
      'scripts/llms-templates',
    ];
    const offenders: string[] = [];
    for (const dir of dirs) {
      for (const file of copySources(resolve(ROOT, dir))) {
        const rel = file.slice(ROOT.length + 1);
        if (PENDING_LEGAL_REVISION.has(rel)) continue;
        if (LICENSE_MECHANISM_TALK.test(readFileSync(file, 'utf8'))) offenders.push(rel);
      }
    }
    expect(offenders).toEqual([]);
  });
});
