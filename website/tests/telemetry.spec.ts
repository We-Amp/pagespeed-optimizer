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

  test('no field value is clipped at 360, 390, 414 or 1440', async ({ browser }) => {
    for (const width of [360, 390, 414, 1440]) {
      const context = await browser.newContext({ viewport: { width, height: 844 } });
      // Keep the strip in its captured state: a failed probe applies nothing.
      await context.route('**/*', (route) =>
        route.request().method() === 'HEAD' ? route.abort() : route.continue(),
      );
      const page = await context.newPage();
      await page.goto('/');
      const strip = page.locator(STRIP);
      await expect(strip).toBeVisible();
      // The dev server serves scoped styles via JS; wait until they land.
      await expect
        .poll(() =>
          page.evaluate(
            () =>
              getComputedStyle(document.querySelector('[data-ui="telemetry-strip"]')!).blockSize,
          ),
        )
        .toBe('32px');
      const problems = await page.evaluate(() => {
        const el0 = document.querySelector<HTMLElement>('[data-ui="telemetry-strip"]')!;
        const out: string[] = [];
        if (el0.scrollWidth > el0.clientWidth)
          out.push(`strip overflow: scrollWidth ${el0.scrollWidth} > clientWidth ${el0.clientWidth}`);
        if (document.documentElement.scrollWidth > window.innerWidth)
          out.push(`page horizontal overflow: ${document.documentElement.scrollWidth} > ${window.innerWidth}`);
        el0.querySelectorAll<HTMLElement>('[data-t]').forEach((el) => {
          if (el.getClientRects().length === 0) return; // not rendered at this width
          if (el.scrollWidth > el.clientWidth)
            out.push(
              `[data-t="${el.dataset.t}"] clipped: scrollWidth ${el.scrollWidth} > clientWidth ${el.clientWidth} ("${el.textContent}")`,
            );
        });
        return out;
      });
      expect(problems, `viewport ${width}px`).toEqual([]);
      await context.close();
    }
  });

  test('the skip link is still the first tab stop', async ({ page }) => {
    await page.goto('/');
    await page.locator(STRIP).waitFor();
    await page.keyboard.press('Tab');
    await expect
      .poll(() =>
        page.evaluate(() => (document.activeElement as HTMLElement | null)?.getAttribute('href')),
      )
      .toBe('#main-content');
  });

  test('the strip is not rendered on the 404 page', async ({ page }) => {
    await page.goto('/404/');
    await expect(page.locator(STRIP)).toHaveCount(0);
  });
});
