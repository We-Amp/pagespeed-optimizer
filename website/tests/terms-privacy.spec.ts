// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('/terms/ page', () => {
  test('has correct page title', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page).toHaveTitle(/Terms of Service/i);
  });

  test('has H1 heading', async ({ page }) => {
    await page.goto('/terms/');
    // Scope to the in-page h1 — the Astro dev toolbar injects its own h1 nodes
    // (outside <main>), so a bare `h1` locator is not strict-mode safe in dev.
    await expect(page.locator('main h1')).toHaveText(/Terms of Service/i);
  });

  test('displays version identifier 2026-09', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('2026-09')).toBeVisible();
  });

  test('displays effective date September 2026', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('September 2026')).toBeVisible();
  });

  test('links to privacy policy', async ({ page }) => {
    await page.goto('/terms/');
    // The privacy policy is now served locally at /privacy/ (was an off-site
    // link to www.we-amp.com/privacy/).
    const privacyLink = page.locator('main a[href="/privacy/"]');
    await expect(privacyLink.first()).toBeVisible();
  });

  test('links to /license/', async ({ page }) => {
    await page.goto('/terms/');
    const licenseLink = page.locator('main a[href="/license/"]');
    await expect(licenseLink.first()).toBeVisible();
  });

  test('has info@we-amp.com contact link', async ({ page }) => {
    await page.goto('/terms/');
    const emailLink = page.locator('a[href="mailto:info@we-amp.com"]');
    await expect(emailLink.first()).toBeVisible();
  });

  test('uses BaseLayout (header nav + footer)', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.locator('header nav')).toBeVisible();
    await expect(page.locator('footer')).toBeVisible();
  });

  test('displays company name We-Amp B.V. and KvK 57898138', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('We-Amp B.V.').first()).toBeVisible();
    await expect(page.getByText('57898138').first()).toBeVisible();
  });

  test('mentions the 14-day consumer withdrawal right', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('14-day right of withdrawal').first()).toBeVisible();
  });

  test('states the software license and the subscription model, no retired model', async ({
    page,
  }) => {
    await page.goto('/terms/');
    const main = page.locator('main');
    await expect(main).toContainText('Apache License 2.0');
    await expect(main).toContainText('subscriber repository');
    await expect(main).not.toContainText('X-PageSpeed-Warn');
    await expect(main).not.toContainText('unlicensed');
    await expect(main).not.toContainText('license key');
  });

  test('mentions Netherlands (governing law)', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('Netherlands').first()).toBeVisible();
  });
});

test.describe('/privacy/ page', () => {
  test('is served locally and references the corporate policy', async ({ page }) => {
    // The privacy policy is now a local page (it previously redirected off-site
    // to www.we-amp.com/privacy/). It must render in place — no off-site
    // redirect — and point to the corporate policy for the full text.
    const response = await page.goto('/privacy/');
    expect(response?.status()).toBe(200);
    expect(page.url()).toContain('/privacy/');
    expect(page.url()).not.toContain('we-amp.com');
    await expect(page.locator('main h1')).toContainText(/Privacy/i);
    await expect(page.locator('a[href="https://we-amp.com/privacy/"]').first()).toBeVisible();
  });

  test('describes the software as sending nothing, no heartbeat', async ({ page }) => {
    await page.goto('/privacy/');
    const main = page.locator('main');
    await expect(main).toContainText('sends no telemetry');
    await expect(main).not.toContainText('heartbeat');
    await expect(main).not.toContainText('license key');
    await expect(main).not.toContainText('license token');
    // GeoIP is a local lookup on our own server; the DB-IP Lite attribution stays.
    await expect(main).toContainText('not sent to DB-IP');
    await expect(main.locator('a[href="https://db-ip.com"]')).toBeVisible();
  });
});
