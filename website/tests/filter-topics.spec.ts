// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The per-filter topic pages (/docs/filters/<name>/): three representative
// pages render (one with a live example, one compound name, one noindex), the
// group pages keep their anchors and link each section to its topic page, and
// the /docs/filters/ table links each row to one.

import { test, expect } from '@playwright/test';

test.describe('Filter topic pages', () => {
  test('a filter with a live example renders its guide', async ({ page }) => {
    await page.goto('/docs/filters/lazyload_images/');
    await expect(page.locator('main h1')).toHaveText('Lazy-load images (lazyload_images)');
    await expect(page.locator('main h1')).toHaveCount(1);
    await expect(page.locator('[data-topic-lede]')).toContainText('below the fold');
    await expect(page.locator('#when-to-use')).toBeVisible();
    await expect(page.locator('#risks')).toBeVisible();
    await expect(page.locator('#configuration')).toBeVisible();
    await expect(page.locator('main')).toContainText('ModPagespeedEnableFilters lazyload_images');
    await expect(page.locator('main')).toContainText('pagespeed EnableFilters lazyload_images;');
    await expect(page.locator('[data-topic-worker] a').first()).toHaveAttribute(
      'href',
      '/docs/worker-configuration/#lazy-load-images',
    );
    await expect(
      page.locator('[data-topic-example] a[href="/examples/lazyload_images/"]'),
    ).toBeVisible();
    await expect(page.locator('[data-topic-faq] dt')).toHaveCount(3);
    await expect(page.locator('meta[name="robots"]')).toHaveCount(0);
  });

  test('a compound name lists the filters it switches on', async ({ page }) => {
    await page.goto('/docs/filters/rewrite_images/');
    await expect(page.locator('main h1')).toHaveText('Optimize images (rewrite_images)');
    const members = page.locator('[data-alias-members] a');
    await expect(members).toHaveCount(13);
    await expect(
      page.locator('[data-alias-members] a[href="/docs/filters/convert_jpeg_to_webp/"]'),
    ).toBeVisible();
    await expect(
      page.locator('[data-topic-example] a[href="/examples/rewrite_images/"]'),
    ).toBeVisible();
  });

  test('a thin page renders and is noindex', async ({ page }) => {
    await page.goto('/docs/filters/split_html/');
    await expect(page.locator('main h1')).toHaveText('Split HTML (split_html)');
    await expect(page.locator('meta[name="robots"]')).toHaveAttribute('content', 'noindex,follow');
    await expect(page.locator('[data-topic-faq] dt').first()).toBeVisible();
  });

  test('group pages keep their anchors and link each section to its topic page', async ({
    page,
  }) => {
    await page.goto('/docs/javascript-filters/#inline_javascript');
    await expect(page.locator('[id="inline_javascript"]')).toHaveCount(1);
    await expect(page.locator('a[href="/docs/filters/inline_javascript/"]')).toHaveText(
      'Full guide →',
    );

    await page.goto('/docs/image-filters/#convert_jpeg_to_webp');
    await expect(page.locator('[id="convert_jpeg_to_webp"]')).toHaveCount(1);
    await expect(page.locator('a[href="/docs/filters/convert_jpeg_to_webp/"]')).toBeVisible();
  });

  test('the filters table links rows to topic pages', async ({ page }) => {
    await page.goto('/docs/filters/');
    const link = page.locator('tr#inline_javascript a[data-topic-link]');
    await expect(link).toHaveAttribute('href', '/docs/filters/inline_javascript/');
    await link.click();
    await expect(page).toHaveURL(/\/docs\/filters\/inline_javascript\/$/);
    await expect(page.locator('main h1')).toHaveText('Inline JavaScript (inline_javascript)');
  });

  test('an example page links to its full guide', async ({ page }) => {
    await page.goto('/examples/combine_css/');
    await expect(page.locator('a[data-full-guide]')).toHaveAttribute(
      'href',
      '/docs/filters/combine_css/',
    );
  });
});
