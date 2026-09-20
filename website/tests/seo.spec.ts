// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

const allPages = [
  '/',
  '/features/',
  '/pricing/',
  '/docs/',
  '/blog/',
  '/demo/',
  '/calculator/',
  '/contact/',
  '/security/',
  '/license/',
  '/terms/',
  '/pagespeed-markers/',
  // /privacy/ redirects to https://www.we-amp.com/privacy/ (301)
];

test.describe('SEO', () => {
  for (const path of allPages) {
    test(`${path} has a <title> tag`, async ({ page }) => {
      await page.goto(path);
      const title = await page.title();
      expect(title.length).toBeGreaterThan(0);
    });
  }

  for (const path of allPages) {
    test(`${path} has a meta description`, async ({ page }) => {
      await page.goto(path);
      const desc = page.locator('meta[name="description"]');
      const content = await desc.getAttribute('content');
      expect(content).toBeTruthy();
      expect(content!.length).toBeGreaterThan(10);
    });
  }

  test('every page has og:title and og:description', async ({ page }) => {
    for (const path of allPages) {
      await page.goto(path);
      const ogTitle = page.locator('meta[property="og:title"]');
      await expect(ogTitle).toHaveAttribute('content', /.+/);
      const ogDesc = page.locator('meta[property="og:description"]');
      await expect(ogDesc).toHaveAttribute('content', /.+/);
    }
  });

  test('every page has canonical URL', async ({ page }) => {
    for (const path of allPages) {
      await page.goto(path);
      const canonical = page.locator('link[rel="canonical"]');
      const href = await canonical.getAttribute('href');
      expect(href).toContain('modpagespeed.com');
    }
  });

  test('landing page has JSON-LD structured data', async ({ page }) => {
    await page.goto('/');
    // Home emits multiple top-level nodes (Organization + WebSite +
    // SoftwareApplication); assert the first parses as schema.org JSON-LD.
    const jsonLd = page.locator('script[type="application/ld+json"]').first();
    await expect(jsonLd).toBeAttached();
    const content = await jsonLd.textContent();
    const data = JSON.parse(content!);
    expect(data['@context']).toBe('https://schema.org');
  });

  test('pricing page has JSON-LD structured data', async ({ page }) => {
    await page.goto('/pricing/');
    const jsonLd = page.locator('script[type="application/ld+json"]').first();
    await expect(jsonLd).toBeAttached();
  });

  // Product / Merchant Listing JSON-LD. /1.1/ retired (its product page is
  // superseded by /); /download/ is now the sole surface for this check —
  // the Offer lives there.
  const productPages = [
    {
      path: '/download/',
      expectType: (t: unknown) =>
        Array.isArray(t) ? (t as string[]).includes('Product') : t === 'Product',
    },
  ];

  test('Product JSON-LD pages have Merchant Listing fields', async ({ page }) => {
    for (const { path, expectType } of productPages) {
      await test.step(path, async () => {
        await page.goto(path);
        const jsonLd = page.locator('script[type="application/ld+json"]').first();
        await expect(jsonLd).toBeAttached();
        const content = await jsonLd.textContent();
        const data = JSON.parse(content!);
        expect(expectType(data['@type'])).toBe(true);
        expect(typeof data.image).toBe('string');
        expect(data.image).toMatch(/^https?:\/\//);
        expect(data.brand?.name).toBeTruthy();
        // The converged product is open source, so the node is a single $0
        // Offer (accurate and rich-result-valid; a Product node must carry
        // offers). The MerchantReturnPolicy is intentionally omitted from the
        // structured data (see src/data/offer-jsonld.ts) because /terms/ grants
        // no unconditional money-back guarantee.
        expect(data.offers['@type']).toBe('Offer');
        expect(data.offers.price).toBe(0);
        expect(data.offers.priceCurrency).toBeTruthy();
        expect(data.offers.availability).toMatch(/InStock/);
        expect(typeof data.offers.url).toBe('string');
        expect(data.offers.url).toMatch(/^https?:\/\//);
      });
    }
  });

  // Post-convergence: one site name everywhere — the /1.1/*-specific
  // og:site_name flip is retired. The /1.1/ product page itself is retired
  // (superseded by /); /1.1/docs/ (once narrowed to the archived
  // release-notes page) is now retired too, its content folded into
  // /docs/release-notes/. There is no page left to compare og:site_name
  // against, so this asserts the retirement itself rather than dropping
  // the test.
  test('retired /1.1/docs/ page no longer carries its own og:site_name', async ({ page }) => {
    const response = await page.goto('/1.1/docs/release-notes/');
    expect(response?.status()).toBe(404);
  });

  // Post-convergence: one breadcrumb model — the segmentNames override that
  // rendered the '1.1' path segment as "mod_pagespeed 1.15" is retired along
  // with the page it rendered on. Same reasoning as the og:site_name check
  // above: assert the route is gone rather than test breadcrumb JSON-LD on
  // a page that no longer builds.
  test('retired /1.1/docs/ page carries no breadcrumb JSON-LD (route is gone)', async ({
    page,
  }) => {
    const response = await page.goto('/1.1/docs/release-notes/');
    expect(response?.status()).toBe(404);
  });

  // Wave 0 (convergence): /download/ must actually sell the current line —
  // a bare integration grid with no 2.1 row was the defect (A4 in
  // convergence-inventory.md). One card per current-line integration, each
  // labelled 'mod_pagespeed 2.1' next to its name.
  test('/download/ offers the current line (mod_pagespeed 2.1)', async ({ page }) => {
    await page.goto('/download/');
    for (const name of ['Apache', 'nginx', 'nginx (Docker)', 'Helm']) {
      await test.step(name, async () => {
        const card = page.locator('a.card', {
          has: page.getByRole('heading', { level: 3, name, exact: true }),
        });
        await expect(card).toContainText('mod_pagespeed 2.1');
      });
    }
  });

  test('/download/ JSON-LD name reflects the converged line', async ({ page }) => {
    await page.goto('/download/');
    const jsonLd = page.locator('script[type="application/ld+json"]').first();
    const content = await jsonLd.textContent();
    const data = JSON.parse(content!);
    expect(data.name).toBe('mod_pagespeed 2.1');
  });

  test('og:image is set on all pages', async ({ page }) => {
    for (const path of allPages) {
      await page.goto(path);
      const ogImage = page.locator('meta[property="og:image"]');
      const content = await ogImage.getAttribute('content');
      expect(content).toBeTruthy();
    }
  });

  test('twitter:card meta tag is present', async ({ page }) => {
    await page.goto('/');
    const twitterCard = page.locator('meta[name="twitter:card"]');
    await expect(twitterCard).toHaveAttribute('content', 'summary_large_image');
  });

  test('no duplicate title tags', async ({ page }) => {
    await page.goto('/');
    // Scope to the document head: the home page embeds inline architecture SVGs
    // that each carry their own accessible <svg><title>, so a bare `title`
    // locator now also matches those. The SEO invariant is exactly one
    // document <title> in <head>.
    const titles = page.locator('head > title');
    expect(await titles.count()).toBe(1);
  });

  test('favicon link is present', async ({ page }) => {
    await page.goto('/');
    const favicon = page.locator('link[rel="icon"]');
    expect(await favicon.count()).toBeGreaterThan(0);
  });
});
