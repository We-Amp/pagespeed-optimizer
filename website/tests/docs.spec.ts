// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

const docSlugs = [
  'getting-started',
  'installation-docker',
  'installation-module',
  'migrating-to-2-1',
  'configuration',
  'deployment',
  'api-reference',
  'troubleshooting',
  'aspnet-getting-started',
  'aspnet-configuration',
  'browser-analysis',
  'cache-control',
  'helm-deployment',
  'http-api',
  'license',
  'workbench',
];

test.describe('Docs', () => {
  test('docs index renders', async ({ page }) => {
    await page.goto('/docs/');
    await expect(page).toHaveTitle(/Doc/i);
    await expect(page.locator('main h1').first()).toBeVisible();
  });

  test('docs index links to all doc pages', async ({ page }) => {
    await page.goto('/docs/');
    for (const slug of docSlugs) {
      await expect(page.locator(`a[href="/docs/${slug}/"]`).first()).toBeAttached();
    }
  });

  for (const slug of docSlugs) {
    test(`doc page "${slug}" renders`, async ({ page }) => {
      await page.goto(`/docs/${slug}/`);
      const h1 = page.locator('h1').first();
      await expect(h1).toBeVisible();
      const title = await h1.textContent();
      expect(title!.trim().length).toBeGreaterThan(0);
    });
  }

  test('doc page has sidebar navigation', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    // Desktop sidebar or mobile dropdown should exist
    const sidebar = page.locator('nav[aria-label="Documentation"]');
    const mobileNav = page.locator('#docs-nav');
    const hasSidebar = (await sidebar.count()) > 0;
    const hasMobileNav = (await mobileNav.count()) > 0;
    expect(hasSidebar || hasMobileNav).toBeTruthy();
  });

  test('doc pages have correct titles', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page).toHaveTitle(/Getting Started/i);
  });

  // v2.0.14 trial-experience fix (docs/fix-trial-quickstart-and-faq):
  // the ASP.NET getting-started page must keep documenting the worker
  // SocketPath default and the content-negotiation (Vary) model, so the
  // "Dashboard shows zeros" misconfiguration stays explained.
  test('aspnet getting-started covers SocketPath and content negotiation', async ({ page }) => {
    await page.goto('/docs/aspnet-getting-started/');
    const body = page.locator('main');
    await expect(body).toContainText('SocketPath');
    await expect(body).toContainText('Vary');
    // The "zeros" troubleshooting heading must be present and linkable.
    await expect(
      page.locator('#my-dashboard-shows-zeros-and-nothing-is-moving'),
    ).toBeAttached();
  });
});
