// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect, type Page } from '@playwright/test';

// The v2 AI readability panel. The scanner and PageSpeed Insights are mocked;
// the real services are never called.

const fixture = (name: string) =>
  readFileSync(new URL(`./fixtures/scan/report-${name}.json`, import.meta.url), 'utf8');

interface Calls {
  scan: number;
  psi: number;
}

async function mock(
  page: Page,
  opts: {
    report?: 'full' | 'clean';
    scanStatus?: number;
    scanError?: string;
    abort?: boolean;
    hang?: boolean;
  } = {},
): Promise<Calls> {
  const calls: Calls = { scan: 0, psi: 0 };
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    calls.psi += 1;
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ lighthouseResult: { categories: { performance: { score: 0.9 } } } }),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    calls.scan += 1;
    if (opts.hang) return new Promise<void>(() => {});
    if (opts.abort) return route.abort();
    if (opts.scanStatus && opts.scanStatus !== 200) {
      return route.fulfill({
        status: opts.scanStatus,
        contentType: 'application/json',
        body: JSON.stringify({ error: opts.scanError ?? 'scanner unavailable' }),
      });
    }
    return route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: fixture(opts.report ?? 'full'),
    });
  });
  return calls;
}

async function warm(page: Page, path: string) {
  await page.goto(path);
  await page.waitForLoadState('networkidle');
}

async function scanV2(page: Page) {
  await warm(page, '/ai-readability/?ui=v2');
  await page.fill('#scan-url', 'shop.example.com');
  await page.click('#scan-submit');
  await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).not.toHaveText(
    'Checking…',
    { timeout: 15000 },
  );
}

const panel = (page: Page) => page.locator('[data-scan-panel="airead"]');

async function openAiread(page: Page) {
  await page.click('[data-scan-tile="airead"]');
  await expect(panel(page)).toBeVisible();
}

test.describe('AI readability panel', () => {
  test('the grade row and category rows match the original page', async ({ page }) => {
    await mock(page);
    await warm(page, '/ai-readability/?ui=v1');
    await page.fill('#ar-url', 'shop.example.com');
    await page.click('#ar-go');
    await expect(page.locator('.ar-cat').first()).toBeVisible({ timeout: 15000 });
    const v1 = {
      grade: (await page.locator('.ar-badge').innerText()).trim(),
      score: (await page.locator('.ar-score').innerText()).trim(),
      cats: await page.locator('.ar-cat').evaluateAll((els) =>
        els.map((e) => ({
          name: e.querySelector('.ar-top span')!.firstChild!.textContent,
          flag: e.querySelector('.ar-flag')?.textContent ?? '',
          score:
            e.querySelectorAll('.ar-top span')[e.querySelector('.ar-flag') ? 2 : 1].textContent,
          verdict: e.querySelector('.ar-v')!.textContent,
        })),
      ),
    };

    const p2 = await page.context().newPage();
    await mock(p2);
    await scanV2(p2);
    await openAiread(p2);
    const cats = p2.locator('.scan-airead-cat');
    await expect(cats).toHaveCount(5);
    expect(v1.cats).toHaveLength(5);
    await expect(p2.locator('[role="img"][aria-label^="Grade"]')).toHaveText(v1.grade);
    await expect(panel(p2)).toContainText(v1.score);
    for (let i = 0; i < 5; i++) {
      const row = cats.nth(i);
      const text = await row.innerText();
      expect(text).toContain(v1.cats[i].name!);
      expect(text).toContain(v1.cats[i].score!);
      expect(text).toContain(v1.cats[i].verdict!);
      if (v1.cats[i].flag) expect(text).toContain(v1.cats[i].flag);
    }
  });

  test('shows the content-gap flag, the word and the token line', async ({ page }) => {
    await mock(page);
    await scanV2(page);
    await openAiread(page);
    await expect(panel(page)).toContainText('Grade C — mixed');
    await expect(panel(page)).toContainText('64/100');
    await expect(panel(page).getByText('invisible without JS')).toHaveCount(1);
    await expect(panel(page)).toContainText(
      'Readable text in raw HTML: ~190 tokens · after JavaScript: ~540 tokens',
    );
  });

  test('the crawler text sits behind a disclosure and scrolls inside its box', async ({ page }) => {
    await mock(page, { report: 'clean' });
    await scanV2(page);
    await openAiread(page);
    const pre = panel(page).locator('pre');
    await expect(pre).toBeHidden();
    await panel(page).getByText('Show what a crawler reads').click();
    await expect(pre).toBeVisible();
    const box = await pre.evaluate((el) => {
      const cs = getComputedStyle(el);
      return {
        overflowY: cs.overflowY,
        maxHeight: parseFloat(cs.maxHeight),
        lineHeight: parseFloat(cs.lineHeight),
        pageScroll: document.documentElement.scrollWidth > document.documentElement.clientWidth,
      };
    });
    expect(box.overflowY).toBe('auto');
    expect(box.maxHeight / box.lineHeight).toBeLessThanOrEqual(16.01);
    expect(box.pageScroll).toBe(false);
  });

  test('the cut blocks are absent', async ({ page }) => {
    await mock(page);
    await scanV2(page);
    await openAiread(page);
    const body = (await panel(page).innerText()).toLowerCase();
    for (const gone of [
      'beyond the grade',
      'after rendering javascript',
      'agent verification',
      'provenance',
      'sovereignty',
      'webmcp',
      'hidden layer',
      'posture',
      'blocked',
      'accessibility score',
    ]) {
      expect(body).not.toContain(gone);
    }
    await expect(panel(page).locator('pre')).toHaveCount(1);
  });

  test('"Also found" lines appear only when their gate fires', async ({ page }) => {
    await mock(page, { report: 'full' });
    await scanV2(page);
    await openAiread(page);
    const found = panel(page).locator('[data-scan-also-found] li');
    await expect(found).toHaveCount(3);
    await expect(found.nth(0)).toContainText('can’t tell a real signed agent from an impostor');
    await expect(found.nth(1)).toContainText('signed-agent requests differently');
    await expect(found.nth(2)).toContainText('self-hosted optimizer can fix on your own servers');
  });

  test('no "Also found" block on a clean report', async ({ page }) => {
    await mock(page, { report: 'clean' });
    await scanV2(page);
    await openAiread(page);
    await expect(panel(page).locator('[data-scan-also-found]')).toHaveCount(0);
    await expect(panel(page)).not.toContainText('Also found');
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-value]')).toHaveText(
      'Grade A · 100/100',
    );
  });
});

