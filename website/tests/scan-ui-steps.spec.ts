// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';
import { mockScan, startScan } from './helpers/scan-mock';

// The "Your next steps" list at the top of a scan result, the summary line
// above it, the note under the topics and the seconds counter of the wait.

const CONTENT =
  'Put this page’s main text in the HTML the server sends, so it does not depend on JavaScript.';
const TITLE =
  'JavaScript changes this page’s title after it loads. Put the final one in the HTML the server sends.';
const SPEED =
  'Speed is poor on mobile: open the Speed panel for the audits PageSpeed Insights flagged.';
const A11Y =
  'Fix 7 accessibility issues the scan found: Images must have alternative text. These are automated checks, not an accessibility audit.';

const steps = (page: Page) =>
  page.locator('[data-scan-step]').evaluateAll((items) =>
    items.map((li) => ({
      text: li.querySelector('span')!.textContent,
      tag: li.querySelector('[data-scan-step-tag]')!.textContent,
      show: li.querySelector('button')!.textContent,
    })),
  );

test.describe('next steps on a scan result', () => {
  test('a result with gaps lists three steps, each with who can do it', async ({ page }) => {
    await mockScan(page, 'full');
    await startScan(page);
    const block = page.locator('[data-scan-steps]');
    await expect(block.getByRole('heading', { level: 3, name: 'Your next steps' })).toBeVisible();
    expect(await steps(page)).toEqual([
      { text: CONTENT, tag: 'needs a developer', show: 'Show in AI readability' },
      { text: TITLE, tag: 'needs a developer', show: 'Show in Risk & SEO' },
      { text: SPEED, tag: 'needs a developer', show: 'Show in Speed' },
    ]);
    // The line above never says nothing needs attention over a list of steps.
    await expect(page.locator('[data-scan-health]')).toHaveText(
      '4 things to improve. Start with the steps below.',
    );
    // The list sits under the summary and before the tiles.
    expect(
      await block.evaluate(
        (el) =>
          !!(
            el.compareDocumentPosition(document.querySelector('[data-scan-tiles]')!) &
            Node.DOCUMENT_POSITION_FOLLOWING
          ),
      ),
    ).toBe(true);
  });

  test('a step opens its panel and puts focus on the row it is about', async ({ page }) => {
    await mockScan(page, 'full');
    await startScan(page);
    await page.locator('[data-scan-step="content"] button').click();
    await expect(page.locator('[data-scan-tile="airead"]')).toHaveAttribute(
      'aria-expanded',
      'true',
    );
    const row = page.locator('[data-scan-panel="airead"] [data-scan-cat="Content fidelity"]');
    await expect(row).toBeVisible();
    await expect(row).toBeFocused();
    await page.locator('[data-scan-step="seo"] button').click();
    await expect(page.locator('[data-scan-panel="airead"]')).toBeHidden();
    await expect(
      page.locator('[data-scan-panel="risk"] [data-risk-row="seoDefects"]'),
    ).toBeFocused();
    await page.locator('[data-scan-step="speed"] button').click();
    await expect(page.locator('[data-scan-panel="speed"]')).toBeFocused();
  });

  test('the list follows the results as they land and never takes focus', async ({ page }) => {
    await mockScan(page, 'full', { psiDelay: 1500 });
    await startScan(page, false);
    await expect(page.locator('[data-scan-tile="airead"]')).not.toHaveAttribute(
      'data-state',
      'checking',
      { timeout: 15000 },
    );
    // Speed is still on its way: the third place goes to the next finding.
    expect((await steps(page)).map((s) => s.text)).toEqual([CONTENT, TITLE, A11Y]);
    await expect(page.locator('[data-scan-health]')).toHaveText('Checking 1 of 3 areas…');
    await page.locator('#scan-lead-email').focus();
    await expect(page.locator('[data-scan-step="speed"]')).toBeVisible({ timeout: 15000 });
    expect((await steps(page)).map((s) => s.text)).toEqual([CONTENT, TITLE, SPEED]);
    await expect(page.locator('#scan-lead-email')).toBeFocused();
    await expect(page.locator('[data-scan-live]')).not.toContainText('next steps');
  });

  test('a hosted result keeps its steps and shortens the notice', async ({ page }) => {
    await mockScan(page, 'hosted');
    await startScan(page);
    const list = await steps(page);
    expect(list.map((s) => s.text)).toEqual([CONTENT, TITLE, SPEED]);
    expect(list.some((s) => s.tag === 'needs server access')).toBe(false);
    await expect(page.locator('[data-scan-fit-text]')).toHaveText(
      'This site runs on Wix, a hosted platform. mod_pagespeed installs on a web server you run yourself, so it cannot be used for this site. The findings below still apply.',
    );
    await expect(page.locator('[data-scan-fit-links]')).toHaveText(
      'Where to change this in Wix: Wix help: page SEO settings, Wix help: robots.txt.',
    );
    // Only the general question is offered, so no status note is shown.
    await expect(page.locator('[data-scan-topics-note]')).toBeHidden();
  });

  test('a clean result says nothing is urgent and keeps the areas sentence', async ({ page }) => {
    await mockScan(page, 'clean', { speed: 'good' });
    await startScan(page);
    await expect(page.locator('[data-scan-step]')).toHaveCount(0);
    await expect(page.locator('[data-scan-steps-none]')).toHaveText(
      'Nothing urgent on this page. Open a panel for the details.',
    );
    await expect(page.locator('[data-scan-health]')).toHaveText(
      'None of the 3 areas checked needs attention.',
    );
  });

  test('the topics carry plain labels and a note about what is not available yet', async ({
    page,
  }) => {
    await mockScan(page, 'full');
    await startScan(page);
    await expect(page.locator('[data-scan-chip="tollbooth"]')).toHaveText(
      'Deciding which AI crawlers may read the site (experimental) (flagged by your scan)',
    );
    await expect(page.locator('[data-scan-chip="agents"]')).toHaveText(
      'A question about this result',
    );
    await expect(page.locator('[data-scan-topics-note]')).toHaveText(
      'Topics marked planned or not offered yet are not available today; we will tell you where they stand.',
    );
  });
});

