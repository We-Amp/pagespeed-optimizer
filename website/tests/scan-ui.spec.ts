// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';

// The shared scan interface ships behind a flag: v1 stays the default, `?ui=v2`
// opts in (and sticks), `?ui=v1` opts out. These specs pin the flag, the
// double-execution guard and the tile states of the v2 shell.

const PSI_BODY = (score: number) => ({
  lighthouseResult: {
    categories: { performance: { score } },
    audits: {
      'render-blocking-resources': {
        id: 'render-blocking-resources',
        title: 'Eliminate render-blocking resources',
        score: 0.4,
      },
    },
  },
});

const SCAN_BODY = {
  cached: false,
  id: 'abc123',
  permalink: 'https://modpagespeed.com/ai-readability/r/abc123',
  report: {
    url: 'https://example.com/',
    grade: 'C',
    score: 64,
    categories: {},
    preConsentLeak: { status: 'ok', verdict: 'leaks', trackerCount: 3 },
    scriptInventory: { status: 'ok', verdict: 'clean' },
  },
};

interface Calls {
  mobile: number;
  desktop: number;
  scan: number;
}

async function mockBackends(
  page: Page,
  opts: { mobile?: number; desktop?: number; scanStatus?: number; delayMs?: number } = {},
): Promise<Calls> {
  const calls: Calls = { mobile: 0, desktop: 0, scan: 0 };
  const delay = () => new Promise((r) => setTimeout(r, opts.delayMs ?? 0));
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    const strategy = new URL(route.request().url()).searchParams.get('strategy');
    if (strategy === 'mobile') calls.mobile += 1;
    if (strategy === 'desktop') calls.desktop += 1;
    await delay();
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(
        PSI_BODY(strategy === 'mobile' ? (opts.mobile ?? 0.4) : (opts.desktop ?? 0.95)),
      ),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    calls.scan += 1;
    await delay();
    const status = opts.scanStatus ?? 200;
    await route.fulfill({
      status,
      contentType: 'application/json',
      body: JSON.stringify(status === 200 ? SCAN_BODY : { error: 'scanner unavailable' }),
    });
  });
  return calls;
}

// A cold dev server re-optimizes its module graph on the first hit of a page
// and reloads once; do that outside the measured visit.
async function warm(page: Page, path: string) {
  await page.goto(path);
  await page.waitForLoadState('networkidle');
}

const PAGES = [
  { path: '/analyze/', v1Form: '#psi-form' },
  { path: '/ai-readability/', v1Form: '#ar-form' },
];

test.describe('scan interface flag', () => {
  for (const { path, v1Form } of PAGES) {
    test(`${path}: v1 is untouched by default`, async ({ page }) => {
      await page.goto(path);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v1');
      await expect(page.locator(v1Form)).toBeVisible();
      await expect(page.locator('form[data-scan-box]')).toBeHidden();
      await expect(page.locator('[data-scan-results]')).toBeHidden();
    });

    test(`${path}: ?ui=v2 shows v2, sticks after a reload, and ?ui=v1 escapes`, async ({
      page,
    }) => {
      await page.goto(`${path}?ui=v2`);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v2');
      await expect(page.locator('form[data-scan-box]')).toBeVisible();
      await expect(page.locator(v1Form)).toBeHidden();

      await page.goto(path);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v2');
      await expect(page.locator('form[data-scan-box]')).toBeVisible();
      await expect(page.locator(v1Form)).toBeHidden();

      await page.goto(`${path}?ui=v1`);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v1');
      await expect(page.locator(v1Form)).toBeVisible();
      await expect(page.locator('form[data-scan-box]')).toBeHidden();

      await page.goto(path);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v1');
    });
  }

  test('the v2 hero keeps one disclaimer and one privacy note', async ({ page }) => {
    await page.goto('/analyze/?ui=v2');
    await expect(page.locator('p:visible', { hasText: 'This tool calls Google' })).toHaveCount(1);
    await expect(page.getByRole('link', { name: 'Privacy policy' }).first()).toBeVisible();
  });
});

test.describe('deep link runs each request once', () => {
  for (const ui of ['v1', 'v2']) {
    test(`/analyze/?url= under ${ui} calls PSI once per strategy`, async ({ page }) => {
      const calls = await mockBackends(page);
      await warm(page, `/analyze/?ui=${ui}`);
      calls.mobile = calls.desktop = calls.scan = 0;
      await page.goto(`/analyze/?ui=${ui}&url=https://example.com`);
      if (ui === 'v1') {
        await expect(page.locator('#psi-results')).toBeVisible({ timeout: 15000 });
      } else {
        await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).not.toHaveText(
          'Checking…',
          { timeout: 15000 },
        );
      }
      await page.waitForTimeout(500);
      expect(calls.mobile).toBe(1);
      expect(calls.desktop).toBe(1);
      expect(calls.scan).toBe(ui === 'v2' ? 1 : 0);
    });
  }

  test('/ai-readability/?url= under v2 runs all three requests once', async ({ page }) => {
    const calls = await mockBackends(page);
    await warm(page, '/ai-readability/?ui=v2');
    calls.mobile = calls.desktop = calls.scan = 0;
    await page.goto('/ai-readability/?ui=v2&url=https://example.com');
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).not.toHaveText(
      'Checking…',
      { timeout: 15000 },
    );
    await page.waitForTimeout(500);
    expect(calls).toEqual({ mobile: 1, desktop: 1, scan: 1 });
  });
});

