// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

const blogPosts = [
  'why-i-rebuilt-mod-pagespeed',
  'migrating-from-1x',
  'critical-css-heuristics',
  'economics-of-image-optimization',
  'save-data-bandwidth',
  'viewport-aware-image-optimization',
  'benchmarking-real-numbers',
  'aspnet-core-middleware',
];

test.describe('Blog', () => {
  test('blog index lists all posts', async ({ page }) => {
    await page.goto('/blog/');
    // Each post should have a link
    for (const slug of blogPosts) {
      await expect(page.locator(`a[href="/blog/${slug}/"]`).first()).toBeAttached();
    }
  });

  test('blog index has correct page title', async ({ page }) => {
    await page.goto('/blog/');
    await expect(page).toHaveTitle(/Blog/i);
  });

  test('blog index shows publication dates', async ({ page }) => {
    await page.goto('/blog/');
    const times = page.locator('time[datetime]');
    expect(await times.count()).toBeGreaterThanOrEqual(blogPosts.length);
  });

  for (const slug of blogPosts) {
    test(`blog post "${slug}" renders`, async ({ page }) => {
      await page.goto(`/blog/${slug}/`);
      // Post should have a title (h1)
      const h1 = page.locator('main h1').first();
      await expect(h1).toBeVisible();
      const title = await h1.textContent();
      expect(title!.trim().length).toBeGreaterThan(0);
    });
  }

  test('blog post page has BaseLayout', async ({ page }) => {
    await page.goto('/blog/why-i-rebuilt-mod-pagespeed/');
    await expect(page.locator('header nav')).toBeVisible();
    await expect(page.getByRole('contentinfo')).toBeVisible();
  });

  test('blog post page has publication date', async ({ page }) => {
    await page.goto('/blog/why-i-rebuilt-mod-pagespeed/');
    await expect(page.locator('time[datetime]')).toBeAttached();
  });

  // Regression guard for the GSC sanity-check finding: every blog post used
  // to share /og-default.png as its og:image + JSON-LD image, which weakened
  // social snippets and Discover surfacing. The build-time title-card
  // generator (`scripts/generate-title-cards.mjs`) now writes per-post PNGs
  // to /og-cards/{slug}.png. Assert two posts advertise DISTINCT images, and
  // that neither falls back to the default — if the generator silently
  // breaks or the renderer regresses, this test catches it.
  test('blog posts advertise distinct per-post og:image', async ({ page }) => {
    await page.goto('/blog/why-i-rebuilt-mod-pagespeed/');
    const a = await page.locator('meta[property="og:image"]').first().getAttribute('content');

    await page.goto('/blog/migrating-from-1x/');
    const b = await page.locator('meta[property="og:image"]').first().getAttribute('content');

    expect(a).toBeTruthy();
    expect(b).toBeTruthy();
    expect(a).not.toBe(b);
    expect(a).not.toMatch(/og-default\.png(\?|$)/);
    expect(b).not.toMatch(/og-default\.png(\?|$)/);
    // Cards carry a `?v=` cache-busting query, so match the filename, not EOL.
    expect(a).toMatch(/\/og-cards\/why-i-rebuilt-mod-pagespeed\.png(\?|$)/);
    expect(b).toMatch(/\/og-cards\/migrating-from-1x\.png(\?|$)/);
  });

  // The chart lightbox (cyclone-cache benchmark post) is an aria-modal dialog:
  // Tab must stay inside it while it is open, and closing returns focus to
  // the button that opened it.
  test.describe('chart lightbox', () => {
    const post = '/blog/cyclone-cache-vs-file-cache-benchmark/';

    test('traps Tab inside while open, Escape closes and restores focus', async ({ page }) => {
      await page.goto(post);
      const zoom = page.locator('.cyc-zoom').first();
      await zoom.scrollIntoViewIfNeeded();
      await zoom.click();
      const lb = page.locator('#cyc-lightbox');
      await expect(lb).toBeVisible();
      await expect(page.locator('.cyc-lb-close')).toBeFocused();

      const focusInside = () =>
        page.evaluate(() => {
          const el = document.activeElement;
          return Boolean(el && el.closest && el.closest('#cyc-lightbox'));
        });
      for (const key of ['Tab', 'Tab', 'Shift+Tab', 'Shift+Tab']) {
        await page.keyboard.press(key);
        expect(await focusInside()).toBe(true);
      }

      await page.keyboard.press('Escape');
      await expect(lb).toBeHidden();
      await expect(zoom).toBeFocused();
    });

    test('outside click closes the lightbox', async ({ page }) => {
      await page.goto(post);
      const zoom = page.locator('.cyc-zoom').first();
      await zoom.scrollIntoViewIfNeeded();
      await zoom.click();
      const lb = page.locator('#cyc-lightbox');
      await expect(lb).toBeVisible();
      // The backdrop is the dialog element itself; the panel is centred.
      await lb.click({ position: { x: 4, y: 4 } });
      await expect(lb).toBeHidden();
    });
  });
});
