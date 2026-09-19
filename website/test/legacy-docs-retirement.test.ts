// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// legacy-docs-retirement.test.ts — the docs-1.1 collection narrowed to one
// entry (release-notes), and the /1.1/docs/[slug].astro route narrowed to
// match. This is the source-side (always-on) half of the retirement gate:
// it asserts the collection on disk holds exactly the kept slug, and — when
// a build exists — that the retired paths are NOT in the built output while
// the kept page IS, with its archive line, a self-canonical, and no robots
// noindex meta. The built-HTML half skips itself (never throws) when
// dist/client/1.1/docs does not exist, the same convention anchor-gate.test.ts
// uses; run `npm run build` first to exercise it.

import { readdirSync, readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DOCS11_SRC_DIR = path.join(WEBSITE_ROOT, 'src/content/docs-1.1');
const DIST_1_1_DOCS_DIR = path.join(WEBSITE_ROOT, 'dist/client/1.1/docs');

const KEPT_SLUG = 'release-notes';

// The 26 collection entries retired in this pass (the hub route is a
// separate check below — it was never a docs-1.1 collection entry).
const RETIRED_SLUGS = [
  'admin-console',
  'aspnet-sidecar',
  'caching-url-filters',
  'caching',
  'configuration',
  'cpanel',
  'css-filters',
  'directive-index',
  'domain-configuration',
  'downloads',
  'faq',
  'filter-reference',
  'filter-selection',
  'filters-overview',
  'getting-started',
  'html-filters',
  'https-configuration',
  'iis-configuration',
  'iis-tuning',
  'image-filters',
  'javascript-filters',
  'security',
  'troubleshooting',
  'upgrading-from-open-source',
  'upgrading-to-2-1',
  'whats-new',
];

describe('docs-1.1 collection source (always-on)', () => {
  it('holds exactly one entry: the kept release-notes page', () => {
    const files = readdirSync(DOCS11_SRC_DIR);
    expect(files).toEqual([`${KEPT_SLUG}.mdx`]);
  });

  it('carries the archive notice linking to the current release notes', () => {
    const body = readFileSync(path.join(DOCS11_SRC_DIR, `${KEPT_SLUG}.mdx`), 'utf8');
    expect(body).toContain('Archived 1.15 documentation');
    expect(body).toContain('(/docs/release-notes/)');
  });
});

const BUILT = existsSync(DIST_1_1_DOCS_DIR);

if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    `[legacy-docs-retirement] built output not found at ${DIST_1_1_DOCS_DIR} — skipping the ` +
      'built-HTML retirement check. Run "npm run build" first to exercise it.',
  );
}

describe.skipIf(!BUILT)('the 27 retired 1.15 doc paths (built output)', () => {
  it('does not build the hub page', () => {
    expect(existsSync(path.join(DIST_1_1_DOCS_DIR, 'index.html'))).toBe(false);
  });

  for (const slug of RETIRED_SLUGS) {
    it(`does not build /1.1/docs/${slug}/`, () => {
      expect(existsSync(path.join(DIST_1_1_DOCS_DIR, slug, 'index.html'))).toBe(false);
    });
  }
});

describe.skipIf(!BUILT)(`the kept /1.1/docs/${KEPT_SLUG}/ page (built output)`, () => {
  const htmlPath = path.join(DIST_1_1_DOCS_DIR, KEPT_SLUG, 'index.html');
  let html: string;

  it('builds', () => {
    expect(existsSync(htmlPath)).toBe(true);
    html = readFileSync(htmlPath, 'utf8');
  });

  it('carries the archive line linking to the current release notes', () => {
    html ??= readFileSync(htmlPath, 'utf8');
    expect(html).toMatch(/Archived 1\.15 documentation/);
    expect(html).toMatch(/href="\/docs\/release-notes\/"/);
  });

  it('is self-canonical', () => {
    html ??= readFileSync(htmlPath, 'utf8');
    expect(html).toContain(
      '<link rel="canonical" href="https://modpagespeed.com/1.1/docs/release-notes/">',
    );
  });

  it('carries no robots noindex meta', () => {
    html ??= readFileSync(htmlPath, 'utf8');
    expect(html).not.toMatch(/<meta[^>]+name="robots"[^>]*>/);
  });

  it('breadcrumb JSON-LD names only built pages (no bare /1.1/ hub segment)', () => {
    html ??= readFileSync(htmlPath, 'utf8');
    const match = html.match(/"@type":"BreadcrumbList","itemListElement":(\[.*?\])\}/);
    expect(match, 'expected a BreadcrumbList JSON-LD block').toBeTruthy();
    const items = JSON.parse(match![1]) as Array<{ item: string }>;
    const urls = items.map((item) => item.item);
    expect(urls).toEqual([
      'https://modpagespeed.com/',
      'https://modpagespeed.com/1.1/docs/release-notes/',
    ]);
  });
});
