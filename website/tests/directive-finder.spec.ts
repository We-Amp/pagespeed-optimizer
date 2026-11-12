// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';
import { stubUmami, trackedEvents } from './helpers/umami';

// The directive finder on the configuration reference: a labelled field above
// the generated directive list that narrows the list to the directives whose
// name contains the typed text.

const PAGE = '/docs/configuration/';
const field = '#directive-filter';
const status = '#directive-filter-status';

/** How many directive blocks and areas are currently shown. */
const shown = (page: Page) =>
  page.evaluate(() => {
    const visible = (selector: string) =>
      [...document.querySelectorAll<HTMLElement>(selector)].filter((el) => !el.hidden).length;
    return { directives: visible('[data-directive]'), areas: visible('[data-directive-area]') };
  });

/** Distance from the top of the window to the top of the element with `id`. */
const topOf = (page: Page, id: string) =>
  page.evaluate((id) => document.getElementById(id)!.getBoundingClientRect().top, id);

/** The lowest edge of what is pinned to the top: header, then platform tabs. */
const pinnedBottom = (page: Page) =>
  page.evaluate(() =>
    Math.max(
      document.querySelector('body > header')!.getBoundingClientRect().bottom,
      document.getElementById('platform-selector')?.getBoundingClientRect().bottom ?? 0,
    ),
  );

async function expectLandedOn(page: Page, id: string) {
  await expect
    .poll(async () => {
      const top = await topOf(page, id);
      return top >= (await pinnedBottom(page)) && top < 400;
    })
    .toBe(true);
}

