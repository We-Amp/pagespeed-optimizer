// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// /buy/ is the O7 placeholder: license checkout retired with the open-source
// flip; support-plan checkout follows the pricing workstream. The page holds
// the URL (noindex), keeps the terms-version stamp rendered, and loads no
// FastSpring SBL.
test.describe('/buy/ — placeholder while support checkout is pending', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/buy/');
  });

  test('serves the placeholder with the sales contact', async ({ page }) => {
    await expect(page.locator('h1').first()).toContainText('Checkout opens with pricing');
    await expect(page.locator('a[href="mailto:sales@we-amp.com"]').first()).toBeVisible();
  });

  test('page stays noindex', async ({ page }) => {
    const robots = page.locator('meta[name="robots"]');
    await expect(robots).toHaveAttribute('content', /noindex/);
  });

  test('renders the terms-version stamp', async ({ page }) => {
    await expect(page.locator('[data-terms-version]')).toHaveAttribute(
      'data-terms-version',
      /^\d{4}-\d{2}$/,
    );
    await expect(page.locator('a[href="/terms/"]').first()).toBeVisible();
  });

  test('loads no FastSpring SBL', async ({ page }) => {
    const sbl = await page.locator('script[src*="fastspring"]').count();
    expect(sbl).toBe(0);
  });

  test('legacy SKU deep-links still land on the placeholder (receipts link here)', async ({
    page,
  }) => {
    await page.goto('/buy/?product=business-site-monthly');
    await expect(page.locator('h1').first()).toContainText('Checkout opens with pricing');
  });
});
