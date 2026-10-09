// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// /docs/configuration/ collapses each directive's detail block behind a native
// <details> whose <summary> holds the directive's own h4 heading, so the page
// keeps every heading, anchor and sentence while most of the document stays
// unrendered until opened. These specs pin the three contracts of that shape:
// the collapsed reference, the deep link that opens a section, and the in-page
// area index links that do the same.
test.describe('/docs/configuration/ collapsed directive reference', () => {
  test('directive sections render as collapsed details with the heading as the summary', async ({
    page,
  }) => {
    await page.goto('/docs/configuration/');
    const details = page.locator('details.directive-details');
    const total = await details.count();
    // The whole reference (318 directives) is wrapped...
    expect(total).toBeGreaterThan(300);
    // ...each summary is the directive's own h4, id and all...
    const headed = await page.locator('details.directive-details > summary > h4[id]').count();
    expect(headed).toBe(total);
    // ...nothing is open on arrival, and a closed section's content is not
    // rendered while its heading is still in the document.
    const allow = page.locator('h4#allow');
    await expect(allow).toHaveCount(1);
    const allowDetails = page.locator('details.directive-details', { has: allow });
    await expect(allowDetails).toHaveCount(1);
    expect(await allowDetails.getAttribute('open')).toBeNull();
    await expect(allowDetails.getByText('Syntax:', { exact: false }).first()).toBeHidden();
  });

  test('a deep link opens the directive section and scrolls to it', async ({ page }) => {
    await page.goto('/docs/configuration/#allow');
    const allow = page.locator('h4#allow');
    await expect(allow).toBeVisible();
    const allowDetails = page.locator('details.directive-details', { has: allow });
    await expect(allowDetails).toHaveAttribute('open', '');
    await expect(allowDetails.getByText('Syntax:', { exact: false }).first()).toBeVisible();
    // The page scrolled to the section, not just opened it. The scroll settles
    // asynchronously (fonts and code highlighting reflow the page in dev), so
    // poll until the heading lands at the top of the viewport.
    await expect
      .poll(async () => (await allow.boundingBox())?.y ?? Number.NaN, { timeout: 10_000 })
      .toBeLessThan(400);
  });

  test('an in-page link to a directive opens its section', async ({ page }) => {
    await page.goto('/docs/configuration/#area-enabling-and-filter-selection');
    // The area's index paragraph links to each directive with a plain anchor.
    await page.locator('a[href="#disablefilters"]').first().click();
    const heading = page.locator('h4#disablefilters');
    await expect(heading).toBeVisible();
    const details = page.locator('details.directive-details', { has: heading });
    await expect(details).toHaveAttribute('open', '');
    await expect(details.getByText('Syntax:', { exact: false }).first()).toBeVisible();
  });
});
