// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// nav-docs-links.test.ts: every /docs/<slug>/ link in the header Docs menu
// (BaseLayout.astro, `docsSections`) must resolve to a docs-collection page or
// a hand-written docs route, so the menu can never point at a 404.

import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const layout = readFileSync(path.join(root, 'src/layouts/BaseLayout.astro'), 'utf8');

const start = layout.indexOf('const docsSections');
const end = layout.indexOf('\n];', start);
const menuSource = layout.slice(start, end);
const hrefs = [...menuSource.matchAll(/href: '(\/[^']+)'/g)].map((m) => m[1]);

function resolves(href: string): boolean {
  const segs = href.split('/').filter(Boolean);
  if (segs[0] === 'docs' && segs.length === 2) {
    const slug = segs[1];
    const docs = readdirSync(path.join(root, 'src/content/docs'));
    if (docs.some((f) => f.replace(/\.mdx?$/, '') === slug)) return true;
    return (
      existsSync(path.join(root, 'src/pages/docs', `${slug}.astro`)) ||
      existsSync(path.join(root, 'src/pages/docs', slug, 'index.astro'))
    );
  }
  const rel = segs.join('/');
  return (
    existsSync(path.join(root, 'src/pages', `${rel}.astro`)) ||
    existsSync(path.join(root, 'src/pages', rel, 'index.astro'))
  );
}

describe('header Docs menu links', () => {
  it('finds the menu links', () => {
    expect(hrefs.length).toBeGreaterThanOrEqual(25);
  });
  for (const href of hrefs) {
    it(`${href} resolves`, () => {
      expect(resolves(href)).toBe(true);
    });
  }
});
