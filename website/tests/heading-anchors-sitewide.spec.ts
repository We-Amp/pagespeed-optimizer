// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Sitewide guard for heading permalinks. Docs and how-it-works headings carry
// a "#" link (rendered at build time by src/lib/rehype-heading-anchors.mjs).
// A "#" that became part of the heading's text read "Roll back#" in a copy,
// in reader mode, to crawlers and in a Markdown copy of the page. Every page
// of every type is loaded and every heading checked:
//
//   - its text (textContent, innerText, a selection of it) never ends in "#";
//   - its accessible name never ends in "#" and never holds the link's label;
//   - every permalink holds no text of its own (the "#" is CSS generated);
//   - on a screen that can hover, a permalink is invisible until its heading
//     is hovered or the link is focused;
//   - on a touch screen, a visible permalink keeps a gap to the heading text.

import { test, expect, devices, type Page } from '@playwright/test';
import { readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { axQuery } from './helpers/ax-tree';

const SRC = fileURLToPath(new URL('../src', import.meta.url));

/** Static routes under src/pages, with the same exclusions as links.spec. */
function staticRoutes(): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(`${SRC}/pages`, { recursive: true }) as string[]) {
    const rel = entry.replace(/\\/g, '/');
    if (!rel.endsWith('.astro') || rel.includes('[')) continue;
    if (rel === '404.astro' || rel === '500.astro') continue;
    if (rel.startsWith('1.0/') || rel.startsWith('1.1/') || rel.startsWith('api/')) continue;
    if (rel === 'index.astro') out.push('/');
    else if (rel.endsWith('/index.astro')) out.push(`/${rel.slice(0, -'/index.astro'.length)}/`);
    else out.push(`/${rel.slice(0, -'.astro'.length)}/`);
  }
  return out.sort();
}

/** Routes of a content collection: one page per Markdown file. */
function collectionRoutes(dir: string, prefix: string): string[] {
  return readdirSync(`${SRC}/content/${dir}`)
    .filter((f) => /\.mdx?$/.test(f))
    .map((f) => `${prefix}${f.replace(/\.mdx?$/, '')}/`)
    .sort();
}

const DOCS = collectionRoutes('docs', '/docs/');
const HOW = collectionRoutes('how-it-works', '/how-it-works/');
// Blog drafts have no page; the check below skips a 404 only for the blog.
const BLOG = collectionRoutes('blog', '/blog/');
const STATIC = staticRoutes();

/** Pages whose headings carry permalinks. */
const WITH_ANCHORS = new Set([...DOCS, ...HOW]);

interface HeadingFacts {
  tag: string;
  textContent: string;
  innerText: string;
  selection: string;
}

/** Text of every visible heading, read the way a reader, a copy or a crawler reads it. */
async function headingTexts(page: Page): Promise<HeadingFacts[]> {
  return page.locator(':is(h1, h2, h3, h4, h5, h6)').evaluateAll((els) =>
    els
      .filter((el) => el.getClientRects().length > 0)
      .map((el) => {
        const sel = window.getSelection();
        let selection = '';
        if (sel) {
          const range = document.createRange();
          range.selectNodeContents(el);
          sel.removeAllRanges();
          sel.addRange(range);
          selection = sel.toString();
          sel.removeAllRanges();
        }
        return {
          tag: el.tagName.toLowerCase(),
          textContent: (el.textContent ?? '').trim(),
          innerText: (el as HTMLElement).innerText.trim(),
          selection: selection.trim(),
        };
      }),
  );
}

async function load(page: Page, path: string): Promise<boolean> {
  const res = await page.goto(path, { waitUntil: 'load' });
  if (res?.status() === 404 && path.startsWith('/blog/')) return false; // a draft
  expect(res?.status(), `${path} status`).toBe(200);
  return true;
}

const ALL = [...STATIC, ...DOCS, ...HOW, ...BLOG];

// A device descriptor's browser type cannot change inside a describe group.
const device = (name: string) => {
  const { defaultBrowserType: _ignored, ...rest } = devices[name];
  return rest;
};