test.describe('v2 shell tile states', () => {
  test('tiles start as Checking and settle independently', async ({ page }) => {
    await mockBackends(page, { mobile: 0.4, desktop: 0.95 });
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'example.com');
    await page.click('#scan-submit');

    const speed = page.locator('[data-scan-tile="speed"]');
    const airead = page.locator('[data-scan-tile="airead"]');
    const risk = page.locator('[data-scan-tile="risk"]');
    await expect(page.locator('[data-scan-results]')).toBeVisible();
    await expect(speed.locator('[data-scan-status]')).toHaveText('Poor', { timeout: 15000 });
    await expect(speed.locator('[data-scan-value]')).toHaveText('Mobile 40 · Desktop 95');
    await expect(airead.locator('[data-scan-status]')).toHaveText('Needs work');
    await expect(airead.locator('[data-scan-value]')).toHaveText('Grade C · 64/100');
    await expect(risk.locator('[data-scan-status]')).toHaveText('1 flagged');
    await expect(risk.locator('[data-scan-value]')).toHaveText('1 of 2 checks flagged');
    await expect(page.locator('[data-scan-health]')).toHaveText('3 of 3 areas need attention');
    await expect(page.locator('[data-scan-live]')).not.toBeEmpty();
  });

  test('a scanner error marks AI readability and Risk & SEO as not measured', async ({ page }) => {
    await mockBackends(page, { scanStatus: 503 });
    await warm(page, '/ai-readability/?ui=v2');
    await page.fill('#scan-url', 'https://example.com');
    await page.click('#scan-submit');
    for (const p of ['airead', 'risk']) {
      const tile = page.locator(`[data-scan-tile="${p}"]`);
      await expect(tile.locator('[data-scan-status]')).toHaveText('Not measured', {
        timeout: 15000,
      });
      await expect(tile.locator('[data-scan-value]')).toHaveText('scanner unavailable');
    }
  });

  test('an invalid URL is rejected before any request', async ({ page }) => {
    const calls = await mockBackends(page);
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'ftp://example.com');
    await page.click('#scan-submit');
    await expect(page.locator('#scan-form-error')).toHaveText(
      'Only http:// and https:// URLs are supported.',
    );
    await expect(page.locator('[data-scan-results]')).toBeHidden();
    expect(calls).toEqual({ mobile: 0, desktop: 0, scan: 0 });
  });

  test('tiles are disclosure buttons and only one panel is open', async ({ page }) => {
    await mockBackends(page);
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'https://example.com');
    await page.click('#scan-submit');
    const speed = page.locator('[data-scan-tile="speed"]');
    const airead = page.locator('[data-scan-tile="airead"]');
    await expect(speed).toHaveAttribute('aria-expanded', 'false');
    await expect(speed).toHaveAttribute('aria-controls', 'scan-panel-speed');
    await speed.click();
    await expect(speed).toHaveAttribute('aria-expanded', 'true');
    await expect(speed).toHaveAttribute('aria-label', 'Hide Speed details');
    await expect(page.locator('#scan-panel-speed')).toBeVisible();
    await airead.click();
    await expect(speed).toHaveAttribute('aria-expanded', 'false');
    await expect(airead).toHaveAttribute('aria-expanded', 'true');
    await expect(page.locator('#scan-panel-speed')).toBeHidden();
    await expect(page.locator('#scan-panel-airead')).toBeVisible();
    await airead.click();
    await expect(page.locator('#scan-panel-airead')).toBeHidden();
  });

  test('the fixture hook paints every tile without a network', async ({ page }) => {
    await warm(page, '/ai-readability/?ui=v2');
    await page.goto('/ai-readability/?ui=v2&fixture=full');
    await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).toHaveText(
      'Needs work',
    );
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      '2 flagged',
    );
  });
});

test.describe('v2 layout', () => {
  test('one tile column on a phone, three on a desktop, no horizontal scroll', async ({ page }) => {
    await page.route('**/psi/v5/runPagespeed**', (r) => r.fulfill({ status: 200, body: '{}' }));
    await warm(page, '/analyze/?ui=v2');
    for (const [width, columns] of [
      [390, 1],
      [1280, 3],
    ] as const) {
      await page.setViewportSize({ width, height: 844 });
      await page.goto('/analyze/?ui=v2&fixture=full');
      await expect(page.locator('[data-scan-results]')).toBeVisible();
      const lefts = await page
        .locator('[data-scan-tile]')
        .evaluateAll((els) => els.map((e) => Math.round(e.getBoundingClientRect().left)));
      expect(new Set(lefts).size).toBe(columns);
      const overflow = await page.evaluate(
        () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
      );
      expect(overflow).toBeLessThanOrEqual(0);
      const box = await page.locator('[data-scan-tile="speed"]').boundingBox();
      expect(box!.height).toBeGreaterThanOrEqual(44);
    }
  });

  for (const path of ['/analyze/', '/ai-readability/']) {
    test(`${path}: the skeleton and the tile swap shift the layout by at most 0.02`, async ({
      page,
    }) => {
      await page.addInitScript(() => {
        const w = window as unknown as { __cls: number };
        w.__cls = 0;
        new PerformanceObserver((list) => {
          for (const entry of list.getEntries()) {
            const shift = entry as PerformanceEntry & { hadRecentInput?: boolean; value?: number };
            if (!shift.hadRecentInput) w.__cls += shift.value ?? 0;
          }
        }).observe({ type: 'layout-shift', buffered: true });
      });
      await mockBackends(page, { delayMs: 1500 });
      await warm(page, `${path}?ui=v2`);
      await page.goto(`${path}?ui=v2`);
      await page.fill('#scan-url', 'https://example.com');
      await page.click('#scan-submit');
      await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).toHaveText('Poor', {
        timeout: 15000,
      });
      await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
        '1 flagged',
      );
      await page.waitForTimeout(500);
      const cls = await page.evaluate(() => (window as unknown as { __cls: number }).__cls);
      expect(cls).toBeLessThanOrEqual(0.02);
    });
  }
});
