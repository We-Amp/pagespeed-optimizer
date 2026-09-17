// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { test, expect } from '@playwright/test';

// Auto-discover first-class Astro routes from src/pages/**/*.astro so that
// adding a new top-level page immediately gets link-integrity coverage.
//
// File-to-route mapping:
//   index.astro           -> /
//   features.astro        -> /features/
//   blog/index.astro      -> /blog/
//   docs/foo/index.astro  -> /docs/foo/
//
// Skip rules — keep narrow; silently dropping a route is how #275 happened,
// so prefer adding a page over widening a skip:
//   - Dynamic routes: any filename containing `[` (e.g. [slug].astro).
//   - Error pages: 404.astro, 500.astro — only rendered on error.
//   - Legacy static: 1.0/**, 1.1/** — pre-Astro HTML, not served by dev.
//   - API endpoints: api/** — .ts handlers returning JSON, not pages.
const PAGES_DIR = fileURLToPath(new URL('../src/pages', import.meta.url));

function discoverPages(pagesDir: string): string[] {
  const entries = readdirSync(pagesDir, { recursive: true }) as string[];
  const routes: string[] = [];
  for (const entry of entries) {
    const rel = entry.replace(/\\/g, '/'); // normalize on Windows
    if (!rel.endsWith('.astro')) continue;
    if (rel.includes('[')) continue;
    if (rel === '404.astro' || rel === '500.astro') continue;
    if (rel.startsWith('1.0/') || rel.startsWith('1.1/') || rel.startsWith('api/')) continue;

    let route: string;
    if (rel === 'index.astro') route = '/';
    else if (rel.endsWith('/index.astro')) route = `/${rel.slice(0, -'/index.astro'.length)}/`;
    else route = `/${rel.slice(0, -'.astro'.length)}/`;
    routes.push(route);
  }
  return routes.sort();
}

const PAGES_TO_CHECK = discoverPages(PAGES_DIR);

if (PAGES_TO_CHECK.length === 0) {
  throw new Error(`links.spec.ts: discovered no pages under ${PAGES_DIR}`);
}

test.describe('Link integrity', () => {
  for (const pagePath of PAGES_TO_CHECK) {
    test(`all anchor hrefs on ${pagePath} resolve`, async ({ page, baseURL }) => {
      const response = await page.goto(pagePath);
      // /error/404/ and /error/500/ are branded, prerendered, statically-served
      // preview pages. In prod nginx wires them as its error_page targets and
      // injects the real 4xx/5xx itself (deploy/nginx.conf); as direct URLs they
      // are ordinary 200 pages. Astro does not stamp status by basename on these
      // non-special paths (and post-7.x only does so for exact /404 and /500).
      // So link-integrity treats them like any other page.
      expect(response?.status()).toBeLessThan(400);

      // Some pages (e.g. /privacy/) 301-redirect to canonical URLs on a different
      // origin. Once we've left baseURL there's nothing meaningful to assert about
      // the landing page's hrefs in the modpagespeed.com link-integrity context.
      if (baseURL && new URL(page.url()).origin !== new URL(baseURL).origin) {
        test.info().annotations.push({
          type: 'skipped',
          description: `${pagePath} redirected off-origin to ${page.url()}`,
        });
        return;
      }

      // Collect all anchor hrefs that point to fragment identifiers on this page.
      const fragmentLinks = await page.$$eval('a[href^="#"]', (anchors) =>
        anchors
          .map((a) => a.getAttribute('href'))
          .filter((h): h is string => h !== null && h !== '#'),
      );

      // Verify each fragment target exists on the page.
      for (const href of fragmentLinks) {
        const targetId = href.slice(1); // remove leading #
        const target = page.locator(`[id="${targetId}"]`);
        await expect(target, `${pagePath}: ${href} target missing`).toHaveCount(1, {
          timeout: 2000,
        });
      }
    });

    test(`no broken internal links on ${pagePath}`, async ({ page, baseURL }) => {
      await page.goto(pagePath);

      if (baseURL && new URL(page.url()).origin !== new URL(baseURL).origin) {
        test.info().annotations.push({
          type: 'skipped',
          description: `${pagePath} redirected off-origin to ${page.url()}`,
        });
        return;
      }

      // Collect all internal links (starting with /).
      const internalLinks = await page.$$eval('a[href^="/"]', (anchors) =>
        [...new Set(anchors.map((a) => a.getAttribute('href')))].filter(
          (h): h is string => h !== null,
        ),
      );

      for (const href of internalLinks) {
        // Strip fragment
        const path = href.split('#')[0];
        if (!path) continue;

        // Skip legacy static paths not served by Astro dev server
        if (path.startsWith('/1.0/') || path.startsWith('/1.1/')) continue;

        const resp = await page.request.get(path);
        expect(resp.status(), `${pagePath} -> ${href} returned ${resp.status()}`).toBeLessThan(400);
      }
    });
  }

  // The real error routes (top-level /404, /500) are excluded from the
  // link-integrity crawl above because they only render on error, but their
  // HTTP status IS a contract the dev/preview server must honor by basename.
  // This is the signal the old /error/ assertion was reaching for; assert it
  // directly against the routes that actually carry it. (Prod maps these via
  // nginx error_page → the prerendered /error/* pages; that path is smoked at
  // deploy time, not here.)
  test('real error routes return their real status', async ({ page }) => {
    expect((await page.goto('/404'))?.status()).toBe(404);
    expect((await page.goto('/500'))?.status()).toBe(500);
  });

  test('no form actions pointing to missing endpoints', async ({ page }) => {
    for (const pagePath of PAGES_TO_CHECK) {
      await page.goto(pagePath);

      const formActions = await page.$$eval('form[action]', (forms) =>
        forms
          .map((f) => f.getAttribute('action'))
          .filter((a): a is string => a !== null && a.startsWith('/')),
      );

      for (const action of formActions) {
        const resp = await page.request.post(action, {
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          data: 'test=1',
        });
        // A 405 (Method Not Allowed) is acceptable -- the endpoint exists.
        // A 404 means the endpoint is missing.
        expect(resp.status(), `Form on ${pagePath} POSTs to ${action} which returned 404`).not.toBe(
          404,
        );
      }
    }
  });
});
