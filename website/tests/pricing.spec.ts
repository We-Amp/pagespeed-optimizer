// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// /pricing/: the software is free; support is sold in three tiers with hardened
// builds from Priority up. Owner ruling: no price amounts and no per-tier
// response-time targets anywhere on the page, and a quote form instead of a
// checkout.
test.describe('Pricing page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/pricing/');
  });

  test('hero states the model', async ({ page }) => {
    await expect(page.locator('h1').first()).toHaveText(
      'The software is free. We sell support and hardened builds.',
    );
  });

  test('renders the three tiers, Priority emphasized', async ({ page }) => {
    const cards = page.locator('#plans [data-tier]');
    await expect(cards).toHaveCount(3);
    for (const name of ['Standard support', 'Priority support', 'Enterprise']) {
      await expect(page.locator('#plans').getByRole('heading', { name, exact: true })).toBeVisible();
    }
    await expect(page.locator('#plans [data-tier="priority"]')).toHaveClass(/card-featured/);
    await expect(page.locator('#plans [data-tier="priority"]')).toContainText('Hardened builds');
    await expect(page.locator('#plans [data-tier="standard"]')).not.toContainText(
      'Hardened builds',
    );
  });

  test('states pricing on request and the early-subscriber rate, once', async ({ page }) => {
    const status = page.locator('#plans [data-pricing-status]');
    await expect(status).toContainText('Pricing on request');
    await expect(status).toContainText('early subscribers keep their quoted rate for three years');
    await expect(page.locator('#plans')).toContainText(
      'Response targets by severity are stated in your quote.',
    );
  });

  test('no price amount and no per-period rate anywhere on the page', async ({ page }) => {
    const text = await page.locator('body').innerText();
    // Currency symbols or codes next to a number, and "/yr" or "/mo" rates.
    expect(text).not.toMatch(/[€$£]\s?\d/);
    expect(text).not.toMatch(/\d[\d.,]*\s?(?:EUR|USD|GBP)\b/);
    expect(text).not.toMatch(/\d[\d.,]*\s*\/\s*(?:yr|year|mo|month)\b/i);
    expect(text).not.toMatch(/per (?:year|month)/i);
    // No per-tier response-hour targets either.
    expect(text).not.toMatch(/\d+\s*(?:business )?hours?\b/i);
  });

  test('hosting partner strip and consulting line', async ({ page }) => {
    await expect(page.locator('#hosting-strip a[href="/hosting-partners/"]')).toBeVisible();
    await expect(page.locator('main a[href="https://we-amp.com/consulting/"]')).toBeVisible();
  });

  test('quote form has the qualifying fields', async ({ page }) => {
    const form = page.locator('#quote-form');
    await expect(form.locator('#quote-name')).toHaveAttribute('required', '');
    await expect(form.locator('#quote-email')).toHaveAttribute('required', '');
    await expect(form.locator('#quote-org')).toBeVisible();
    await expect(form.locator('input[name="servers"]')).toHaveCount(3);
    await expect(form.locator('input[name="platforms"]')).not.toHaveCount(0);
    await expect(form.locator('#quote-traffic')).toBeVisible();
    await expect(form.locator('#quote-deadline')).toBeVisible();
    await expect(form.locator('#quote-early')).toBeVisible();
    await expect(form.locator('#quote-message')).toBeVisible();
    // The endpoint treats `company` as a bot honeypot: the form must not use it.
    await expect(form.locator('[name="company"]')).toHaveCount(0);
  });

  test('what happens next lists the three commitments', async ({ page }) => {
    const aside = page.locator('#quote aside');
    await expect(aside).toContainText('within 1 business day');
    await expect(aside).toContainText('within 3 business days');
    await expect(aside).toContainText('within 5 business days of signing');
  });

  test('quote submit posts topic quote with the qualifiers folded in', async ({ page }) => {
    let posted: Record<string, unknown> | null = null;
    await page.route('**/ai-readability/api/contact', async (route) => {
      posted = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await stubUmami(page);
    await page.goto('/pricing/');

    await page.fill('#quote-name', 'Ada');
    await page.fill('#quote-email', 'ada@example.com');
    await page.fill('#quote-org', 'Example Hosting');
    await page.locator('input[name="servers"][data-band="up-to-25"]').check();
    await page.locator('input[name="platforms"][value="nginx"]').check();
    await page.locator('input[name="platforms"][value="Apache"]').check();
    await page.selectOption('#quote-deadline', 'This quarter');
    await page.locator('#quote-early').check();
    await page.fill('#quote-message', 'Two data centres.');
    await page.locator('#quote-submit').click();

    await expect(page.locator('#quote-status')).toContainText('Thanks');
    expect(posted).toMatchObject({ topic: 'quote', name: 'Ada', email: 'ada@example.com' });
    expect(posted).not.toHaveProperty('company');
    const message = String((posted as unknown as { message: string }).message);
    expect(message).toContain('Company: Example Hosting');
    expect(message).toContain('Production servers: Up to 25');
    expect(message).toContain('Platforms: Apache, nginx');
    expect(message).toContain('Deadline: This quarter');
    expect(message).toContain('Early-subscriber rate: yes');
    expect(message).toContain('Two data centres.');

    const events = await trackedEvents(page);
    expect(events).toContainEqual({
      name: 'lead_submit',
      data: { channel: 'quote', topic: 'quote', source_path: '/pricing/' },
    });
  });

  test('paid CTAs carry the cta_commercial event and its properties', async ({ page }) => {
    const ctas = page.locator('main [data-umami-event="cta_commercial"]');
    expect(await ctas.count()).toBeGreaterThanOrEqual(6);
    for (const tier of ['standard', 'priority', 'enterprise']) {
      const cta = page.locator(`#plans [data-tier="${tier}"] a`);
      await expect(cta).toHaveAttribute('data-umami-event', 'cta_commercial');
      await expect(cta).toHaveAttribute('data-umami-event-offer', 'support');
      await expect(cta).toHaveAttribute('data-umami-event-surface', 'pricing');
      await expect(cta).toHaveAttribute('data-umami-event-position', `tier_${tier}`);
    }
    await expect(page.locator('#hosting-strip a')).toHaveAttribute(
      'data-umami-event-offer',
      'hosting',
    );
  });

  test('one FAQ of at most eight questions, none from the retired 2.0 block', async ({ page }) => {
    const questions = page.locator('#faq details > summary');
    const count = await questions.count();
    expect(count).toBeGreaterThan(0);
    expect(count).toBeLessThanOrEqual(8);
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

  test('FAQPage JSON-LD mirrors the visible FAQ', async ({ page }) => {
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
    // The software offer is free; support gets no priced Offer node.
    const app = graph.find((n: { '@type': string }) => n['@type'] === 'SoftwareApplication');
    expect(app.offers.price).toBe(0);
    expect(JSON.stringify(graph)).not.toMatch(/"price":\s*[1-9]/);
  });

  test('no checkout links and no retired addresses', async ({ page }) => {
    await expect(page.locator('a[href^="/buy/"]')).toHaveCount(0);
    await expect(page.locator(`a[href*="${['sales', 'we-amp.com'].join('@')}"]`)).toHaveCount(0);
    await expect(page.locator('a[href="mailto:info@we-amp.com"]').first()).toBeVisible();
  });

  test('the quote form names the terms version', async ({ page }) => {
    await expect(page.locator('#quote-form [data-terms-version]')).toHaveAttribute(
      'data-terms-version',
      /^\d{4}-\d{2}$/,
    );
    await expect(page.locator('#quote-form a[href="/terms/"]')).toBeVisible();
    await expect(page.locator('a[href="https://we-amp.com/licensing/"]').first()).toBeVisible();
  });
});
