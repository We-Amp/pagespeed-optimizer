// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// The QuickMessage widget is mounted in BaseLayout, so it renders on every page.
test.describe('QuickMessage widget', () => {
  test('launcher is present site-wide', async ({ page }) => {
    for (const path of ['/', '/pricing/', '/blog/']) {
      await page.goto(path);
      await expect(page.locator('[data-qm-launcher]')).toBeVisible();
    }
  });

  test('opens on click, focuses the textarea, closes on Escape', async ({ page }) => {
    await page.goto('/');
    const launcher = page.locator('[data-qm-launcher]');
    const panel = page.locator('[data-qm-panel]');

    await expect(panel).toBeHidden();
    await launcher.click();
    await expect(panel).toBeVisible();
    await expect(page.locator('[data-qm-message]')).toBeFocused();
    await expect(launcher).toHaveAttribute('aria-expanded', 'true');

    await page.keyboard.press('Escape');
    await expect(panel).toBeHidden();
    await expect(launcher).toHaveAttribute('aria-expanded', 'false');
    await expect(launcher).toBeFocused();
  });

  test('optional email field is collapsed by default and reveals on demand', async ({ page }) => {
    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    await expect(page.locator('[data-qm-email-wrap]')).toBeHidden();
    await page.locator('[data-qm-email-toggle]').click();
    await expect(page.locator('[data-qm-email-wrap]')).toBeVisible();
    await expect(page.locator('[data-qm-email]')).toBeFocused();
  });

  test('sends a one-way message (no email) and shows success', async ({ page }) => {
    let posted: any = null;
    await page.route('**/ai-readability/api/contact', async (route) => {
      posted = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });

    await page.goto('/pricing/');
    await page.locator('[data-qm-launcher]').click();
    await page.locator('[data-qm-message]').fill('Does this work behind Cloudflare?');
    await page.locator('[data-qm-submit]').click();

    await expect(page.locator('[data-qm-success]')).toBeVisible();
    expect(posted).toMatchObject({
      message: 'Does this work behind Cloudflare?',
      email: '',
      topic: 'quick-message',
      // PATH ONLY — never the full href / query string.
      url: '/pricing/',
    });
  });

  test('requires a non-empty message before sending', async ({ page }) => {
    let hits = 0;
    await page.route('**/ai-readability/api/contact', async (route) => {
      hits += 1;
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });

    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    await page.locator('[data-qm-submit]').click();
    await expect(page.locator('[data-qm-status]')).toContainText('Type a message');
    expect(hits).toBe(0);
  });

  test('rejects a malformed email but keeps the message', async ({ page }) => {
    let hits = 0;
    await page.route('**/ai-readability/api/contact', async (route) => {
      hits += 1;
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });

    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    await page.locator('[data-qm-message]').fill('hello there');
    await page.locator('[data-qm-email-toggle]').click();
    await page.locator('[data-qm-email]').fill('not-an-email');
    await page.locator('[data-qm-submit]').click();
    await expect(page.locator('[data-qm-status]')).toContainText("doesn't look right");
    expect(hits).toBe(0);
  });

  test('shows a privacy notice naming Telegram + linking the policy', async ({ page }) => {
    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    const privacy = page.locator('.qm-privacy');
    await expect(privacy).toContainText('Telegram');
    await expect(privacy.locator('a[href="/privacy/"]')).toBeVisible();
  });
});