test.describe('the wait', () => {
  test('counts the seconds in text, also with reduced motion, outside the live region', async ({
    page,
  }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await mockScan(page, 'full', { scanDelay: 2600, psiDelay: 2600 });
    await startScan(page, false);
    const elapsed = page.locator('[data-scan-elapsed]');
    await expect(elapsed).toHaveText('0 s');
    await expect(elapsed).toHaveText('2 s', { timeout: 5000 });
    expect(await elapsed.evaluate((el) => !!el.closest('[aria-live], [role="status"]'))).toBe(
      false,
    );
    await expect(page.locator('[data-scan-live]')).not.toContainText(/\d s$/);
    // The count ends with the wait.
    await expect(elapsed).toHaveText('', { timeout: 15000 });
  });
});

test.describe('the result on a phone', () => {
  test('the quick-message button never covers a tile’s details control at 390x844', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockScan(page, 'full');
    await startScan(page);
    const launcher = page.locator('[data-qm-launcher]');
    await expect(launcher).toBeVisible();
    for (const id of ['speed', 'airead', 'risk']) {
      const toggle = page.locator(`[data-scan-tile="${id}"] .scan-toggle-closed`);
      // Bring the control level with the fixed button, where it would be covered.
      await toggle.evaluate(
        (el, target) => {
          const box = el.getBoundingClientRect();
          const to = (target as Element).getBoundingClientRect();
          window.scrollBy({
            top: box.top + box.height / 2 - (to.top + to.height / 2),
            behavior: 'instant',
          });
        },
        await launcher.elementHandle(),
      );
      const a = (await toggle.boundingBox())!;
      const b = (await launcher.boundingBox())!;
      const level = a.y < b.y + b.height && b.y < a.y + a.height;
      // The first tiles may sit too high on the page to scroll level with it.
      if (id === 'risk') expect(level, 'the control is level with the button').toBe(true);
      const apart = a.x + a.width <= b.x || b.x + b.width <= a.x;
      expect(apart, `${id}: the two boxes do not intersect`).toBe(true);
    }
  });
});
