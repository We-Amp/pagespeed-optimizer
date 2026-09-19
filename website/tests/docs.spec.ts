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
  // Converged from the legacy docs-1.1 collection into the main docs tree.
  'css-filters',
  'html-filters',
  'image-filters',
  'javascript-filters',
  'filters-overview',
  'filter-selection',
  'filter-reference',
  'iis-configuration',
  'https-configuration',
  'domain-configuration',
  'admin-console',
  'security',
  'directive-index',
];

// Platform-tabbed pages among the converged set (see docSlugs above) — pages
// whose content has nginx/Apache/IIS blocks and so render the platform
// selector.
const platformTabbedSlugs = [
  'filter-selection',
  'https-configuration',
  'domain-configuration',
  'admin-console',
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

  // Post-convergence: the docs tree is single-line, so neither the desktop
  // sidebar nor the mobile selector renders a "2.0 | 1.1" version toggle any
  // more, and there is exactly one doc-tree sidebar on the page (not one per
  // version).
  test('docs page renders no version toggle, desktop or mobile', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page.locator('[role="group"][aria-label="Documentation version"]')).toHaveCount(0);
    await expect(page.locator('nav[aria-label="Documentation"]')).toHaveCount(1);
  });

  // Same toggle removal on the /1.1/docs/ side — its own version toggle is
  // gone too, while the unrelated platform tabs (nginx/apache/iis) stay
  // intact.
  test('1.15 doc page renders no version toggle and keeps its platform tabs', async ({ page }) => {
    await page.goto('/1.1/docs/getting-started/');
    await expect(page.locator('[role="group"][aria-label="Documentation version"]')).toHaveCount(0);
    await expect(page.locator('nav[aria-label="Documentation"]')).toHaveCount(1);
    await expect(page.locator('[data-platform-tab]').first()).toBeAttached();
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
    await expect(page.locator('#my-dashboard-shows-zeros-and-nothing-is-moving')).toBeAttached();
  });

  // The pages converged from the legacy docs-1.1 collection: each one must
  // show up in the main docs sidebar navigation, not just the hub index.
  test('converged pages appear in the docs sidebar navigation', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    const sidebar = page.locator('nav[aria-label="Documentation"]');
    for (const slug of ['css-filters', 'filter-reference', 'admin-console', 'iis-configuration']) {
      await expect(sidebar.locator(`a[href="/docs/${slug}/"]`)).toBeAttached();
    }
  });

  // Platform-tabbed pages converged into the main docs tree must get the same
  // working nginx/Apache/IIS switcher the legacy /1.1/docs/ route has, with
  // the choice persisted across page loads via the shared docs-platform key.
  for (const slug of platformTabbedSlugs) {
    test(`platform tabs on /docs/${slug}/ switch platform and persist the choice`, async ({
      page,
    }) => {
      await page.goto(`/docs/${slug}/`);
      const apacheTab = page.locator('[data-platform-tab="apache"]');
      await expect(apacheTab).toBeAttached();
      await expect(apacheTab).toHaveAttribute('aria-pressed', 'false');

      await apacheTab.click();
      await expect(apacheTab).toHaveAttribute('aria-pressed', 'true');
      await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'apache');

      // Reload: the choice must persist via the docs-platform localStorage key.
      await page.reload();
      await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'apache');
      await expect(page.locator('[data-platform-tab="apache"]')).toHaveAttribute(
        'aria-pressed',
        'true',
      );
    });
  }

  // Non-platform-tabbed converged pages must render exactly as before: no
  // platform selector at all.
  test('non-platform-tabbed converged pages render no platform selector', async ({ page }) => {
    await page.goto('/docs/css-filters/');
    await expect(page.locator('#platform-selector')).toHaveCount(0);
  });

  // Storage-blocked visitors (Safari private mode, strict cookie/storage
  // settings): every localStorage access for the docs-platform key must be
  // wrapped, in both the sitewide pre-paint script and the docs page script,
  // so a throwing storage backend never leaves data-active-platform unset
  // (which would hide every platform-specific content block, per
  // global.css's default-to-hidden rule) and never leaves the tabs inert.
  test('platform selector still works when storage access throws', async ({ page }) => {
    const pageErrors: Error[] = [];
    page.on('pageerror', (err) => pageErrors.push(err));

    // The dev-only Astro toolbar (astro/dist/runtime/client/dev-toolbar) has
    // its own unwrapped localStorage read for its settings, unrelated to this
    // site's code and absent from a production build — block it so this test
    // isolates our own scripts' behavior instead of a dev-tooling artifact.
    await page.route('**/dev-toolbar/**', (route) => route.abort());

    // Scoped to `localStorage` specifically (not Storage.prototype, which
    // sessionStorage also inherits from and an unrelated component on this
    // page uses) — this mirrors real Safari ITP / strict-settings behavior,
    // where merely reading the `localStorage` property throws.
    await page.addInitScript(() => {
      Object.defineProperty(window, 'localStorage', {
        configurable: true,
        get() {
          throw new DOMException('Storage access is blocked.', 'SecurityError');
        },
      });
    });

    await page.goto('/docs/filter-selection/');

    // Default platform content is visible even though storage never
    // resolved — data-active-platform must fall back to a deterministic
    // default rather than staying unset.
    await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'nginx');
    await expect(page.locator('[data-platform-tab="nginx"]')).toHaveAttribute(
      'aria-pressed',
      'true',
    );
    await expect(page.locator('[data-platform="nginx"]').first()).toBeVisible();

    // Clicking another tab still switches the visible content — the click
    // listeners must have attached despite storage throwing during init.
    const apacheTab = page.locator('[data-platform-tab="apache"]');
    await apacheTab.click();
    await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'apache');
    await expect(apacheTab).toHaveAttribute('aria-pressed', 'true');
    await expect(page.locator('[data-platform="apache"]').first()).toBeVisible();

    expect(pageErrors).toEqual([]);
  });
});
