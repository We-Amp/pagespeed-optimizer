// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Navigation', () => {
  test('header logo links to home page', async ({ page }) => {
    await page.goto('/pricing/');
    await page.locator('header a[href="/"]').click();
    await expect(page).toHaveURL('/');
  });

  test('header nav: Blog and Contact are direct links; Docs is a dropdown', async ({ page }) => {
    const directLinks = [
      { label: 'Blog', href: '/blog/' },
      { label: 'Contact', href: '/contact/' },
    ];
    for (const link of directLinks) {
      await page.goto('/');
      await page.locator(`header nav .lg\\:flex > a:has-text("${link.label}")`).first().click();
      await expect(page).toHaveURL(link.href);
    }
    await page.goto('/');
    const groups = await page.locator('header nav button.nav-dropdown-btn').allInnerTexts();
    expect(groups.map((g) => g.trim())).toEqual(['Docs', 'Tools', 'Support', 'Compare']);
    // Rule packs left the top level; it lives in the Docs panel.
    await expect(page.locator('header nav .lg\\:flex > a:has-text("Rule packs")')).toHaveCount(0);
  });

  test('Docs dropdown: Docs home, four columns, Rule packs reachable', async ({ page }) => {
    await page.goto('/');
    await page.locator('header nav button.nav-dropdown-btn:has-text("Docs")').click();
    const panel = page.locator('.nav-dropdown-panel:visible');
    const home = panel.locator('a[href="/docs/"]');
    await expect(home).toContainText('Docs home');
    await expect(home).toHaveAttribute('data-umami-event', 'nav-docs');
    for (const title of [
      'Get started',
      'Configure',
      'Rule & transform packs',
      'Operate & reference',
    ]) {
      await expect(panel.getByText(title, { exact: true })).toBeVisible();
    }
    const packs = panel.locator('a[href="/packs/"]');
    await expect(packs).toContainText('Browse rule packs');
    await expect(packs).toHaveAttribute('data-umami-event', 'nav-packs');
    const install = panel.locator('a[href="/docs/installation-docker/"]');
    await expect(install).toHaveAttribute('data-umami-event', 'nav-docs-menu');
    await expect(install).toHaveAttribute('data-umami-event-position', 'getting_started');
    await packs.click();
    await expect(page).toHaveURL('/packs/');
  });

  test('Docs dropdown: active state and aria-current', async ({ page }) => {
    await page.goto('/docs/cache-modes/');
    const btn = page.locator('header nav button.nav-dropdown-btn:has-text("Docs")');
    await expect(btn).toHaveClass(/nav-item-active/);
    await btn.click();
    const panel = page.locator('.nav-dropdown-panel:visible');
    await expect(panel.locator('a[href="/docs/cache-modes/"]')).toHaveAttribute(
      'aria-current',
      'page',
    );
    await expect(panel.locator('a[href="/docs/"]')).not.toHaveAttribute('aria-current', 'page');
    await page.goto('/packs/');
    await expect(btn).toHaveClass(/nav-item-active/);
  });

  test('Docs dropdown is keyboard operable', async ({ page }) => {
    await page.goto('/');
    const btn = page.locator('header nav button.nav-dropdown-btn:has-text("Docs")');
    await btn.focus();
    await page.keyboard.press('ArrowDown');
    const panel = page.locator('.nav-dropdown-panel:visible');
    await expect(panel).toBeVisible();
    await expect(panel.locator('a').first()).toBeFocused();
    await page.keyboard.press('Escape');
    await expect(panel).toBeHidden();
    await expect(btn).toBeFocused();
    await expect(btn).toHaveAttribute('aria-expanded', 'false');
    // Enter and Space on an open button toggle it closed.
    await page.keyboard.press('Enter');
    await expect(panel).toBeVisible();
    await btn.focus();
    await page.keyboard.press('Enter');
    await expect(panel).toBeHidden();
    await expect(btn).toHaveAttribute('aria-expanded', 'false');
  });

  test('Docs panel fits a short 1024x700 viewport and scrolls inside itself', async ({ page }) => {
    await page.setViewportSize({ width: 1024, height: 700 });
    await page.goto('/');
    await page.locator('header nav button.nav-dropdown-btn:has-text("Docs")').click();
    const box = await page.locator('.nav-dropdown-panel:visible .nav-panel-box').boundingBox();
    expect(box!.y + box!.height).toBeLessThanOrEqual(700);
  });

  test('Docs menu columns hold at most eight links', async ({ page }) => {
    await page.goto('/');
    await page.locator('header nav button.nav-dropdown-btn:has-text("Docs")').click();
    const lists = page.locator('.nav-dropdown-panel:visible ul');
    expect(await lists.count()).toBe(4);
    for (let i = 0; i < 4; i++) {
      expect(await lists.nth(i).locator('a').count()).toBeLessThanOrEqual(8);
    }
  });

  test('mobile Docs menu opens the section holding the current page', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/docs/cache-modes/');
    await page.locator('#mobile-menu-btn').click();
    const open = page.locator('#mobile-menu details[open]');
    await expect(open).toHaveCount(1);
    await expect(open.locator('summary')).toContainText('Configure');
  });

  for (const width of [1024, 1280, 1440]) {
    test(`Docs panel stays inside the viewport at ${width}`, async ({ page }) => {
      await page.setViewportSize({ width, height: 800 });
      await page.goto('/');
      await page.locator('header nav button.nav-dropdown-btn:has-text("Docs")').click();
      const box = await page.locator('.nav-dropdown-panel:visible .nav-panel-box').boundingBox();
      expect(box).not.toBeNull();
      expect(box!.x).toBeGreaterThanOrEqual(0);
      expect(box!.x + box!.width).toBeLessThanOrEqual(width);
      expect(box!.y + box!.height).toBeLessThanOrEqual(800);
      const overflow = await page.evaluate(
        () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
      );
      expect(overflow).toBeLessThanOrEqual(0);
    });
  }

  test('an open mobile menu closes when the viewport grows to desktop width', async ({ page }) => {
    await page.setViewportSize({ width: 800, height: 700 });
    await page.goto('/');
    const btn = page.locator('#mobile-menu-btn');
    await btn.click();
    await expect(btn).toHaveAttribute('aria-expanded', 'true');
    await page.setViewportSize({ width: 1100, height: 700 });
    await expect(btn).toHaveAttribute('aria-expanded', 'false');
    await expect(page.locator('#mobile-menu')).toHaveClass(/hidden/);
    await page.setViewportSize({ width: 800, height: 700 });
    await expect(page.locator('#mobile-menu')).toBeHidden();
  });

  test('the sticky mobile CTA appears after scrolling', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const cta = page.locator('#sticky-cta');
    await expect(cta).toHaveAttribute('data-sticky-cta', 'hidden');
    await page.evaluate(() => window.scrollTo(0, window.innerHeight * 1.5));
    await expect(cta).toHaveAttribute('data-sticky-cta', 'visible');
    await expect(cta).toBeVisible();
    // At the bottom the footer is in view and the CTA steps aside.
    await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
    await expect(cta).toHaveAttribute('data-sticky-cta', 'hidden');
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
      { href: '/support/#hardened-builds', label: 'Signed artifacts', offer: 'hardened' },
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
    const desktopNav = page.locator('header nav .lg\\:flex > a[href="/blog/"]').first();
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
      'https://ngxpagespeed.com/',
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
    await expect(footer.locator('a[href="/docs/upgrade/"]')).toHaveText('Upgrade');
    await expect(footer.locator('a[href="/docs/migrating-to-2-1/"]')).toHaveCount(0);
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

    // Docs: a plain home link, then one <details> per column.
    await expect(mobileMenu.locator('a[href="/docs/"]')).toContainText('Docs home');
    const packsSection = mobileMenu.locator('details', { hasText: 'Rule & transform packs' });
    await packsSection.locator('summary').click();
    await expect(packsSection.locator('a[href="/packs/"]')).toContainText('Browse rule packs');

    await mobileMenu.locator('a[href="/docs/"]').click();
    await expect(page).toHaveURL('/docs/');
  });

  test('the open mobile menu is modal: page behind is inert, scroll is locked, Tab stays inside', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const btn = page.locator('#mobile-menu-btn');
    await btn.click();
    await expect(page.locator('#mobile-menu')).toBeVisible();

    await expect(page.locator('main')).toHaveAttribute('inert', '');
    await expect(page.locator('footer[data-site-footer]')).toHaveAttribute('inert', '');
    expect(await page.evaluate(() => getComputedStyle(document.documentElement).overflow)).toBe(
      'hidden',
    );

    // Tab forwards and backwards: focus never leaves the header.
    for (const key of [...Array<string>(30).fill('Tab'), ...Array<string>(30).fill('Shift+Tab')]) {
      await page.keyboard.press(key);
      const inHeader = await page.evaluate(
        () => !!document.activeElement && !!document.activeElement.closest('header'),
      );
      expect(inHeader, `focus left the header after ${key}`).toBe(true);
    }

    await page.keyboard.press('Escape');
    await expect(page.locator('main')).not.toHaveAttribute('inert', '');
    await expect(page.locator('footer[data-site-footer]')).not.toHaveAttribute('inert', '');
    expect(await page.evaluate(() => getComputedStyle(document.documentElement).overflow)).not.toBe(
      'hidden',
    );
    await expect(btn).toBeFocused();
  });

  test('the sticky CTA is out of the tab order and the accessibility tree while hidden', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const cta = page.locator('#sticky-cta');
    await expect(cta).toHaveAttribute('data-sticky-cta', 'hidden');
    await expect(cta).toHaveAttribute('inert', '');
    expect(await cta.evaluate((e) => getComputedStyle(e).visibility)).toBe('hidden');
    await expect(page.locator('nav[aria-label="Quick actions"] #sticky-cta')).toHaveCount(1);
    await page.evaluate(() => window.scrollTo(0, window.innerHeight * 1.5));
    await expect(cta).toHaveAttribute('data-sticky-cta', 'visible');
    await expect(cta).not.toHaveAttribute('inert', '');
    await expect(cta).toBeVisible();
  });

  test('the header search button advertises its shortcuts', async ({ page }) => {
    await page.setViewportSize({ width: 1100, height: 700 });
    await page.goto('/');
    const search = page.locator('header [data-search-open][title]');
    await expect(search).toHaveAttribute('title', 'Search (/)');
    await expect(search).toHaveAttribute('aria-keyshortcuts', '/ Control+K Meta+K');
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
