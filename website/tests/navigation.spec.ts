// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Navigation', () => {
  test('header logo links to home page', async ({ page }) => {
    await page.goto('/pricing/');
    await page.locator('header a[href="/"]').click();
    await expect(page).toHaveURL('/');
  });

  test('header nav links navigate correctly', async ({ page }) => {
    // Current nav structure: Pricing and Blog are direct top-level <a>;
    // Products, Tools, Docs and Compare are click-to-open dropdowns.
    const directLinks = [
      { label: 'Pricing', href: '/pricing/' },
      { label: 'Blog', href: '/blog/' },
    ];

    for (const link of directLinks) {
      await page.goto('/');
      await page.locator(`header nav >> a:has-text("${link.label}")`).first().click();
      await expect(page).toHaveURL(link.href);
    }

    // Docs dropdown — open (click the labelled disclosure button) and click each
    // child link. Dropdowns are toggled by `.nav-dropdown-btn` and reveal a
    // `.nav-dropdown-panel`.
    await page.goto('/');
    await page.locator('header nav button.nav-dropdown-btn:has-text("Docs")').click();
    await page.locator('.nav-dropdown-panel a[href="/docs/"]').first().click();
    await expect(page).toHaveURL('/docs/');

    // /1.1/docs/ left the Docs dropdown with the convergence and is no
    // longer advertised from the footer either; the route itself is now
    // retired too (see docs.spec.ts's 404 check).
  });

  test('header download CTA links to /download/', async ({ page }) => {
    // Post-audit: the header CTA is a download-first funnel, not "Start Free Trial".
    await page.goto('/');
    const cta = page.locator('header a[data-umami-event="cta_nav_download"]');
    await expect(cta.first()).toHaveAttribute('href', '/download/');
  });

  test('active nav link is highlighted for current page', async ({ page }) => {
    await page.goto('/pricing/');
    // The desktop nav "Pricing" text link (not the CTA button). The CALIBER
    // redesign marks the active link with the teal accent token
    // (`text-interactive`) plus `aria-current="page"`.
    const desktopNav = page.locator('header nav >> div >> a[href="/pricing/"]').first();
    await expect(desktopNav).toHaveClass(/text-interactive/);
    await expect(desktopNav).toHaveAttribute('aria-current', 'page');
  });

  test('footer Product links are present', async ({ page }) => {
    await page.goto('/');
    const footer = page.locator('footer');
    await expect(footer.locator('a[href="/features/"]')).toBeVisible();
    await expect(footer.locator('a[href="/pricing/"]')).toBeVisible();
    await expect(footer.locator('a[href="/demo/"]')).toBeVisible();
    await expect(footer.locator('a[href="/calculator/"]')).toBeVisible();
  });

  test('footer Product group offers a single upgrade entry, not a previous-versions group', async ({
    page,
  }) => {
    // Post-convergence: the old "Previous versions" group (1.15, upgrading-from-1.15,
    // migrating-from-2.0) collapsed to one upgrade link in the Product group;
    // /1.1/ and /1.1/docs/upgrading-to-2-1/ are no longer advertised from the footer.
    await page.goto('/');
    const footer = page.locator('footer');
    await expect(footer.locator('a[href="/docs/migrating-to-2-1/"]')).toHaveText('Upgrade to 2.1');
    await expect(footer.getByText('Previous versions')).toHaveCount(0);
    await expect(footer.locator('a[href="/1.1/"]')).toHaveCount(0);
    await expect(footer.locator('a[href="/1.1/docs/upgrading-to-2-1/"]')).toHaveCount(0);
  });

  test('footer Resources links are present', async ({ page }) => {
    // Post-convergence: Resources contains Docs and ASP.NET Middleware; the
    // "Docs (1.15)" link to /1.1/docs/ is no longer advertised from the footer.
    // Blog moved to Company; /1.0/ is no longer linked from the footer (legacy-only).
    await page.goto('/');
    const footer = page.locator('footer');
    await expect(footer.locator('a[href="/docs/"]')).toBeVisible();
    await expect(footer.locator('a[href="/docs/aspnet-getting-started/"]')).toBeVisible();
    await expect(footer.locator('a[href="/1.1/docs/"]')).toHaveCount(0);
  });

  test('footer Company links are present', async ({ page }) => {
    await page.goto('/');
    const footer = page.locator('footer');
    await expect(footer.locator('a[href="/license/"]')).toBeVisible();
    await expect(footer.locator('a[href="/terms/"]')).toBeVisible();
    await expect(footer.locator('a[href="/security/"]')).toBeVisible();
    await expect(footer.locator('a[href="/contact/"]')).toBeVisible();
  });

  test('footer contains copyright notice with current year', async ({ page }) => {
    await page.goto('/');
    const year = new Date().getFullYear().toString();
    await expect(page.locator('footer')).toContainText(year);
    await expect(page.locator('footer')).toContainText('We-Amp B.V.');
  });

  test('skip-to-content link targets main content', async ({ page }) => {
    await page.goto('/');
    const skipLink = page.locator('a[href="#main-content"]');
    await expect(skipLink).toBeAttached();
    const mainContent = page.locator('#main-content');
    await expect(mainContent).toBeAttached();
  });

  test('mobile menu is hidden on desktop', async ({ page }) => {
    await page.goto('/');
    const mobileMenu = page.locator('#mobile-menu');
    await expect(mobileMenu).toBeHidden();
  });

  test('mobile menu toggles on hamburger click', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const btn = page.locator('#mobile-menu-btn');
    const menu = page.locator('#mobile-menu');

    await expect(menu).toBeHidden();
    await expect(btn).toHaveAttribute('aria-expanded', 'false');

    await btn.click();
    await expect(menu).toBeVisible();
    await expect(btn).toHaveAttribute('aria-expanded', 'true');

    await btn.click();
    await expect(menu).toBeHidden();
    await expect(btn).toHaveAttribute('aria-expanded', 'false');
  });

  test('mobile menu links navigate correctly', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    await page.locator('#mobile-menu-btn').click();

    const mobileMenu = page.locator('#mobile-menu');
    await expect(mobileMenu).toBeVisible();

    const featureLink = mobileMenu.locator('a[href="/features/"]');
    await featureLink.click();
    await expect(page).toHaveURL('/features/');
  });

  test('mobile menu icon toggles between open and close', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const openIcon = page.locator('#menu-icon-open');
    const closeIcon = page.locator('#menu-icon-close');

    await expect(openIcon).toBeVisible();
    await expect(closeIcon).toBeHidden();

    await page.locator('#mobile-menu-btn').click();

    await expect(openIcon).toBeHidden();
    await expect(closeIcon).toBeVisible();
  });
});
