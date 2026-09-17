// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

const PAGES_FOR_AXE_SCAN = [
  '/',
  '/features/',
  '/pricing/',
  '/demo/',
  '/docs/',
  '/license/',
  '/contact/',
  '/calculator/',
  '/blog/',
  '/security/',
  '/terms/',
  // /privacy/ redirects to https://www.we-amp.com/privacy/ (301)
];

test.describe('Accessibility', () => {
  test('skip-to-content link is first focusable element', async ({ page }) => {
    await page.goto('/');
    // Tab to first focusable element
    await page.keyboard.press('Tab');
    const focused = page.locator(':focus');
    await expect(focused).toHaveAttribute('href', '#main-content');
  });

  test('all images have alt text', async ({ page }) => {
    await page.goto('/');
    const images = page.locator('img');
    const count = await images.count();
    for (let i = 0; i < count; i++) {
      const img = images.nth(i);
      const alt = await img.getAttribute('alt');
      // alt="" is valid for decorative images; only fail if alt is missing entirely
      expect(alt !== null, `Image ${i} is missing alt attribute`).toBeTruthy();
    }
  });

  test('calculator inputs have associated labels', async ({ page }) => {
    await page.goto('/calculator/');
    // ADR-092 per-site licensing renamed the "servers" input to "sites".
    const inputIds = ['pageviews', 'pageweight', 'bwcost', 'sites'];
    for (const id of inputIds) {
      const label = page.locator(`label[for="${id}"]`);
      await expect(label, `Label for ${id} should exist`).toBeAttached();
    }
  });

  test('heading hierarchy is correct (no skips)', async ({ page }) => {
    await page.goto('/');
    const headings = page.locator('main h1, main h2, main h3, main h4, main h5, main h6');
    const count = await headings.count();
    let lastLevel = 0;

    for (let i = 0; i < count; i++) {
      const tag = await headings.nth(i).evaluate((el) => el.tagName.toLowerCase());
      const level = parseInt(tag.charAt(1));
      // Should not skip more than 1 level (e.g., h1 -> h3 is bad)
      if (lastLevel > 0) {
        expect(level <= lastLevel + 1, `Heading skip: h${lastLevel} -> h${level}`).toBeTruthy();
      }
      lastLevel = level;
    }
  });

  test('pricing toggle has appropriate ARIA', async ({ page }) => {
    await page.goto('/pricing/');
    const toggle = page.locator('#billing-toggle');
    // Billing toggle only exists when live pricing is enabled (prices are
    // currently TBD). Skip gracefully when the element is absent.
    if ((await toggle.count()) === 0) {
      test.skip();
      return;
    }
    await expect(toggle).toHaveAttribute('role', 'switch');
    await expect(toggle).toHaveAttribute('aria-checked', /true|false/);
    await expect(toggle).toHaveAttribute('aria-label', /billing/i);
  });

  test('mobile menu button has aria-label', async ({ page }) => {
    await page.goto('/');
    const btn = page.locator('#mobile-menu-btn');
    await expect(btn).toHaveAttribute('aria-label', /menu/i);
  });

  test('page has lang attribute on <html>', async ({ page }) => {
    await page.goto('/');
    const html = page.locator('html');
    await expect(html).toHaveAttribute('lang', 'en');
  });

  test('links have discernible text', async ({ page }) => {
    await page.goto('/');
    const links = page.locator('a:not([aria-hidden="true"])');
    const count = await links.count();
    for (let i = 0; i < count; i++) {
      const link = links.nth(i);
      const text = await link.textContent();
      const ariaLabel = await link.getAttribute('aria-label');
      const title = await link.getAttribute('title');
      const hasText = (text && text.trim().length > 0) || ariaLabel || title;
      // Skip SVG-only decorative links
      const hasImg = (await link.locator('img, svg').count()) > 0;
      if (!hasImg) {
        expect(
          hasText,
          `Link ${i} (href=${await link.getAttribute('href')}) has no discernible text`,
        ).toBeTruthy();
      }
    }
  });

  test('main navigation has aria-label', async ({ page }) => {
    await page.goto('/');
    const nav = page.locator('header nav');
    await expect(nav).toHaveAttribute('aria-label', /navigation/i);
  });

  // Phase 4.1: WCAG 2.1 AA axe-core scan on all buyer-facing pages.
  for (const pagePath of PAGES_FOR_AXE_SCAN) {
    test(`axe-core WCAG 2.1 AA scan: ${pagePath}`, async ({ page }) => {
      await page.goto(pagePath);

      const builder = new AxeBuilder({ page })
        .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']);

      // Demo page embeds third-party demo sites in iframes with intentionally
      // non-optimized assets. Exclude iframe content and scrollable demo panels.
      if (pagePath === '/demo/') {
        builder.exclude('iframe').exclude('[role="tabpanel"]').exclude('pre');
      }

      const results = await builder.analyze();

      const violations = results.violations.filter(
        (v) => v.impact === 'critical' || v.impact === 'serious',
      );

      expect(
        violations,
        `${pagePath} has ${violations.length} serious/critical a11y violations:\n` +
          violations
            .map((v) => `  [${v.impact}] ${v.id}: ${v.description} (${v.nodes.length} occurrences)`)
            .join('\n'),
      ).toHaveLength(0);
    });
  }
});
