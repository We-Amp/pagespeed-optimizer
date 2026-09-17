// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// llms-generated.test.ts — DRIFT GUARD for the single-source product facts.
//
// product-facts.mjs is the single source of truth for drift-prone facts
// (license, support tiers, variant count, versions, bundled nginx, package
// names, company info). They flow into surfaces that historically drifted:
//   - public/llms.txt, public/llms-full.txt  (GENERATED from templates + facts)
//   - public/.well-known/ai-plugin.json       (GENERATED from template + facts;
//       added 2026-06 after it drifted to "14-day free trial" / "1.1" because it
//       was hand-maintained outside this single source)
//   - src/pages/api/product.json.ts           (imports the facts)
//   - src/data/offer-jsonld.ts                (imports the facts)
//
// This test fails CI whenever any of those falls out of sync with the facts:
//   1. BYTE-EQUIVALENCE — the committed public/llms*.txt must equal a fresh
//      render of the templates with the current facts. Catches a fact changed
//      without `npm run gen:llms`, a hand-edited generated file, or a template
//      whose token was literalized.
//   2. NON-TOKENIZED FACTS — package names / RIDs / vendor are literals in the
//      prose templates (not {{tokens}}); assert each fact string still appears
//      in the generated text, so changing it in product-facts.mjs forces a
//      template update.
//   3. NO STALE COMMERCIAL COPY — trial, key and per-server-pricing language
//      must not regress in.
//   4. product.json / offer-jsonld reflect the facts (compile-time guaranteed
//      by the import, asserted here as belt-and-suspenders).

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';

import { generateContent, PUBLIC_DIR, TARGETS } from '../../scripts/generate-llms.mjs';
import { GET as productJsonGet } from '../../src/pages/api/product.json.ts';
import {
  SUPPORT_TIERS,
  SUPPORT_LADDER_MD,
  SUPPORT_LADDER_LINE,
  SUPPORT_URL,
  SOURCE_PUBLICATION,
  LICENSE_CLAUSE,
  LICENSING_TERMS_URL,
  PRICE_VALID_UNTIL,
  MAX_VARIANTS,
  RASTER_VARIANTS,
  PRODUCT_NAME,
  CURRENT_LINE,
  V1_LINE,
  V1_RENUMBERED_FROM,
  COMPANY_FOUNDED_YEAR,
  LAST_UPSTREAM_VERSION,
  SIDECAR_NGINX_VERSION,
  BUILTWITH_SITES,
  BUILTWITH_AS_OF,
  STOCK_NGINX_UBUNTU,
  STOCK_NGINX_ALMA,
  COMPANY_KVK,
  VENDOR,
  COMPANY_COUNTRY,
  PKG_ASPNETCORE,
  PKG_SIDECAR,
  PKG_SIDECAR_NATIVE,
  SIDECAR_RIDS,
  ASPNETCORE_RIDS,
} from '../../src/data/product-facts.mjs';
import { offerJsonLd } from '../../src/data/offer-jsonld.ts';

const generated = generateContent();
const combined = TARGETS.map((t) => generated[t.output]).join('\n');