test.describe('Directive finder', () => {
  test.beforeEach(async ({ page }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
  });

  test('every directive is one block, grouped by area, with the field above', async ({ page }) => {
    await page.goto(PAGE);
    const counts = await page.evaluate(() => {
      const reference = document.querySelector('[data-directive-reference]')!;
      const blocks = [...reference.querySelectorAll('[data-directive]')];
      return {
        headings: reference.querySelectorAll('h4[id]').length,
        blocks: blocks.length,
        startWithHeading: blocks.filter((b) => b.firstElementChild?.matches('h4[id]')).length,
        areas: reference.querySelectorAll('[data-directive-area] > h3[id]:first-child').length,
        areaHeadings: reference.querySelectorAll('h3[id]').length,
      };
    });
    expect(counts.blocks).toBeGreaterThan(300);
    expect(counts.blocks).toBe(counts.headings);
    expect(counts.startWithHeading).toBe(counts.blocks);
    expect(counts.areas).toBe(counts.areaHeadings);
    expect(counts.areas).toBeGreaterThan(10);

    const input = page.getByLabel('Find a directive');
    await expect(input).toBeVisible();
    await expect(input).toHaveAttribute('id', 'directive-filter');
    await expect(page.locator(status)).toHaveAttribute('role', 'status');
    await expect(page.locator(status)).toHaveText('');
    expect(await shown(page)).toEqual({ directives: counts.blocks, areas: counts.areas });
  });

  test('typing narrows the reference and the status line reports the result', async ({ page }) => {
    await page.goto(PAGE);
    const input = page.locator(field);
    const all = await shown(page);

    await input.fill('FileCachePath');
    await expect(page.locator(status)).toHaveText('1 directive matches');
    expect(await shown(page)).toEqual({ directives: 1, areas: 1 });
    await expect(page.locator('#filecachepath')).toBeVisible();
    await expect(page.locator('#allow')).toBeHidden();

    // Case does not matter, and a name can be typed the way a configuration
    // file writes it.
    for (const typed of [
      'filecachepath',
      'ModPagespeedFileCachePath /var/cache',
      'pagespeed FileCachePath /var/cache;',
    ]) {
      await input.fill(typed);
      await expect(page.locator(status)).toHaveText('1 directive matches');
      await expect(page.locator('#filecachepath')).toBeVisible();
    }
    // The line that switches the module finds the module's own entry, not
    // every directive with "on" in its name.
    for (const typed of ['pagespeed on;', 'ModPagespeed on', 'pagespeed standby;']) {
      await input.fill(typed);
      await expect(page.locator(status)).toHaveText('1 directive matches');
      await expect(page.locator('#modpagespeed')).toBeVisible();
    }

    await input.fill('FileCache');
    await expect(page.locator(status)).toHaveText(/^\d+ directives match$/);
    const some = await shown(page);
    expect(some.directives).toBeGreaterThan(1);
    expect(some.directives).toBeLessThan(all.directives);
    expect(await page.locator(status).textContent()).toBe(`${some.directives} directives match`);
    // The per-area lists of every directive step aside while a filter is on.
    await expect(page.locator('[data-directive-index]').first()).toBeHidden();

    await input.fill('no-such-directive');
    await expect(page.locator(status)).toHaveText(
      'No directive matches. Clear the filter to see all.',
    );
    expect(await shown(page)).toEqual({ directives: 0, areas: 0 });

    await input.fill('');
    await expect(page.locator(status)).toHaveText('');
    expect(await shown(page)).toEqual(all);
    await expect(page.locator('[data-directive-index]').first()).toBeVisible();
  });

  test('works from the keyboard, and Escape clears the filter', async ({ page }) => {
    await page.goto(PAGE);
    const all = await shown(page);
    // Tab from the link just before the field lands in it.
    await page.locator('[data-directive-reference] > p a').last().focus();
    await page.keyboard.press('Tab');
    await expect(page.locator(field)).toBeFocused();
    await page.keyboard.type('MaxSegmentLength');
    await expect(page.locator(status)).toHaveText('1 directive matches');

    await page.keyboard.press('Escape');
    await expect(page.locator(field)).toHaveValue('');
    await expect(page.locator(field)).toBeFocused();
    await expect(page.locator(status)).toHaveText('');
    expect(await shown(page)).toEqual(all);
  });

  test('a link to a hidden directive or area clears the filter and lands on it', async ({
    page,
  }) => {
    await page.goto(PAGE);
    const input = page.locator(field);
    const all = await shown(page);

    // A link on the page.
    await input.fill('FileCachePath');
    await expect(page.locator(status)).toHaveText('1 directive matches');
    await page.locator('[data-directive-reference] > p a[href="#area-images"]').click();
    await expect(input).toHaveValue('');
    expect(await shown(page)).toEqual(all);
    await expectLandedOn(page, 'area-images');

    // The address bar (and back / forward): the hash changes on its own.
    await input.fill('FileCachePath');
    await expect(page.locator(status)).toHaveText('1 directive matches');
    await page.evaluate(() => {
      location.hash = '#lowercasehtmlnames';
    });
    await expect(input).toHaveValue('');
    await expect(page.locator(status)).toHaveText('');
    await expectLandedOn(page, 'lowercasehtmlnames');

    // An anchor inside a hidden block, not the block's own heading.
    await input.fill('FileCachePath');
    await page.evaluate(() => {
      location.hash = '#pagespeed_disallow';
    });
    await expect(input).toHaveValue('');
    await expectLandedOn(page, 'pagespeed_disallow');
  });

  test('a hash change far below the field lands under the pinned bars', async ({ page }) => {
    // Clearing the filter makes the page tens of thousands of pixels taller;
    // the scroll must use the new layout, not the short filtered one.
    for (const [query, id] of [
      ['Allow', 'useexperimentaljsminifier'],
      ['cache', 'rslcapissuer'],
    ]) {
      await page.goto(PAGE);
      const input = page.locator(field);
      await input.scrollIntoViewIfNeeded();
      await input.fill(query);
      await expect(page.locator(status)).toHaveText(/match/);
      await page.evaluate((id) => {
        location.hash = '#' + id;
      }, id);
      await expect(input).toHaveValue('');
      await expectLandedOn(page, id);
    }
  });

  test('a link to a directive that is still shown keeps the filter', async ({ page }) => {
    await page.goto(PAGE);
    const input = page.locator(field);
    await input.fill('FileCache');
    await expect(page.locator(status)).toHaveText(/directives match$/);
    await page.evaluate(() => {
      location.hash = '#filecachepath';
    });
    await expectLandedOn(page, 'filecachepath');
    await expect(input).toHaveValue('FileCache');
    // A target outside the reference never clears it either.
    await page.evaluate(() => {
      location.hash = '#virtual-hosts';
    });
    await expectLandedOn(page, 'virtual-hosts');
    await expect(input).toHaveValue('FileCache');
  });

  // A link from another page arrives at its directive, however far down.
  test('a deep link into the reference lands on its directive', async ({ page }) => {
    await page.goto(PAGE);
    const ids = await page.evaluate(() => {
      const all = [...document.querySelectorAll('[data-directive] > h4[id]')].map((h) => h.id);
      return [all[0], all[Math.floor(all.length / 2)], all[all.length - 1]];
    });
    for (const id of [...ids, 'area-deprecated-and-ignored', 'optimization-threads']) {
      await page.goto('about:blank');
      await page.goto(`${PAGE}#${id}`);
      await expectLandedOn(page, id);
    }
  });

  test('the first use is counted once, without the typed text', async ({ page }) => {
    await stubUmami(page);
    await page.goto(PAGE);
    const input = page.locator(field);
    await input.fill('Cache');
    await input.fill('CachePath');
    await input.fill('');
    await input.fill('Image');
    await expect(page.locator(status)).toHaveText(/directives match$/);
    const events = (await trackedEvents(page)).filter((e) => e.name === 'docs_directive_filter');
    expect(events).toEqual([{ name: 'docs_directive_filter' }]);
  });

  test('at phone width the field fits and nothing scrolls sideways', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto(PAGE);
    const input = page.locator(field);
    await input.scrollIntoViewIfNeeded();
    const box = (await input.boundingBox())!;
    expect(box.x).toBeGreaterThanOrEqual(0);
    expect(box.x + box.width).toBeLessThanOrEqual(390);
    expect(box.height).toBeGreaterThanOrEqual(44);
    for (const typed of ['', 'Image']) {
      await input.fill(typed);
      expect(
        await page.evaluate(() => document.documentElement.scrollWidth),
        `page width with "${typed}"`,
      ).toBe(390);
    }
  });

  // The reference with a filter on, at desktop and phone width: the field has
  // a name, the status line is a live region, nothing shown fails WCAG 2.2 AA.
  for (const [width, height] of [
    [1440, 900],
    [390, 844],
  ]) {
    test(`axe-core WCAG 2.2 AA scan with a filter on, at ${width}px`, async ({ page }) => {
      test.setTimeout(90_000);
      await page.setViewportSize({ width, height });
      await page.goto(PAGE, { waitUntil: 'networkidle' });
      const input = page.locator(field);
      for (const typed of ['Image', 'no-such-directive']) {
        await input.fill(typed);
        await expect(page.locator(status)).not.toHaveText('');
        const results = await new AxeBuilder({ page })
          .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
          .analyze();
        expect(
          results.violations.map((v) => `${v.id}: ${v.nodes.map((n) => n.target).join(', ')}`),
          `with "${typed}"`,
        ).toEqual([]);
      }
    });
  }

  test('without JavaScript the field is not shown and every directive is', async ({ browser }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    await page.goto(PAGE);
    await expect(page.locator(field)).toBeHidden();
    await expect(page.locator('#filecachepath')).toBeAttached();
    expect(await page.locator('[data-directive][hidden]').count()).toBe(0);
    await context.close();
  });
});
