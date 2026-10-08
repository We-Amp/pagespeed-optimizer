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
  'filter-selection',
  'iis-configuration',
  'https-configuration',
  'domain-configuration',
  'admin-console',
  'security',
  'worker-configuration',
  // The structure tranche: Start here, Install, Upgrade and migrate, Operate.
  'is-it-working',
  'install-iis',
  'install-envoy',
  'upgrade',
  'migrate-from-1-15',
  'migrate-from-ngx-pagespeed',
  'migrate-from-iispeed',
  'migrate-from-google-mod-pagespeed',
  'monitoring',
  'uninstall',
  'production-deployment',
  'cpanel',
  'release-notes',
];

// The sidebar and hub groups, in the order the audiences read them.
const GROUP_ORDER = [
  'Start here',
  'Install',
  'Upgrade and migrate',
  'Configure',
  'Filters',
  'Operate',
  'Agents',
  'Reference',
  'Release notes by line',
];

const NEW_PAGES = [
  'is-it-working',
  'install-iis',
  'install-envoy',
  'upgrade',
  'migrate-from-1-15',
  'migrate-from-ngx-pagespeed',
  'migrate-from-iispeed',
  'migrate-from-google-mod-pagespeed',
  'monitoring',
  'uninstall',
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
    // Desktop sidebar or the mobile "Docs menu" disclosure should exist
    const sidebar = page.locator('nav[aria-label="Documentation"]');
    const mobileNav = page.locator('#docs-menu');
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

  // The /1.1/docs/ archive route (formerly the sole surviving docs-1.1 page)
  // is retired now that its release notes are folded into the converged
  // /docs/release-notes/ page. Assert the old route is actually gone rather
  // than dropping this test outright — there is no page left to check a
  // version toggle on.
  test('retired /1.1/docs/release-notes/ route is gone', async ({ page }) => {
    const response = await page.goto('/1.1/docs/release-notes/');
    expect(response?.status()).toBe(404);
  });

  test('doc pages have correct titles', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page).toHaveTitle(/Install mod_pagespeed 2\.1/i);
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
    for (const slug of ['css-filters', 'filter-selection', 'admin-console', 'iis-configuration']) {
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

// Docs page mechanics: search entry point, previous/next, "Last updated",
// the edit link, copy buttons on code blocks, the mobile disclosure menu and
// the hub's section headings.
test.describe('Docs mechanics', () => {
  const editUrlPattern =
    /^https:\/\/github\.com\/We-Amp\/pagespeed-optimizer\/edit\/main\/website\/src\/content\/docs\/[a-z0-9-]+\.mdx?$/;

  test('docs hub has a search box that opens the search dialog', async ({ page }) => {
    await page.goto('/docs/');
    const box = page.locator('main button[data-search-open]');
    await expect(box).toBeVisible();
    await expect(box).toContainText('Search the docs');
    await box.click();
    const dialog = page.locator('dialog#site-search');
    await expect(dialog).toBeVisible();
    await expect(dialog).toHaveAttribute('open', '');
    await page.keyboard.press('Escape');
    await expect(dialog).toBeHidden();
  });

  test('docs hub has exactly one "Reference" heading and a popular strip', async ({ page }) => {
    await page.goto('/docs/');
    const reference = page.locator('main h2', { hasText: /^\s*Reference\s*$/ });
    await expect(reference).toHaveCount(1);
    // The filters reference page sits inside that one Reference section.
    await expect(page.locator('main a[href="/docs/filters/"]')).toHaveCount(2); // popular + card
    const popular = page.locator('nav[aria-label="Popular pages"] a');
    await expect(popular).toHaveCount(5);
    await expect(popular.first()).toHaveAttribute('href', '/docs/getting-started/');
  });

  test('release notes have one destination from the docs sidebar', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    const sidebar = page.locator('nav[aria-label="Documentation"]');
    await expect(sidebar.locator('a[href="/docs/release-notes/"]')).toHaveCount(1);
    await expect(sidebar.locator('a[href="/blog/"]')).toHaveCount(0);
  });

  test('a middle doc has previous and next links that follow the sidebar order', async ({
    page,
  }) => {
    await page.goto('/docs/installation-module/');
    const order = await page
      .locator('nav[aria-label="Documentation"] a[href^="/docs/"]')
      .evaluateAll((links) => links.map((a) => a.getAttribute('href')));
    const here = order.indexOf('/docs/installation-module/');
    expect(here).toBeGreaterThan(0);
    expect(here).toBeLessThan(order.length - 1);

    const pager = page.locator('nav[aria-label="Previous and next pages"]');
    await expect(pager).toBeVisible();
    await expect(pager.locator('a[rel="prev"]')).toHaveAttribute('href', order[here - 1]!);
    await expect(pager.locator('a[rel="next"]')).toHaveAttribute('href', order[here + 1]!);
  });

  test('first doc has no previous link; last doc has no next link', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page.locator('a[rel="prev"]')).toHaveCount(0);
    await expect(page.locator('a[rel="next"]')).toHaveCount(1);
  });

  test('"Last updated" is visible and dateModified in the JSON-LD matches it', async ({ page }) => {
    await page.goto('/docs/configuration/');
    const updated = page.locator('main time[datetime]').first();
    await expect(updated).toBeVisible();
    await expect(page.getByText(/Last updated \d{1,2} [A-Z][a-z]+ \d{4}/)).toBeVisible();
    const iso = await updated.getAttribute('datetime');
    expect(iso).toMatch(/^\d{4}-\d{2}-\d{2}$/);
    const jsonLd = await page
      .locator('script[type="application/ld+json"]')
      .evaluateAll((scripts) => scripts.map((s) => JSON.parse(s.textContent || '{}')));
    const article = jsonLd.find((j) => j['@type'] === 'TechArticle');
    expect(article?.dateModified).toBe(iso);
  });

  test('"Edit this page" links to the content file on GitHub', async ({ page }) => {
    await page.goto('/docs/configuration/');
    const edit = page.locator('main a', { hasText: 'Edit this page' });
    await expect(edit).toBeVisible();
    expect(await edit.getAttribute('href')).toMatch(editUrlPattern);
    expect(await edit.getAttribute('href')).toContain('/configuration.md');
  });

  // Every docs page is also served as Markdown at its URL with a .md suffix
  // (src/pages/docs/[slug].md.ts), the body the agent files carry: the title
  // as the one H1, the lede, the canonical URL, absolute links.
  test('/docs/getting-started.md serves the page as Markdown', async ({ request }) => {
    const res = await request.get('/docs/getting-started.md');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toMatch(/^text\/markdown/);
    const body = await res.text();
    expect(body.startsWith('# Getting started\n')).toBe(true);
    expect(body).toContain('Canonical URL: https://modpagespeed.com/docs/getting-started/');
    expect(body).toContain('](https://modpagespeed.com/docs/is-it-working/)');
    expect(body).not.toContain('](/docs/');
    expect(body).not.toMatch(/^import |<[A-Z][A-Za-z]*\b[^>]*\/>/m);
  });

  test('the HTML page advertises its Markdown twin in <head> and beside the edit link', async ({
    page,
  }) => {
    await page.goto('/docs/getting-started/');
    const alternate = page.locator('head link[rel="alternate"][type="text/markdown"]');
    await expect(alternate).toHaveCount(1);
    await expect(alternate).toHaveAttribute('href', '/docs/getting-started.md');
    const view = page.locator('main a', { hasText: 'View as Markdown' });
    await expect(view).toBeVisible();
    await expect(view).toHaveAttribute('href', '/docs/getting-started.md');
    // The twin a page points at is the one the server serves.
    const res = await page.request.get((await view.getAttribute('href'))!);
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toMatch(/^text\/markdown/);
  });

  test('an unknown slug has no Markdown twin', async ({ request }) => {
    const res = await request.get('/docs/no-such-page.md');
    expect(res.status()).toBe(404);
  });

  // The trailing-slash scheme applies to pages; a file-like URL is served
  // without one and has no slash variant, the same answer /llms.txt/ and
  // /api/product.json/ give. The Markdown route follows that scheme.
  test('the Markdown route is a file-like URL under the trailing-slash scheme', async ({
    request,
  }) => {
    expect((await request.get('/docs/getting-started.md')).status()).toBe(200);
    expect((await request.get('/docs/getting-started.md/')).status()).toBe(404);
    expect((await request.get('/llms.txt/')).status()).toBe(404);
  });

  test('code blocks have a copy button that copies the code', async ({ page, context }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    await page.goto('/docs/installation-docker/');
    const pre = page.locator('main pre:has(> code)').first();
    const button = pre.locator('button.code-copy-btn');
    await expect(button).toBeAttached();
    await expect(button).toHaveAttribute('aria-label', /copy/i);

    await button.click();
    await expect(button).toHaveAttribute('data-copied', 'true');
    await expect(button).toHaveAttribute('aria-label', /copied/i);
    const expected = (await pre.locator('code').textContent()) ?? '';
    const clipboard = await page.evaluate(() => navigator.clipboard.readText());
    expect(clipboard).toBe(expected);
  });

  test('copy button is keyboard operable', async ({ page, context }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    await page.goto('/docs/installation-docker/');
    const button = page.locator('main pre:has(> code) button.code-copy-btn').first();
    await button.focus();
    await page.keyboard.press('Enter');
    await expect(button).toHaveAttribute('data-copied', 'true');
  });

  test('mobile docs navigation is a disclosure, not a navigate-on-change select', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 375, height: 740 });
    await page.goto('/docs/configuration/');
    await expect(page.locator('select#docs-nav')).toHaveCount(0);
    const menu = page.locator('details#docs-menu');
    await expect(menu).toBeVisible();
    await expect(menu).not.toHaveAttribute('open', '');
    const current = menu.locator('nav a[aria-current="page"]');
    await expect(current).toBeHidden();

    await menu.locator('summary').click();
    await expect(menu).toHaveAttribute('open', '');
    await expect(current).toBeVisible();
    await expect(current).toHaveAttribute('href', '/docs/configuration/');
    // Opening the menu changes nothing but the disclosure: same page.
    await expect(page).toHaveURL('/docs/configuration/');
  });

  // Pagefind runs WebAssembly; a Content-Security-Policy without
  // 'wasm-unsafe-eval' makes compilation throw. The dialog must say so
  // visibly instead of leaving the input "Searching…" forever. Simulated by
  // making the probe compile throw, the way a blocked CSP does.
  test('search dialog reports when WebAssembly is blocked', async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(WebAssembly, 'Module', {
        configurable: true,
        value: function () {
          throw new Error('CompileError: WebAssembly is blocked');
        },
      });
    });
    await page.goto('/docs/');
    await page.locator('main button[data-search-open]').click();
    const status = page.locator('#site-search-status');
    await expect(status).toBeVisible();
    await expect(status).toContainText(/WebAssembly is blocked/);
    await expect(page.locator('#site-search input.pagefind-ui__search-input')).toHaveCount(0);
  });
});