describe('llms*.txt drift guard', () => {
  describe('1. byte-equivalence: committed file === fresh render of templates+facts', () => {
    for (const { output } of TARGETS) {
      it(`public/${output} is byte-identical to generated output`, () => {
        const committed = readFileSync(resolve(PUBLIC_DIR, output), 'utf8');
        // If this fails, run `npm run gen:llms` (a fact changed, a template was
        // edited, or the published file was hand-edited).
        expect(committed).toBe(generated[output]);
      });
    }
  });

  describe('2. tokenized facts substituted into the prose', () => {
    it('llms.txt carries the license + support + version facts', () => {
      const t = generated['llms.txt'];
      // The support ladder must appear byte-equivalently as derived from
      // SUPPORT_TIERS — a literalized {{SUPPORT_LADDER_MD}} token or a
      // hand-edited tier row fails here.
      expect(t).toContain(SUPPORT_LADDER_MD);
      expect(t).toContain(LICENSE_CLAUSE);
      expect(t).toContain(SOURCE_PUBLICATION.license);
      expect(t).toContain(LICENSING_TERMS_URL);
      // The retired launch promo must never resurface.
      expect(t).not.toContain('% off year one');
      expect(t).not.toContain('half price');
      expect(t).toContain(`up to ${MAX_VARIANTS} variants per image`);
      expect(t).toContain(`${RASTER_VARIANTS} raster`);
      expect(t).toContain(`${PRODUCT_NAME} ${CURRENT_LINE}`);
      expect(t).toContain(`mod_pagespeed ${V1_LINE}`);
      expect(t).toContain(`renumbered from ${V1_RENUMBERED_FROM}`);
      expect(t).toContain(BUILTWITH_SITES);
      expect(t).toContain(BUILTWITH_AS_OF);
      expect(t).toContain(COMPANY_KVK);
      expect(t).toContain(`founded ${COMPANY_FOUNDED_YEAR}`);
    });
    it('llms-full.txt carries the support ladder (block + one-line) + version + nginx facts', () => {
      const t = generated['llms-full.txt'];
      expect(t).toContain(SUPPORT_LADDER_MD);
      expect(t).toContain(LICENSING_TERMS_URL);
      expect(t).toContain(LAST_UPSTREAM_VERSION);
      expect(t).toContain(`bundles nginx ${SIDECAR_NGINX_VERSION}`);
      expect(t).toContain(STOCK_NGINX_UBUNTU);
      expect(t).toContain(STOCK_NGINX_ALMA);
      expect(t).toContain(`${PRODUCT_NAME} ${CURRENT_LINE}`);
      expect(t).toContain(`mod_pagespeed ${V1_LINE}`);
    });
    it('no unresolved {{TOKEN}} survives into the generated output', () => {
      expect(combined).not.toMatch(/\{\{\s*[A-Za-z0-9_]+\s*\}\}/);
    });
  });

  describe('3. non-tokenized single-sourced facts still appear (forces template update on change)', () => {
    const literals = [VENDOR, COMPANY_COUNTRY, PKG_ASPNETCORE, PKG_SIDECAR, PKG_SIDECAR_NATIVE];
    for (const lit of literals) {
      it(`"${lit}" appears in the generated llms text`, () => {
        expect(combined).toContain(lit);
      });
    }
    for (const rid of [...new Set([...SIDECAR_RIDS, ...ASPNETCORE_RIDS])]) {
      it(`RID "${rid}" appears in the generated llms text`, () => {
        expect(combined).toContain(rid);
      });
    }
  });

  describe('4. no stale commercial copy', () => {
    it('contains no "14-day" / "14 day" trial language', () => {
      expect(combined).not.toMatch(/14[\s-]?day/i);
    });
    it('contains no "stops optimizing" / "stop optimizing" language', () => {
      expect(combined).not.toMatch(/stops? optimizing/i);
    });
    it('contains no trial framing at all', () => {
      expect(combined).not.toMatch(/free trial/i);
    });
    it('never mentions keys, a warn header, or a commercial license for production', () => {
      expect(combined).not.toMatch(/requires a commercial license/i);
      expect(combined).not.toMatch(/license key/i);
      expect(combined).not.toMatch(/X-PageSpeed-Warn/i);
    });
  });

  describe('5. no retired pricing (per-server, and the retired license ladder)', () => {
    it('never prices per server', () => {
      expect(combined).not.toMatch(/server\/month/i);
      expect(combined).not.toMatch(/per server/i);
      expect(combined).not.toMatch(/one production server/i);
    });
    it('carries no pre-ladder price points ($49 / $39 / $468)', () => {
      expect(combined).not.toMatch(/\$49(?![\d,.])/);
      expect(combined).not.toMatch(/\$39(?![\d,.])/);
      expect(combined).not.toMatch(/\$468(?![\d,.])/);
    });
    it('carries no license-ladder price points ($99 / $948 / $5,000 as prices)', () => {
      expect(combined).not.toMatch(/\$99(?![\d,.])/);
      expect(combined).not.toMatch(/\$948(?![\d,.])/);
      expect(combined).not.toMatch(/\$5,000(?![\d,.])/);
    });
  });
});

