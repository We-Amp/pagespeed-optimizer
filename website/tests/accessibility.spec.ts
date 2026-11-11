// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

const PAGES_FOR_AXE_SCAN = [
  '/',
  '/features/',
  '/pricing/',
  '/support/',
  '/hosting-partners/',
  '/platform/',
  '/demo/',
  '/docs/',
  '/license/',
  '/contact/',
  '/calculator/',
  '/blog/',
  '/security/',
  '/terms/',
  '/ai-readability/',
  // /privacy/ points at the notice on we-amp.com; it is covered by its own
  // suite in terms-privacy.spec.ts
];

// Scanned at phone width as well: narrow layouts stack and wrap links, which
// is where small or crowded targets (WCAG 2.5.8) and overflowing regions show.
const PAGES_FOR_PHONE_AXE_SCAN = [
  '/docs/filters/',
  '/download/',
  '/docs/getting-started/',
  '/examples/',
  '/alternatives/',
  '/blog/avif-vs-webp-2026/',
  '/error/404/',
];

test.describe('Accessibility', () => {
  test('skip-to-content link is first focusable element', async ({ page }) => {
    await page.goto('/');
    // A cold dev server reloads the page once, right after the first load, when
    // it has optimized its dependencies. A Tab pressed into the document that
    // is then replaced leaves nothing focused in the new one. So mark the
    // loaded document, press Tab, and read the focus together with the mark: a
    // missing mark means the key press did not land in a fully loaded document
    // that is still there, and the check is repeated on a fresh load.
    type Probe = { __tabProbe?: boolean };
    let href: string | null | undefined;
    for (let attempt = 0; attempt < 3 && href === undefined; attempt++) {
      if (attempt > 0) await page.reload();
      await page
        .evaluate(() => {
          if (document.readyState === 'complete') (window as unknown as Probe).__tabProbe = true;
        })
        .catch(() => undefined);
      // Tab to first focusable element
      await page.keyboard.press('Tab');
      href = await page
        .evaluate(() =>
          (window as unknown as Probe).__tabProbe
            ? (document.activeElement?.getAttribute('href') ?? null)
            : undefined,
        )
        .catch(() => undefined);
    }
    expect(href).toBe('#main-content');
  });

  test('all images have alt text', async ({ page }) => {
    await page.goto('/');
    const images = page.locator('img');
    const count = await images.count();
    for (let i = 0; i < count; i++) {
      const img = images.nth(i);
      const alt = await img.getAttribute('alt');
      // alt="" is valid for decorative images; only fail if alt is missing entirely
      expect(alt !== null, `Image ${i} is missing alt attribute`).toBeTruthy();
    }
  });

  test('calculator inputs have associated labels', async ({ page }) => {
    await page.goto('/calculator/');
    // per-site licensing renamed the "servers" input to "sites".
    const inputIds = ['pageviews', 'pageweight', 'bwcost', 'sites'];
    for (const id of inputIds) {
      const label = page.locator(`label[for="${id}"]`);
      await expect(label, `Label for ${id} should exist`).toBeAttached();
    }
  });

  test('heading hierarchy is correct (no skips)', async ({ page }) => {
    await page.goto('/');
    const headings = page.locator('main h1, main h2, main h3, main h4, main h5, main h6');
    const count = await headings.count();
    let lastLevel = 0;

    for (let i = 0; i < count; i++) {
      const tag = await headings.nth(i).evaluate((el) => el.tagName.toLowerCase());
      const level = parseInt(tag.charAt(1));
      // Should not skip more than 1 level (e.g., h1 -> h3 is bad)
      if (lastLevel > 0) {
        expect(level <= lastLevel + 1, `Heading skip: h${lastLevel} -> h${level}`).toBeTruthy();
      }
      lastLevel = level;
    }
  });

  test('pricing toggle has appropriate ARIA', async ({ page }) => {
    await page.goto('/pricing/');
    const toggle = page.locator('#billing-toggle');
    // Billing toggle only exists when live pricing is enabled (prices are
    // currently TBD). Skip gracefully when the element is absent.
    if ((await toggle.count()) === 0) {
      test.skip();
      return;
    }
    await expect(toggle).toHaveAttribute('role', 'switch');
    await expect(toggle).toHaveAttribute('aria-checked', /true|false/);
    await expect(toggle).toHaveAttribute('aria-label', /billing/i);
  });

  test('mobile menu button has aria-label', async ({ page }) => {
    await page.goto('/');
    const btn = page.locator('#mobile-menu-btn');
    await expect(btn).toHaveAttribute('aria-label', /menu/i);
  });

  test('page has lang attribute on <html>', async ({ page }) => {
    await page.goto('/');
    const html = page.locator('html');
    await expect(html).toHaveAttribute('lang', 'en');
  });

  test('links have discernible text', async ({ page }) => {
    await page.goto('/');
    const links = page.locator('a:not([aria-hidden="true"])');
    const count = await links.count();
    for (let i = 0; i < count; i++) {
      const link = links.nth(i);
      const text = await link.textContent();
      const ariaLabel = await link.getAttribute('aria-label');
      const title = await link.getAttribute('title');
      const hasText = (text && text.trim().length > 0) || ariaLabel || title;
      // Skip SVG-only decorative links
      const hasImg = (await link.locator('img, svg').count()) > 0;
      if (!hasImg) {
        expect(
          hasText,
          `Link ${i} (href=${await link.getAttribute('href')}) has no discernible text`,
        ).toBeTruthy();
      }
    }
  });

  test('main navigation has aria-label', async ({ page }) => {
    await page.goto('/');
    const nav = page.locator('header nav');
    await expect(nav).toHaveAttribute('aria-label', /navigation/i);
  });

  // WCAG 2.4.11 Focus Not Obscured: walking back up a page with Shift+Tab
  // scrolls each stop into view at the top edge, where the sticky header is
  // and, on docs pages that have one, the platform selector pinned under it.
  // Every stop must come to rest below both, not behind them.
  // /docs/cache-modes/ is a page with the platform selector.
  for (const pagePath of ['/', '/pricing/', '/docs/getting-started/', '/docs/cache-modes/']) {
    test(`Shift+Tab never puts focus behind the sticky header: ${pagePath}`, async ({ page }) => {
      test.setTimeout(90_000);
      // Reduced motion makes focus scrolling instant, so each stop can be
      // measured right after the key press.
      await page.emulateMedia({ reducedMotion: 'reduce' });
      await page.goto(pagePath, { waitUntil: 'networkidle' });
      await page.evaluate(() => {
        window.scrollTo(0, document.documentElement.scrollHeight);
        const links = document.querySelectorAll<HTMLElement>('footer a[href]');
        links[links.length - 1].focus();
      });

      const obscured: string[] = [];
      let stops = 0;
      for (; stops < 600; stops++) {
        await page.keyboard.press('Shift+Tab');
        const stop = await page.evaluate(() => {
          const el = document.activeElement as HTMLElement | null;
          const header = document.querySelector('body > header');
          if (!el || el === document.body || !header) return { done: true };
          if (header.contains(el)) return { done: true };
          const rect = el.getBoundingClientRect();
          const headerBottom = header.getBoundingClientRect().bottom;
          // The header only covers anything while it is pinned to the top.
          let edge = headerBottom > 0 ? headerBottom : 0;
          let what = 'header';
          // The platform selector covers its own column while it is pinned
          // under the header.
          const selector = document.getElementById('platform-selector');
          if (selector && !selector.contains(el)) {
            const bar = selector.getBoundingClientRect();
            const pinned =
              getComputedStyle(selector).position === 'sticky' && bar.top <= headerBottom + 1;
            if (pinned && rect.right > bar.left && rect.left < bar.right && bar.bottom > edge) {
              edge = bar.bottom;
              what = 'platform selector';
            }
          }
          const name = (el.getAttribute('aria-label') ?? el.textContent ?? '')
            .trim()
            .replace(/\s+/g, ' ')
            .slice(0, 50);
          return {
            done: false,
            covered: rect.top < edge - 1,
            label: `<${el.tagName.toLowerCase()}> "${name}" top ${Math.round(rect.top)} < ${what} bottom ${Math.round(edge)}`,
          };
        });
        if (stop.done) break;
        if (stop.covered) obscured.push(stop.label!);
      }

      expect(stops, 'the walk reaches the header').toBeLessThan(600);
      expect(stops, 'the walk passes through the page content').toBeGreaterThan(5);
      expect(obscured, `focus stops behind a pinned bar:\n${obscured.join('\n')}`).toHaveLength(0);
    });
  }

  // Forced colors (Windows contrast themes) drop author backgrounds and
  // shadows. States that are a fill in the normal theme must still be visible
  // (WCAG 1.4.1 Use of Color, 1.4.11 Non-text Contrast).
  test.describe('forced colors', () => {
    test.beforeEach(async ({ page }) => {
      await page.emulateMedia({ forcedColors: 'active', reducedMotion: 'reduce' });
    });

    test('the primary button has a visible border', async ({ page }) => {
      await page.goto('/');
      const border = await page
        .locator('main .btn-primary')
        .first()
        .evaluate((el) => {
          const cs = getComputedStyle(el);
          return {
            width: parseFloat(cs.borderTopWidth),
            style: cs.borderTopStyle,
            color: cs.borderTopColor,
            ground: getComputedStyle(document.body).backgroundColor,
          };
        });
      expect(border.width).toBeGreaterThanOrEqual(1);
      expect(border.style).toBe('solid');
      expect(border.color).not.toBe(border.ground);
    });

    test('the selected tab is filled, and its focus ring stands out from the fill', async ({
      page,
    }) => {
      await page.goto('/');
      const selected = page.locator('.qs-tab[aria-pressed="true"]').first();
      const other = page.locator('.qs-tab[aria-pressed="false"]').first();
      await selected.scrollIntoViewIfNeeded();
      const read = (el: Element) => {
        const cs = getComputedStyle(el);
        return {
          // 'none' keeps the element's own fill; 'auto' lets the mode drop it.
          adjust: cs.getPropertyValue('forced-color-adjust'),
          background: cs.backgroundColor,
          outlineStyle: cs.outlineStyle,
          outlineColor: cs.outlineColor,
        };
      };
      const unfocused = await selected.evaluate(read);
      const unselected = await other.evaluate(read);
      expect(unfocused.adjust, 'the selected tab keeps its fill').toBe('none');
      expect(unselected.adjust, 'an unselected tab has no fill').toBe('auto');
      expect(unfocused.background, 'selected and unselected tabs differ').not.toBe(
        unselected.background,
      );

      // Reach the tab with the keyboard so the focus ring (:focus-visible) shows.
      await other.focus();
      await page.keyboard.press('Shift+Tab');
      await expect(selected).toBeFocused();
      const focused = await selected.evaluate(read);
      expect(unfocused.outlineStyle).toBe('none');
      expect(focused.outlineStyle).toBe('solid');
      expect(focused.outlineColor, 'the ring is not the colour of the fill').not.toBe(
        focused.background,
      );
    });

    test('the current page in the docs sidebar is marked by a border', async ({ page }) => {
      await page.goto('/docs/getting-started/');
      const nav = page.locator('nav[aria-label="Documentation"]');
      const width = (el: Element) => parseFloat(getComputedStyle(el).borderInlineStartWidth);
      const current = await nav.locator('a[aria-current="page"]').evaluate(width);
      const sibling = await nav.locator('a:not([aria-current])').first().evaluate(width);
      expect(current).toBeGreaterThanOrEqual(3);
      expect(sibling).toBe(0);
    });
  });

  // WCAG 2.2 AA axe-core scan on all buyer-facing pages.
  for (const pagePath of PAGES_FOR_AXE_SCAN) {
    test(`axe-core WCAG 2.2 AA scan: ${pagePath}`, async ({ page }) => {
      // Scan the resting colours. The telemetry strip fades a value in once
      // when the live reading arrives; axe would otherwise measure contrast
      // mid-fade, depending on timing. Reduced motion removes that fade, so
      // the scan measures the resting state.
      await page.emulateMedia({ reducedMotion: 'reduce' });

      // The dev server can reload a page once after its first visit
      // (dependency optimisation). A reload that lands while axe is running
      // destroys the execution context it works in (or leaves the scan
      // waiting on a document that is gone). So load the page, wait until no
      // further load has happened for a moment, and scan; if the context is
      // still destroyed, scan again from a fresh load.
      test.setTimeout(90_000);
      let loads = 0;
      page.on('load', () => {
        loads += 1;
      });
      const scan = async () => {
        await page.goto(pagePath, { waitUntil: 'networkidle' });
        for (let quiet = 0; quiet < 3; quiet++) {
          const seen = loads;
          await page.waitForTimeout(500);
          if (loads !== seen) quiet = -1;
        }

        const builder = new AxeBuilder({ page }).withTags([
          'wcag2a',
          'wcag2aa',
          'wcag21a',
          'wcag21aa',
          'wcag22aa',
        ]);

        // Demo page embeds third-party demo sites in iframes with intentionally
        // non-optimized assets. Exclude iframe content and scrollable demo panels.
        if (pagePath === '/demo/') {
          builder.exclude('iframe').exclude('[role="tabpanel"]').exclude('pre');
        }

        return builder.analyze();
      };
      let results: Awaited<ReturnType<typeof scan>>;
      try {
        results = await scan();
      } catch (err) {
        if (!String(err).includes('Execution context was destroyed')) throw err;
        results = await scan();
      }

      const violations = results.violations.filter(
        (v) => v.impact === 'critical' || v.impact === 'serious',
      );

      expect(
        violations,
        `${pagePath} has ${violations.length} serious/critical a11y violations:\n` +
          violations
            .map((v) => `  [${v.impact}] ${v.id}: ${v.description} (${v.nodes.length} occurrences)`)
            .join('\n'),
      ).toHaveLength(0);
    });
  }

  test.describe('phone width', () => {
    test.use({ viewport: { width: 390, height: 844 } });

    for (const pagePath of PAGES_FOR_PHONE_AXE_SCAN) {
      test(`axe-core WCAG 2.2 AA scan at 390px: ${pagePath}`, async ({ page }) => {
        // Same settling as the scans above: resting colours, and a scan that
        // is repeated when a dev-server reload destroys its context.
        await page.emulateMedia({ reducedMotion: 'reduce' });
        test.setTimeout(90_000);
        let loads = 0;
        page.on('load', () => {
          loads += 1;
        });
        const scan = async () => {
          await page.goto(pagePath, { waitUntil: 'networkidle' });
          for (let quiet = 0; quiet < 3; quiet++) {
            const seen = loads;
            await page.waitForTimeout(500);
            if (loads !== seen) quiet = -1;
          }
          return new AxeBuilder({ page })
            .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
            .analyze();
        };
        let results: Awaited<ReturnType<typeof scan>>;
        try {
          results = await scan();
        } catch (err) {
          if (!String(err).includes('Execution context was destroyed')) throw err;
          results = await scan();
        }

        const violations = results.violations.filter(
          (v) => v.impact === 'critical' || v.impact === 'serious',
        );

        expect(
          violations,
          `${pagePath} has ${violations.length} serious/critical a11y violations at 390px:\n` +
            violations
              .map(
                (v) => `  [${v.impact}] ${v.id}: ${v.description} (${v.nodes.length} occurrences)`,
              )
              .join('\n'),
        ).toHaveLength(0);
      });
    }
  });
});
