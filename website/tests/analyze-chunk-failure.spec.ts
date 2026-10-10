// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// Deploy-skew failure mode on /analyze/: the audit-mapping table is a lazy
// chunk requested on the first submit. If a deploy replaced the _astro/
// hashes after the page loaded, the chunk request fails. The run must then
// show a reload-the-page error — not the "PageSpeed Insights did not answer." wording, and
// never the internal chunk URL — and "Try again" must not fire PSI
// requests, because the browser caches the failed module fetch and a retry
// can never recover until the page is reloaded.

// Minimal PSI response: a 0.9 performance score and one flagged audit.
const PSI_OK = {
  lighthouseResult: {
    categories: { performance: { score: 0.9 } },
    audits: {
      'render-blocking-resources': {
        id: 'render-blocking-resources',
        title: 'Eliminate render-blocking resources',
        score: 0.4,
      },
    },
  },
};

test.describe('analyzer mapping chunk failure', () => {
  test('shows a reload error, hides the chunk URL, and Try again fires no PSI request', async ({
    page,
  }) => {
    const pageErrors: string[] = [];
    page.on('pageerror', (err) => pageErrors.push(String(err)));

    const psiCalls: string[] = [];
    await page.route('**/psi/v5/runPagespeed**', async (route) => {
      psiCalls.push(route.request().url());
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(PSI_OK),
      });
    });
    // Route by URL pattern: /_astro/psi-mps-mapping.<hash>.js in the build,
    // the source module path under the dev server.
    await page.route('**/psi-mps-mapping*', (route) => route.abort());

    // Warm up the page first: on a cold dev server Vite re-optimizes the
    // page's module graph on the first /analyze/ hit and forces one full
    // page reload, which would re-fire the auto-run mid-test. This visit
    // has no ?url= param, so it starts no run; the reload cycle completes
    // here and the measured run below is stable.
    await page.goto('/analyze/');
    await page.waitForLoadState('networkidle');

    // Trigger through the ?url= deep-link auto-run: the page script starts
    // the analysis itself once it initializes, so the test does not depend
    // on click-vs-init timing (the dev server compiles the page lazily).
    await page.goto('/analyze/?url=https://example.com');

    await expect(page.locator('#psi-error')).toBeVisible({ timeout: 15000 });
    await expect(page.locator('#psi-error-title')).toHaveText('This page needs a reload.');
    await expect(page.locator('#psi-error-body')).toContainText('Reload the page');

    // The internal asset URL must never reach visible text.
    const visibleText = await page.locator('body').innerText();
    expect(visibleText).not.toContain('psi-mps-mapping');
    expect(visibleText).not.toContain('/_astro/');
    expect(visibleText).not.toContain('dynamically imported');

    // Both strategies fire on the first run; the chunk failure happens
    // alongside, not instead of them.
    expect(psiCalls).toHaveLength(2);

    // Retry can never recover (the browser caches the failed module fetch),
    // so it must not spend PSI calls. networkidle gives any wrongly-fired
    // request a window to show up before the negative assertion.
    await page.click('#psi-error-retry');
    await expect(page.locator('#psi-error-title')).toHaveText('This page needs a reload.');
    await page.waitForLoadState('networkidle');
    expect(psiCalls).toHaveLength(2);

    // The chunk rejection lands while the PSI fetches are still in flight;
    // a handler is attached up front, so nothing escapes as an unhandled
    // rejection.
    expect(pageErrors).toEqual([]);
  });
});