describe('ai-plugin.json manifest drift guard', () => {
  // Byte-equivalence + no-unresolved-token + stale-copy sweeps are already
  // covered automatically (the manifest is a TARGET, so it's in the section-1
  // byte loop and in `combined`). These add legible, manifest-specific
  // assertions on the model-facing description.
  const manifest = JSON.parse(generated['.well-known/ai-plugin.json']);

  it('is a valid v1 plugin manifest', () => {
    expect(manifest.schema_version).toBe('v1');
    expect(manifest.name_for_model).toBe('modpagespeed');
  });

  it('description_for_model carries the single-sourced identity + support facts', () => {
    const m: string = manifest.description_for_model;
    expect(m).toContain(`${PRODUCT_NAME} ${CURRENT_LINE}`);
    expect(m).toContain(`mod_pagespeed ${V1_LINE}`);
    expect(m).toContain(`renumbered from ${V1_RENUMBERED_FROM}`);
    expect(m).toContain(LICENSE_CLAUSE);
    // One-line support ladder, byte-equivalent to the derivation from SUPPORT_TIERS.
    expect(m).toContain(SUPPORT_LADDER_LINE);
    expect(m).toContain(LICENSING_TERMS_URL);
    expect(m).toContain(`up to ${MAX_VARIANTS} variants per image`);
  });

  it('does not regress to the stale "14-day free trial" / bare-"1.1" product label', () => {
    const m: string = manifest.description_for_model;
    expect(m).not.toMatch(/14[\s-]?day/i);
    expect(m).not.toMatch(/free trial/i);
    expect(m).not.toContain(`mod_pagespeed ${V1_RENUMBERED_FROM} `);
  });
});

describe('product-facts consumers reflect the single source', () => {
  it('support tiers are placeholder-priced and price-free in their ladder prose', () => {
    for (const t of SUPPORT_TIERS) {
      expect(t.prices.annualUsd).toBeNull();
      expect(t.prices.monthlyUsd).toBeNull();
    }
    expect(SUPPORT_LADDER_MD).not.toMatch(/\$\d/);
    expect(SUPPORT_LADDER_LINE).not.toMatch(/\$\d/);
  });

  it('offer-jsonld emits the $0 Offer', () => {
    const offer = offerJsonLd();
    expect(offer['@type']).toBe('Offer');
    expect(offer.price).toBe(0);
    expect(offer.priceCurrency).toBe('USD');
    expect(offer.priceValidUntil).toBe(PRICE_VALID_UNTIL);
    expect(offer.description).toContain(SOURCE_PUBLICATION.license);
    expect(offer.description).toContain(SUPPORT_URL);
    expect(offer.description).not.toMatch(/commercial license/i);
    expect(offer.description).not.toMatch(/server\/month/i);
  });

  it('/api/product.json reflects the converged identity, support tiers and predecessors', async () => {
    const res = (productJsonGet as (ctx?: unknown) => Response)({});
    const data = JSON.parse(await res.text());
    expect(data.product).toBe(`${PRODUCT_NAME} ${CURRENT_LINE}`);
    expect(data.license).toBe(SOURCE_PUBLICATION.licenseId);
    // The license/publication record is pinned in full by
    // source-publication-sync.test.ts; no evaluation story exists.
    expect(data).not.toHaveProperty('evaluation');
    // Support block mirrors SUPPORT_TIERS (placeholder prices stay null).
    expect(data.support.terms).toBe(LICENSING_TERMS_URL);
    expect(data.support.tiers).toHaveLength(SUPPORT_TIERS.length);
    expect(data.support.tiers.map((t: { id: string }) => t.id)).toEqual(
      SUPPORT_TIERS.map((t) => t.id),
    );
    for (const t of data.support.tiers) {
      expect(t.annual_usd).toBeNull();
      expect(t.monthly_usd).toBeNull();
    }
    // No retired commercial blocks.
    expect(data.pricing).toBeUndefined();
    expect(data.promotion).toBeUndefined();
    expect(data.features.max_variants_per_image).toBe(MAX_VARIANTS);
    // image_formats_by_line keys only grow: predecessors stay, converged joins.
    expect(Object.keys(data.features.image_formats_by_line)).toEqual(
      expect.arrayContaining([V1_LINE, '2.0', CURRENT_LINE]),
    );
    // related_products keeps its key name (public contract); entries are the
    // predecessor lines with an explicit status.
    expect(data.related_products[0].product).toBe(`mod_pagespeed ${V1_LINE}`);
    expect(data.related_products[0].status).toBe('security-fixes-only');
    expect(data.related_products[0].nuget_sidecar.bundled_nginx).toBe(SIDECAR_NGINX_VERSION);
    expect(data.related_products[0].nuget_sidecar.package).toBe(PKG_SIDECAR);
    expect(data.related_products[1].status).toBe('frozen');
  });
});
