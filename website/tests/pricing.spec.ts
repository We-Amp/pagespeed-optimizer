// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

const STANDARD_CHECKOUT = 'https://mpspro.onfastspring.com/support-standard-annual';
const PRIORITY_CHECKOUT = 'https://mpspro.onfastspring.com/support-priority-annual';

// /pricing/: the software is free; support is sold in three tiers with
// published yearly prices (euro, excl. VAT). Standard and Priority go straight
// to the hosted checkout; Enterprise and the audit go to the contact form.
test.describe('Pricing page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/pricing/');
  });

  test('hero states the model, without the word free in the H1', async ({ page }) => {
    const h1 = page.locator('h1').first();
    await expect(h1).toHaveText('Run it on any number of servers. Pay us for support.');
    expect(await h1.innerText()).not.toMatch(/\bfree\b/i);
    await expect(page.locator('[data-pricing-terms]')).toHaveText(
      'Prices are per year, billed annually, excl. VAT.',
    );
  });

  test('renders the three tiers with prices, Priority emphasized', async ({ page }) => {
    const cards = page.locator('#tiers [data-tier]');
    await expect(cards).toHaveCount(3);
    for (const name of ['Standard support', 'Priority support', 'Enterprise']) {
      await expect(
        page.locator('#tiers').getByRole('heading', { name, exact: true }),
      ).toBeVisible();
    }
    await expect(page.locator('#tiers [data-tier="priority"]')).toHaveClass(/card-featured/);
    await expect(page.locator('#tiers [data-tier="priority"]')).toContainText(
      'Private package repository (being set up)',
    );
    await expect(page.locator('#tiers [data-tier="standard"]')).not.toContainText(
      'Private package repository',
    );
    await expect(page.locator('#tiers [data-tier="standard"] [data-tier-price]')).toContainText(
      '€490',
    );
    await expect(page.locator('#tiers [data-tier="priority"] [data-tier-price]')).toContainText(
      '€1,900',
    );
    const ent = page.locator('#tiers [data-tier="enterprise"] [data-tier-price]');
    await expect(ent).toContainText('From');
    await expect(ent).toContainText('€7,500');
  });

  test('states the first-response targets per tier, the price hold and the audit price', async ({
    page,
  }) => {
    const tiers = page.locator('#tiers');
    await expect(tiers.locator('[data-tier="standard"]')).toContainText(
      'First response within 2 business days',
    );
    await expect(tiers.locator('[data-tier="priority"]')).toContainText(
      'First response next business day; severity 1 (production down) within 8 business hours',
    );
    await expect(tiers.locator('[data-tier="enterprise"]')).toContainText(
      'Severity 1 (production down) within 4 business hours; other severities per agreement',
    );
    await expect(tiers.locator('[data-pricing-status]')).toContainText(
      'The price you sign at holds for one year',
    );
    await expect(page.locator('[data-audit-price]')).toContainText('€950 per site, excl. VAT.');
  });

  test('no "on request" pricing, no quote form, no USD', async ({ page }) => {
    const text = await page.locator('body').innerText();
    expect(text).not.toMatch(/pricing on request/i);
    expect(text).not.toMatch(/ask for a quote/i);
    expect(text).not.toMatch(/\$\s?\d/);
    await expect(page.locator('#quote')).toHaveCount(0);
    await expect(page.locator('main form')).toHaveCount(0);
  });

  test('Buy buttons go straight to the hosted checkout, same tab', async ({ page }) => {
    for (const [tier, href] of [
      ['standard', STANDARD_CHECKOUT],
      ['priority', PRIORITY_CHECKOUT],
    ]) {
      const cta = page.locator(`#tiers [data-tier="${tier}"] a`);
      await expect(cta).toHaveAttribute('href', href);
      await expect(cta).toHaveAttribute('rel', /noopener/);
      await expect(cta).not.toHaveAttribute('target', /.+/);
    }
    await expect(page.locator('#tiers [data-tier="enterprise"] a')).toHaveAttribute(
      'href',
      '/contact/?topic=enterprise',
    );
    await expect(page.locator('#audit a[href="/contact/?topic=audit"]')).toBeVisible();
  });

  test('hosting partner strip keeps its rate unpublished', async ({ page }) => {
    await expect(page.locator('#hosting-strip a[href="/hosting-partners/"]')).toBeVisible();
    await expect(page.locator('#hosting-strip')).toContainText(
      'Partner terms are agreed per fleet.',
    );
    await expect(page.locator('main a[href="https://we-amp.com/consulting/"]')).toBeVisible();
  });

  test('paid CTAs carry the cta_commercial event with the new offers', async ({ page }) => {
    const ctas = page.locator('main [data-umami-event="cta_commercial"]');
    expect(await ctas.count()).toBeGreaterThanOrEqual(8);
    for (const [tier, offer] of [
      ['standard', 'buy-standard'],
      ['priority', 'buy-priority'],
      ['enterprise', 'enterprise'],
    ]) {
      const cta = page.locator(`#tiers [data-tier="${tier}"] a`);
      await expect(cta).toHaveAttribute('data-umami-event', 'cta_commercial');
      await expect(cta).toHaveAttribute('data-umami-event-offer', offer);
      await expect(cta).toHaveAttribute('data-umami-event-surface', 'pricing');
      await expect(cta).toHaveAttribute('data-umami-event-position', `tier_${tier}`);
    }
    await expect(page.locator('#audit a[href="/contact/?topic=audit"]')).toHaveAttribute(
      'data-umami-event-offer',
      'audit',
    );
    await expect(page.locator('#hosting-strip a')).toHaveAttribute(
      'data-umami-event-offer',
      'hosting',
    );
    await expect(page.locator('[data-closing-cta] a')).toHaveCount(3);
  });

  test('one FAQ of at most thirteen questions, none from the retired 2.0 block', async ({
    page,
  }) => {
    const questions = page.locator('#faq details > summary');
    const count = await questions.count();
    expect(count).toBeGreaterThan(0);
    expect(count).toBeLessThanOrEqual(13);
    const body = page.locator('body');
    for (const retired of [
      'How does the nginx integration work?',
      'Does it work with Apache?',
      'Which integrations are supported?',
      'Can I run mod_pagespeed under ASP.NET Core?',
      'there is no separate bare-metal module to install',
    ]) {
      await expect(body).not.toContainText(retired);
    }
  });

  test('FAQPage JSON-LD mirrors the visible FAQ; the offers carry the tier prices', async ({
    page,
  }) => {
    const blocks = await page.locator('script[type="application/ld+json"]').allTextContents();
    const graph = blocks.flatMap((b) => {
      const d = JSON.parse(b);
      return d['@graph'] ?? [d];
    });
    const faq = graph.find((n: { '@type': string }) => n['@type'] === 'FAQPage');
    expect(faq).toBeTruthy();
    const visible = await page.locator('#faq details > summary').allInnerTexts();
    expect(faq.mainEntity.map((q: { name: string }) => q.name)).toEqual(
      visible.map((v) => v.trim()),
    );
    const app = graph.find((n: { '@type': string }) => n['@type'] === 'SoftwareApplication');
    const offers: Array<{
      price?: number;
      priceCurrency?: string;
      priceValidUntil: string;
      priceSpecification?: { minPrice: number; priceCurrency: string };
    }> = app.offers;
    expect(offers[0].price).toBe(0);
    const paid = offers.slice(1);
    expect(paid.map((o) => o.price)).toEqual([490, 1900, undefined]);
    expect(paid[2].priceSpecification).toMatchObject({ minPrice: 7500, priceCurrency: 'EUR' });
    for (const o of paid.slice(0, 2)) {
      expect(o.priceCurrency).toBe('EUR');
      expect(o.priceValidUntil).toBe(offers[0].priceValidUntil);
    }
  });

  test('no retired checkout path and no retired addresses', async ({ page }) => {
    await expect(page.locator('a[href^="/buy/"]')).toHaveCount(0);
    await expect(page.locator(`a[href*="${['sales', 'we-amp.com'].join('@')}"]`)).toHaveCount(0);
    await expect(page.locator('#start a[href="mailto:info@we-amp.com"]')).toBeVisible();
  });

  test('the page names the terms version and links the support terms', async ({ page }) => {
    await expect(page.locator('[data-terms-version]')).toHaveAttribute(
      'data-terms-version',
      /^\d{4}-\d{2}(-\d{2})?$/,
    );
    await expect(page.locator('[data-terms-version] a[href="/terms/"]')).toBeVisible();
    await expect(page.locator('#start a[href="https://we-amp.com/licensing/"]')).toBeVisible();
  });
});
