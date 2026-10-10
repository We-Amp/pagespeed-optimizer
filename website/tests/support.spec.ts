// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// /support/ is the commercial product page: what a subscription buys, the
// three tiers, signed packages and images (what ships today, how to
// verify), partners, consulting, existing customers, and the FAQ.
test.describe('Support page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/support/');
  });

  test('title, description and H1', async ({ page }) => {
    const title = await page.title();
    expect(title.length).toBeLessThanOrEqual(60);
    expect(title).toContain('mod_pagespeed 2.1');
    const description = await page.locator('meta[name="description"]').getAttribute('content');
    expect(description!.length).toBeLessThanOrEqual(155);
    await expect(page.locator('h1')).toHaveCount(1);
  });

  test('renders every section', async ({ page }) => {
    for (const id of [
      'what-you-get',
      'tiers',
      'hardened-builds',
      'verify',
      'hosting-partners',
      'consulting',
      'existing-customers',
      'reach-us',
      'faq',
    ]) {
      await expect(page.locator(`#${id}`), `#${id}`).toBeVisible();
    }
    await expect(page.locator('#tiers [data-tier]')).toHaveCount(3);
    await expect(page.locator('#tiers [data-pricing-status]')).toContainText('Pricing on request');
  });

  test('what a subscription buys names channel, response, updates and scope', async ({ page }) => {
    const readout = page.locator('#what-you-get dl');
    for (const key of ['channel:', 'response:', 'security updates:', 'scope:']) {
      await expect(readout).toContainText(key);
    }
    await expect(readout).toContainText('CET business days');
  });

  test('signed artifacts say only what ships today', async ({ page }) => {
    const section = page.locator('#hardened-builds');
    await expect(section).toContainText('GPG-signed');
    await expect(section).toContainText('SHA256SUMS');
    await expect(section).toContainText('SPDX');
    await expect(section).toContainText('cosign');
    await expect(section).toContainText('SBOM and VEX');
    await expect(section).toContainText('Enterprise adds custom build targets');
    // There is no subscriber repository and no early delivery of releases.
    await expect(section).not.toContainText('subscriber repository');
    await expect(section).not.toContainText('ahead of the public release');
    await expect(section).not.toContainText('hardened build pipeline');
    // The packages carry no per-build signed SBOM or provenance claim.
    await expect(section).not.toContainText('signed SBOM');
    await expect(section).not.toContainText('with every build');
    const recipe = await page.locator('#verify pre code').innerText();
    expect(recipe.trim().split('\n')).toHaveLength(4);
    expect(recipe).toContain('gpg --show-keys --with-fingerprint');
    expect(recipe).toContain('signed-by=');
    expect(recipe).toContain('sha256sum --check');
  });

  test('no price amount and no response-hour target', async ({ page }) => {
    const text = await page.locator('main').innerText();
    expect(text).not.toMatch(/[€$£]\s?\d/);
    expect(text).not.toMatch(/\d[\d.,]*\s*\/\s*(?:yr|year|mo|month)\b/i);
    expect(text).not.toMatch(/\d+\s*(?:business )?hours?\b/i);
  });

  test('commercial and security addresses are separate', async ({ page }) => {
    const reach = page.locator('#reach-us');
    await expect(reach.locator('a[href="mailto:info@we-amp.com"]')).toBeVisible();
    await expect(reach.locator('a[href="mailto:security@modpagespeed.com"]')).toBeVisible();
    await expect(page.locator(`a[href*="${['sales', 'we-amp.com'].join('@')}"]`)).toHaveCount(0);
  });

  test('existing customers are routed to the transfer topic', async ({ page }) => {
    await expect(
      page.locator('#existing-customers a[href="/contact/?topic=iispeed-transfer"]'),
    ).toBeVisible();
  });

  test('FAQ has eight questions and the FAQPage JSON-LD mirrors them', async ({ page }) => {
    const visible = (await page.locator('#faq details > summary').allInnerTexts()).map((t) =>
      t.trim(),
    );
    expect(visible).toHaveLength(8);
    const blocks = await page.locator('script[type="application/ld+json"]').allTextContents();
    const faq = blocks.map((b) => JSON.parse(b)).find((d) => d['@type'] === 'FAQPage');
    expect(faq).toBeTruthy();
    expect(faq.mainEntity.map((q: { name: string }) => q.name)).toEqual(visible);
    for (const q of faq.mainEntity) {
      expect(q.acceptedAnswer.text).not.toMatch(/<[^>]+>/);
    }
  });

  test('every paid CTA carries cta_commercial with offer, surface and position', async ({
    page,
  }) => {
    const ctas = page.locator('main [data-umami-event="cta_commercial"]');
    const count = await ctas.count();
    expect(count).toBeGreaterThanOrEqual(8);
    const offers = new Set<string>();
    for (let i = 0; i < count; i++) {
      const cta = ctas.nth(i);
      const offer = await cta.getAttribute('data-umami-event-offer');
      expect(['support', 'hardened', 'hosting', 'consulting', 'agentic']).toContain(offer);
      offers.add(offer!);
      await expect(cta).toHaveAttribute('data-umami-event-surface', 'support');
      await expect(cta).toHaveAttribute('data-umami-event-position', /\S/);
    }
    expect([...offers].sort()).toEqual(['consulting', 'hardened', 'hosting', 'support']);
  });

  test('consulting hands off to we-amp.com', async ({ page }) => {
    await expect(page.locator('#consulting a[href="https://we-amp.com/consulting/"]')).toBeVisible();
    await expect(page.locator('#consulting')).toContainText('fixed-price');
  });
});
