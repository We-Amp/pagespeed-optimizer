// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// /pricing/ is the support page (converged model): the software is free, the
// two offerings in SUPPORT_TIERS (support subscription; hardened attested
// builds) render as placeholder-priced cards alongside the two-lane artifacts
// section (standard free / hardened paid, pricing TBA), and no surface may show
// a price until published.
test.describe('Support plans page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/pricing/');
  });

  test('hero states the support model', async ({ page }) => {
    const hero = page.locator('h1').first();
    await expect(hero).toContainText('free');
    await expect(hero).toContainText("Support — and hardened builds — are what's for sale");
  });

  test('renders the two offerings as placeholder-priced cards, no tier names', async ({ page }) => {
    await expect(page.locator('#plans .card')).toHaveCount(2);
    for (const name of ['Support subscription', 'Hardened attested builds']) {
      await expect(page.locator('#plans').getByText(name, { exact: true })).toBeVisible();
    }
    // Placeholder pricing on every card; never a dollar figure; no tier ladder.
    await expect(page.locator('#plans').getByText('Pricing to be announced')).toHaveCount(2);
    await expect(page.locator('#plans')).not.toContainText('$');
    for (const retired of ['Starter', 'Business', 'Enterprise', 'Hoster']) {
      await expect(page.locator('#plans')).not.toContainText(retired);
    }
  });

  test('every offering card carries a contact CTA, no checkout links', async ({ page }) => {
    await expect(page.locator('#plans a.btn-primary')).toHaveCount(2);
    await expect(page.locator('#plans a[href^="/buy/"]')).toHaveCount(0);
    await expect(page.locator('#plans a[href="/contact/?topic=support"]').first()).toBeVisible();
    await expect(page.locator('#plans a[href="/contact/?topic=enterprise"]').first()).toBeVisible();
    await expect(page.locator('a[href="/hosting-partners/"]').first()).toBeVisible();
  });

  test('no license-era copy survives', async ({ page }) => {
    const body = page.locator('body');
    await expect(body).not.toContainText('requires a commercial license');
    await expect(body).not.toContainText('X-PageSpeed-Warn');
    await expect(body).not.toContainText('licensed per site');
  });

  test('artifact lanes render: standard free, hardened priced-TBA', async ({ page }) => {
    await expect(page.locator('#artifacts .card')).toHaveCount(2);
    const lanes = page.locator('#artifacts');
    await expect(lanes.getByRole('heading', { name: 'Standard packages' })).toBeVisible();
    await expect(lanes.getByRole('heading', { name: 'Hardened builds' })).toBeVisible();
    await expect(page.locator('#artifacts')).not.toContainText('$');
  });

  test('links the terms of service and the support terms', async ({ page }) => {
    await expect(page.locator('main a[href="/terms/"]').first()).toBeVisible();
    await expect(page.locator('a[href="https://we-amp.com/licensing/"]').first()).toBeVisible();
  });

  test('FAQ renders from the shared + product entries', async ({ page }) => {
    const count = await page.locator('details > summary').count();
    expect(count).toBeGreaterThanOrEqual(8);
  });

  test('getting-started strip is free-software framed', async ({ page }) => {
    await expect(page.getByRole('heading', { name: 'How to get started' })).toBeVisible();
    await expect(page.getByText("Run it — it's yours")).toBeVisible();
  });
});