// Docs structure: the groups and their order, the retitled pages at their old
// URLs, the preserved anchors, the quickstart chooser and the retired page.
test.describe('Docs structure', () => {
  test('sidebar groups appear in the audience order', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    const labels = await page
      .locator('nav[aria-label="Documentation"] > div > p')
      .allTextContents();
    expect(labels.map((l) => l.trim())).toEqual(GROUP_ORDER);
  });

  test('the hub lists every group once, in order, and links the new pages', async ({ page }) => {
    await page.goto('/docs/');
    const headings = await page.locator('main h2.eyebrow').allTextContents();
    expect(headings.map((h) => h.trim())).toEqual([...GROUP_ORDER, 'Concepts']);
    for (const slug of NEW_PAGES) {
      await expect(page.locator(`main a[href="/docs/${slug}/"]`).first()).toBeAttached();
    }
    // The hero card is Getting started; "Is it working?" sits in its own
    // Start here section rather than vanishing behind the hero.
    await expect(page.locator('main a[href="/docs/is-it-working/"]')).toHaveCount(1);
  });

  test('retitled pages keep their URLs', async ({ page }) => {
    const titles: Array<[string, string]> = [
      ['/docs/installation-module/', 'Install the module on Apache and nginx'],
      ['/docs/deployment/', 'Run in production'],
      ['/docs/production-deployment/', 'ASP.NET Core in production'],
      ['/docs/migrating-to-2-1/', 'Migrate from ModPageSpeed 2.0'],
      ['/docs/aspnet-configuration/', 'ASP.NET Core settings'],
    ];
    for (const [path, title] of titles) {
      await page.goto(path);
      await expect(page.locator('main h1').first()).toHaveText(title);
    }
  });

  test('anchors referenced elsewhere survive the heading changes', async ({ page }) => {
    const anchors: Array<[string, string]> = [
      ['/docs/installation-module/', 'using-the-optimizer-daemon-with-nginx'],
      ['/docs/installation-module/', 'nginx-compatibility'],
      ['/docs/admin-console/', 'optimizer-daemon-panels'],
      ['/docs/migrating-to-2-1/', 'upgrading-from-1-15'],
      ['/docs/migrating-to-2-1/', 'coming-from-open-source'],
      ['/docs/iis-configuration/', 'install-the-module'],
      ['/docs/iis-configuration/', 'iis-tuning'],
      ['/docs/deployment/', 'monitoring'],
      ['/docs/deployment/', 'cache-sizing-recommendations'],
      ['/docs/uninstall/', 'roll-back-to-the-previous-release'],
    ];
    for (const [path, id] of anchors) {
      await page.goto(path);
      await expect(page.locator(`[id="${id}"]`), `${path}#${id}`).toHaveCount(1);
    }
  });

  test('getting started is a chooser with four quickstarts and one verify step', async ({
    page,
  }) => {
    await page.goto('/docs/getting-started/');
    const main = page.locator('main');
    for (const id of [
      'pick-an-integration',
      'quickstart-apache-or-nginx-module',
      'quickstart-docker-reverse-proxy',
      'quickstart-iis',
      'quickstart-aspnet-core',
      'verify-it-works',
    ]) {
      await expect(main.locator(`[id="${id}"]`), id).toHaveCount(1);
    }
    // Every quickstart ends in the same verify step.
    expect(await main.locator('a[href="#verify-it-works"]').count()).toBeGreaterThanOrEqual(4);
    await expect(main).toContainText('curl -I http://localhost/');
    await expect(main.locator('a[href="/docs/is-it-working/"]').first()).toBeAttached();
  });

  test('is-it-working carries the header table, the bypass parameter and the health probe', async ({
    page,
  }) => {
    await page.goto('/docs/is-it-working/');
    const main = page.locator('main');
    for (const text of ['X-Mod-Pagespeed', 'X-Page-Speed', 'X-PageSpeed', '?PageSpeed=off', '/v1/health']) {
      await expect(main).toContainText(text);
    }
    await expect(main.locator('a[href="/pagespeed-markers/"]')).toBeAttached();
  });

  test('/pagespeed-markers/ links to is-it-working', async ({ page }) => {
    await page.goto('/pagespeed-markers/');
    await expect(page.locator('main a[href="/docs/is-it-working/"]')).toBeAttached();
  });

  test('the retired documentation-plan page is a 301 to the hub', async ({ request, baseURL }) => {
    const res = await request.get('/docs/documentation-plan/', { maxRedirects: 0 });
    expect(res.status()).toBe(301);
    expect(new URL(res.headers()['location'], baseURL).pathname).toBe('/docs/');
  });

  test('prose says worker, not daemon, on the pages this tranche owns', async ({ page }) => {
    for (const path of ['/docs/getting-started/', '/docs/installation-module/']) {
      await page.goto(path);
      const text = (await page.locator('main .prose').innerText()).replace(
        /\b(?:Daemon[A-Za-z]+|ipro_daemon_\w+|pagespeed_daemon\.conf|daemon\.env|daemon-reload|\/v1\/daemon\/[^\s]*)\b/g,
        '',
      );
      expect(text, path).not.toMatch(/\bdaemon\b/i);
    }
  });
});

test.describe('Admin console docs', () => {
  test('opens with a tip pointing at the live console', async ({ page }) => {
    await page.goto('/docs/admin-console/');
    const tip = page.locator('aside.callout-tip').first();
    await expect(tip).toContainText('See it running');
    await expect(tip.locator('a[href="https://we-amp.com/pagespeed_global_admin/"]')).toBeVisible();
    await expect(tip).toContainText('public on purpose');
  });
});
