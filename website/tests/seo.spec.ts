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

  // Product JSON-LD pages use deliberately asymmetric @type checks:
  //   /1.1/      — strict 'Product' (single schema type today)
  //   /download/ — accepts ['Product', 'SoftwareApplication']
  // If /1.1/ later becomes an array (e.g., gains SoftwareApplication), the strict
  // check here fails loudly so we review the schema shape intentionally.
  const productPages = [
    { path: '/1.1/', expectType: (t: unknown) => t === 'Product' },
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