test.describe('heading permalinks, every page', () => {
  test.use(device('Desktop Chrome'));

  for (const path of ALL) {
    test(`${path}`, async ({ page }) => {
      if (!(await load(page, path))) return;

      // (d) The text a reader, a copy or a crawler takes never ends in "#".
      for (const h of await headingTexts(page)) {
        for (const [how, value] of Object.entries({
          textContent: h.textContent,
          innerText: h.innerText,
          selection: h.selection,
        })) {
          expect(value, `${h.tag} ${how}: "${value}"`).not.toMatch(/#\s*$/);
        }
      }

      // (c) Accessible names: never end in "#", never carry the link's label.
      const names = (await axQuery(page, 'body', 'heading')).map((h) => h.name);
      for (const n of names) {
        expect(n, `heading name "${n}"`).not.toMatch(/#\s*$/);
        expect(n, `heading name "${n}"`).not.toContain('Link to “');
      }

      const anchors = page.locator(':is(h1, h2, h3, h4, h5, h6) > .heading-anchor');
      const count = await anchors.count();
      if (WITH_ANCHORS.has(path)) {
        // Every docs and how-it-works page with a section heading has them.
        const sections = await page.locator('main .prose :is(h2, h3, h4)[id]').count();
        if (sections > 0) expect(count, 'permalinks on section headings').toBeGreaterThan(0);
      }
      if (count === 0) return;

      // Permalinks hold no text; the "#" is generated content.
      const facts = await anchors.evaluateAll((els) =>
        els.map((a) => ({
          text: a.textContent ?? '',
          after: getComputedStyle(a, '::after').content,
          opacity: getComputedStyle(a).opacity,
          href: a.getAttribute('href'),
          id: a.parentElement?.id,
          label: a.getAttribute('aria-label') ?? '',
        })),
      );
      for (const f of facts) {
        expect(f.text, `permalink text in #${f.id}`).toBe('');
        expect(f.after, `permalink glyph in #${f.id}`).toMatch(/^"#"/);
        expect(f.href).toBe(`#${f.id}`);
        expect(f.label).toMatch(/^Link to /);
        // (a) Hidden on a screen that can hover until hovered or focused.
        expect(f.opacity, `resting opacity in #${f.id}`).toBe('0');
      }

      // (a) Hovering the heading reveals its permalink; focus does too.
      const first = anchors.first();
      if (await first.isVisible()) {
        await first.locator('xpath=..').hover();
        await expect(first).toHaveCSS('opacity', '1');
        await page.mouse.move(0, 0);
        await expect(first).toHaveCSS('opacity', '0');
      }
    });
  }
});

test.describe('heading permalinks on a touch screen', () => {
  test.use(device('Pixel 5'));

  for (const path of [...DOCS, ...HOW]) {
    test(`${path}`, async ({ page }) => {
      await load(page, path);
      expect(await page.evaluate(() => matchMedia('(hover: none)').matches)).toBe(true);

      // (b) A visible permalink is faint and never abuts the heading text: the
      // drawn "#" starts at least 4px after the last line of the text.
      const gaps = await page.locator('main :is(h2, h3, h4) > .heading-anchor').evaluateAll((els) =>
        els
          .filter((a) => a.getClientRects().length > 0)
          .map((a) => {
            const cs = getComputedStyle(a);
            const prev = a.previousElementSibling;
            const r = document.createRange();
            if (prev) r.selectNodeContents(prev);
            const lines = prev ? Array.from(r.getClientRects()) : [];
            const last = lines[lines.length - 1];
            const box = a.getClientRects()[0];
            const glyphLeft = box.left + parseFloat(cs.paddingLeft);
            // Same line only (vertical centres within half a text line); a
            // permalink wrapped to a line of its own has no neighbour.
            const mid = (r: DOMRect) => r.top + r.height / 2;
            const sameLine = last && Math.abs(mid(last) - mid(box)) < last.height / 2;
            return {
              id: a.parentElement?.id,
              opacity: cs.opacity,
              gap: sameLine ? glyphLeft - last.right : null,
            };
          }),
      );
      for (const g of gaps) {
        expect(g.opacity, `touch opacity in #${g.id}`).toBe('0.4');
        if (g.gap !== null) expect(g.gap, `gap before "#" in #${g.id}`).toBeGreaterThanOrEqual(4);
      }
    });
  }
});