test.describe('scanner failures', () => {
  test('an error answer marks both tiles Not measured and shows the message', async ({ page }) => {
    await mock(page, { scanStatus: 503, scanError: 'The scanner is busy. Try again shortly.' });
    await scanV2(page);
    for (const t of ['airead', 'risk']) {
      await expect(page.locator(`[data-scan-tile="${t}"] [data-scan-status]`)).toHaveText(
        'Not measured',
      );
    }
    await openAiread(page);
    await expect(panel(page)).toContainText('Couldn’t scan that.');
    await expect(panel(page)).toContainText('The scanner is busy. Try again shortly.');
    await expect(panel(page).getByRole('button', { name: 'Try again' })).toBeVisible();
    await page.click('[data-scan-tile="risk"]');
    await expect(page.locator('[data-scan-panel="risk"]')).toContainText(
      'The scanner is busy. Try again shortly.',
    );
  });

  test('an unreachable scanner says so', async ({ page }) => {
    await mock(page, { abort: true });
    await scanV2(page);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      'Not measured',
    );
    await openAiread(page);
    await expect(panel(page)).toContainText(
      'Could not reach the scanner service. Check the URL and try again.',
    );
  });

  test('a non-JSON proxy error reads as unreachable, not as a raw status', async ({ page }) => {
    await page.route('**/psi/v5/runPagespeed**', (route) => route.abort());
    await page.route('**/ai-readability/api/scan**', (route) =>
      route.fulfill({ status: 502, contentType: 'text/html', body: '<h1>Bad gateway</h1>' }),
    );
    await scanV2(page);
    await openAiread(page);
    await expect(panel(page)).toContainText(
      'Could not reach the scanner service. Check the URL and try again.',
    );
    await expect(panel(page)).not.toContainText('HTTP 502');
    await expect(panel(page)).not.toContainText('Couldn’t scan that.');
  });

  test('a scanner that never answers times out', async ({ page }) => {
    await page.clock.install();
    await mock(page, { hang: true });
    await warm(page, '/ai-readability/?ui=v2');
    await page.fill('#scan-url', 'shop.example.com');
    await page.click('#scan-submit');
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).toHaveText(
      'Checking…',
    );
    await page.clock.runFor(46_000);
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).toHaveText(
      'Not measured',
    );
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      'Not measured',
    );
    await openAiread(page);
    await expect(panel(page)).toContainText('The scanner did not answer in time.');
  });

  test('Try again re-runs only the scanner', async ({ page }) => {
    let fail = true;
    const calls: Calls = { scan: 0, psi: 0 };
    await page.route('**/psi/v5/runPagespeed**', (route) => {
      calls.psi += 1;
      return route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ lighthouseResult: { categories: { performance: { score: 0.9 } } } }),
      });
    });
    await page.route('**/ai-readability/api/scan**', (route) => {
      calls.scan += 1;
      if (fail) {
        return route.fulfill({
          status: 500,
          contentType: 'application/json',
          body: JSON.stringify({ error: 'boom' }),
        });
      }
      return route.fulfill({ status: 200, contentType: 'application/json', body: fixture('full') });
    });
    await scanV2(page);
    await openAiread(page);
    const psiBefore = calls.psi;
    expect(calls.scan).toBe(1);
    fail = false;
    await panel(page).getByRole('button', { name: 'Try again' }).click();
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).toHaveText(
      'Needs work',
    );
    expect(calls.scan).toBe(2);
    expect(calls.psi).toBe(psiBefore);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).not.toHaveText(
      'Not measured',
    );
  });
});
