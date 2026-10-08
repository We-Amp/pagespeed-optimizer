// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect } from '@playwright/test';

// Expected fallback values come from the committed capture file; the live
// upgrade is tested with a fictitious header value (9.9.9-test), never a real
// or guessed production one.
const capture = JSON.parse(
  readFileSync(new URL('../src/data/telemetry-capture.json', import.meta.url), 'utf8'),
) as {
  capturedAt: string;
  paths: Record<string, { wireBytes: number; headers: Record<string, string | null> }>;
};
const capturedDate = capture.capturedAt.slice(0, 10);
const capturedXmps = capture.paths['/'].headers['x-mod-pagespeed'] ?? 'not present';

const STRIP = '[data-ui="telemetry-strip"]';

test.describe('Telemetry strip', () => {
  test('with JS disabled it shows the captured values and their date', async ({ browser }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip).toBeVisible();
    await expect(strip.locator('[data-t="state"]').first()).toHaveText(`captured ${capturedDate}`);
    await expect(strip.locator('[data-t="xmps"]')).toHaveText(capturedXmps);
    // A real captured byte value, not a dash (the root path is in the capture).
    await expect(strip.locator('[data-t="wire"]')).toHaveText(/\d[\d,]*\sB/);
    await context.close();
  });

  test('live upgrade shows the HEAD response header and the live state', async ({ page }) => {
    // The strip issues the page's only HEAD request; give it a fictitious
    // x-mod-pagespeed value.
    await page.route('**/*', (route) =>
      route.request().method() === 'HEAD'
        ? route.fulfill({
            status: 200,
            headers: { 'x-mod-pagespeed': '9.9.9-test', vary: 'Save-Data,Accept-Encoding' },
          })
        : route.continue(),
    );
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip.locator('[data-t="state"]').first()).toHaveText('this view · live');
    await expect(strip.locator('[data-t="xmps"]')).toHaveText('9.9.9-test');
    await expect(strip.locator('[data-t="vary"]')).toHaveText('Save-Data,Accept-Encoding');
  });

  test('a failed probe keeps the captured fallback', async ({ page }) => {
    await page.route('**/*', (route) =>
      route.request().method() === 'HEAD' ? route.abort() : route.continue(),
    );
    const headSeen = page.waitForRequest((req) => req.method() === 'HEAD');
    await page.goto('/');
    await headSeen;
    // Give the rejected fetch a beat in which it must NOT apply anything.
    await page.waitForTimeout(500);
    const strip = page.locator(STRIP);
    await expect(strip.locator('[data-t="state"]').first()).toHaveText(`captured ${capturedDate}`);
    await expect(strip.locator('[data-t="xmps"]')).toHaveText(capturedXmps);
  });

  test('the strip box is identical before and after the live upgrade', async ({ page }) => {
    // Hold the HEAD response back so the first measurement is the SSR fallback.
    await page.route('**/*', async (route) => {
      if (route.request().method() === 'HEAD') {
        await new Promise((r) => setTimeout(r, 1200));
        return route.fulfill({ status: 200, headers: { 'x-mod-pagespeed': '9.9.9-test' } });
      }
      return route.continue();
    });
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip).toBeVisible();
    const before = await strip.boundingBox();
    await expect(strip.locator('[data-t="state"]').first()).toHaveText('this view · live');
    const after = await strip.boundingBox();
    expect(after).toEqual(before);
  });

  test('the skip link is still the first tab stop', async ({ page }) => {
    await page.goto('/');
    await page.keyboard.press('Tab');
    await expect(page.locator(':focus')).toHaveAttribute('href', '#main-content');
  });

  test('the strip is not rendered on the 404 page', async ({ page }) => {
    await page.goto('/404/');
    await expect(page.locator(STRIP)).toHaveCount(0);
  });
});
