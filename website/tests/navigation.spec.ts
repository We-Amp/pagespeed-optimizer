// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Navigation', () => {
  test('header logo links to home page', async ({ page }) => {
    await page.goto('/pricing/');
    await page.locator('header a[href="/"]').click();
    await expect(page).toHaveURL('/');
  });

  test('header nav: Docs, Blog and Contact are direct links', async ({ page }) => {
    // Target IA: Docs is a direct link (no dropdown); Tools, Support and
    // Compare are click-to-open dropdowns; Blog and Contact are flat.
    const directLinks = [
      { label: 'Docs', href: '/docs/' },
      { label: 'Blog', href: '/blog/' },
      { label: 'Contact', href: '/contact/' },
    ];
    for (const link of directLinks) {
      await page.goto('/');
      await page
        .locator(`header nav .md\\:flex > a:has-text("${link.label}")`)
        .first()
        .click();
      await expect(page).toHaveURL(link.href);
    }
    await page.goto('/');
    await expect(page.locator('header nav button.nav-dropdown-btn:has-text("Docs")')).toHaveCount(0);
    const groups = await page.locator('header nav button.nav-dropdown-btn').allInnerTexts();
    expect(groups.map((g) => g.trim())).toEqual(['Tools', 'Support', 'Compare']);
  });

  test('Tools dropdown names the two lead tools plainly', async ({ page }) => {
    await page.goto('/');
    await page.locator('header nav button.nav-dropdown-btn:has-text("Tools")').click();
    const panel = page.locator('.nav-dropdown-panel:visible');
    await expect(panel.locator('a[href="/ai-readability/"]')).toContainText('AI readability check');
    await expect(panel.locator('a[href="/analyze/"]')).toContainText('PageSpeed analyzer');
    // Existing event names survive the relabel.
    await expect(panel.locator('a[href="/ai-readability/"]')).toHaveAttribute(
      'data-umami-event',
      'nav-ai-readability',
    );
    await expect(panel.locator('a[href="/analyze/"]')).toHaveAttribute(
      'data-umami-event',
      'nav-analyze',
    );
    for (const href of ['/demo/', '/examples/', '/calculator/']) {
      await expect(panel.locator(`a[href="${href}"]`)).toBeVisible();
    }
  });

  test('Tools dropdown lists the live admin console between examples and calculator', async ({
    page,
  }) => {
    await page.goto('/');
    await page.locator('header nav button.nav-dropdown-btn:has-text("Tools")').click();
    const panel = page.locator('.nav-dropdown-panel:visible');
    const labels = (await panel.locator('a span.text-sm').allInnerTexts()).map((t) => t.trim());
    expect(labels).toEqual([
      'AI readability check',
      'PageSpeed analyzer',
      'Filter examples',
      'Live admin console',
      'Savings calculator',
      'Demo',
    ]);
    const live = panel.locator('a[href="https://we-amp.com/pagespeed_global_admin/"]');
    await expect(live).toContainText('The real console on our own sites');
    await expect(live).toHaveAttribute('data-umami-event', 'nav-live-console');
    // Our own property: same tab.
    await expect(live).not.toHaveAttribute('target', /.+/);
  });

  test('footer Tools lists the live admin console', async ({ page }) => {
    await page.goto('/');
    const footerLinks = (await page.locator('footer a').allInnerTexts()).map((t) => t.trim());
    const i = footerLinks.indexOf('Live admin console');
    expect(i).toBeGreaterThan(footerLinks.indexOf('Filter examples'));
    expect(i).toBeLessThan(footerLinks.indexOf('Savings calculator'));
    await expect(
      page.locator('footer a[href="https://we-amp.com/pagespeed_global_admin/"]'),
    ).toHaveCount(1);
  });

  test('Support dropdown carries the four offers as commercial CTAs', async ({ page }) => {
    await page.goto('/');
    await page.locator('header nav button.nav-dropdown-btn:has-text("Support")').click();
    const panel = page.locator('.nav-dropdown-panel:visible');
    const expected = [
      { href: '/support/', label: 'Support plans', offer: 'support' },
      { href: '/support/#hardened-builds', label: 'Hardened builds', offer: 'hardened' },
      { href: '/hosting-partners/', label: 'Hosting partners', offer: 'hosting' },
      { href: 'https://we-amp.com/consulting/', label: 'Consulting', offer: 'consulting' },
    ];
    await expect(panel.locator('a')).toHaveCount(expected.length);
    for (const item of expected) {
      const a = panel.locator(`a[href="${item.href}"]`);
      await expect(a).toContainText(item.label);
      await expect(a).toHaveAttribute('data-umami-event', 'cta_commercial');
      await expect(a).toHaveAttribute('data-umami-event-offer', item.offer);
      await expect(a).toHaveAttribute('data-umami-event-surface', 'nav');
    }
    await panel.locator('a[href="/support/"]').click();
    await expect(page).toHaveURL('/support/');
  });

  test('header has no separate Message entry; the floating launcher stays', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('header [data-qm-open]')).toHaveCount(0);
    await expect(page.locator('header').getByText('Message', { exact: true })).toHaveCount(0);
    await expect(page.locator('[data-qm-launcher]')).toBeVisible();
  });

  test('header download CTA links to /download/', async ({ page }) => {
    // Post-audit: the header CTA is a download-first funnel, not "Start Free Trial".
    await page.goto('/');
    const cta = page.locator('header a[data-umami-event="cta_nav_download"]');
    await expect(cta.first()).toHaveAttribute('href', '/download/');
  });

  test('active nav link is highlighted for current page', async ({ page }) => {
    await page.goto('/blog/');
    const desktopNav = page.locator('header nav .md\\:flex > a[href="/blog/"]').first();
    await expect(desktopNav).toHaveClass(/text-interactive/);
    await expect(desktopNav).toHaveAttribute('aria-current', 'page');
  });

  test('footer has four groups plus Legal', async ({ page }) => {
    await page.goto('/');
    // Group headings are the labels directly above each link list.
    const headings = await page.locator('footer p.uppercase:has(+ ul)').allInnerTexts();
    expect(headings.map((h) => h.trim().toLowerCase())).toEqual([
      'product',
      'install and docs',
      'tools',
      'support and company',
      'legal',
    ]);
  });

  test('footer carries the support and company links', async ({ page }) => {
    await page.goto('/');
    const footer = page.locator('footer');
    for (const href of [
      '/support/',
      '/pricing/',
      '/hosting-partners/',
      'https://we-amp.com/consulting/',
      '/contact/',
      '/security/',
      '/docs/release-notes/',
      '/about/',
    ]) {
      await expect(footer.locator(`a[href="${href}"]`).first(), href).toBeVisible();
    }
    await expect(footer.locator('a[href="/support/"]')).toHaveAttribute(
      'data-umami-event',
      'cta_commercial',
    );
    await expect(footer.locator('a[href="/support/"]')).toHaveAttribute(
      'data-umami-event-surface',
      'footer',
    );
  });

  test('footer Product, docs and tools links are present', async ({ page }) => {
    await page.goto('/');
    const footer = page.locator('footer');
    for (const href of [
      '/features/',
      '/download/',
      '/docs/',
      '/docs/getting-started/',
      '/docs/aspnet-getting-started/',
      '/demo/',
      '/calculator/',
      '/license/',
      '/terms/',
    ]) {
      await expect(footer.locator(`a[href="${href}"]`).first(), href).toBeVisible();
    }
    // Existing event names survive the regroup.
    await expect(footer.locator('a[href="/download/"]')).toHaveAttribute(
      'data-umami-event',
      'cta_footer_download',
    );
    await expect(footer.locator('a[href="/analyze/"]')).toHaveAttribute(
      'data-umami-event',
      'nav-analyze-footer',
    );
    await expect(footer.locator('a[href="/docs/migrating-to-2-1/"]')).toHaveText('Upgrade to 2.1');
    await expect(footer.locator('a[href="/1.1/"]')).toHaveCount(0);
    await expect(footer.locator('a[href="/1.1/docs/"]')).toHaveCount(0);
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

    // Same structure as the desktop nav: Docs direct, the Support group with
    // its commercial events (position tagged for mobile), Contact.
    const support = mobileMenu.locator('a[href="/support/"]');
    await expect(support).toHaveAttribute('data-umami-event', 'cta_commercial');
    await expect(support).toHaveAttribute('data-umami-event-position', 'support_plans_mobile');
    for (const href of [
      '/support/#hardened-builds',
      '/hosting-partners/',
      'https://we-amp.com/consulting/',
      '/ai-readability/',
      '/analyze/',
    ]) {
      await expect(mobileMenu.locator(`a[href="${href}"]`), href).toBeVisible();
    }
    await expect(mobileMenu.locator('[data-qm-open]')).toHaveCount(0);

    await mobileMenu.locator('a[href="/docs/"]').click();
    await expect(page).toHaveURL('/docs/');
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

test.describe('Site search', () => {
  test('header search button opens the dialog; Escape closes it and returns focus', async ({
    page,
  }) => {
    await page.goto('/');
    const button = page.locator('header button[data-search-open]:has-text("Search")');
    await expect(button).toBeVisible();
    await expect(button).toHaveAttribute('aria-haspopup', 'dialog');
    const dialog = page.locator('dialog#site-search');
    await expect(dialog).toBeHidden();

    await button.click();
    await expect(dialog).toBeVisible();
    await expect(dialog).toHaveAttribute('open', '');
    // The dev server has no Pagefind index; the dialog must say so rather than
    // fail silently (the production build writes /pagefind/ at build time).
    await expect(dialog.locator('#site-search-status')).toContainText(/search/i);

    await page.keyboard.press('Escape');
    await expect(dialog).toBeHidden();
    await expect(button).toBeFocused();
  });

  test('"/" and Ctrl/Cmd+K open the search dialog', async ({ page }) => {
    await page.goto('/pricing/');
    const dialog = page.locator('dialog#site-search');
    await page.keyboard.press('/');
    await expect(dialog).toBeVisible();
    await page.keyboard.press('Escape');
    await expect(dialog).toBeHidden();

    await page.keyboard.press('Control+k');
    await expect(dialog).toBeVisible();
    await page.keyboard.press('Escape');
    await expect(dialog).toBeHidden();
  });

  test('mobile header has an icon-only search button', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const button = page.locator('header button[data-search-open][aria-label="Search"]');
    await expect(button).toBeVisible();
    await button.click();
    await expect(page.locator('dialog#site-search')).toBeVisible();
  });
});
