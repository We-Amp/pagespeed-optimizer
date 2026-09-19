// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// anchor-source-gate.test.ts — the CI gate for the deep-link anchors that
// external redirects depend on. Computes, from the markdown SOURCE, the set
// of ids each converged page's headings and raw anchor tags will expose once
// built, using the same heading-id mechanism the site's markdown pipeline
// uses: an explicit `{#id}` (remark-custom-heading-id) is read literally;
// a heading with no explicit id falls back to `github-slugger` — the exact
// package @astrojs/markdown-remark's own heading-id collector
// (rehype-collect-headings.js) imports, called once per file in document
// order so its dedupe state matches. Raw `<a id="…">` tags (e.g. the table
// anchors in filter-reference.md) pass through the pipeline unchanged, so
// their id is read literally too.
//
// This needs no build step, so it runs wherever `npx vitest run` runs,
// including CI (whose unit-test job does not build the site first). The
// built-HTML counterpart that reads the actual production output is
// anchor-gate.test.ts — it skips itself when dist/ is absent rather than
// gating CI, and is the stronger, catch-anything check to run before a
// deploy.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import GithubSlugger from 'github-slugger';
import { describe, it, expect } from 'vitest';
import { loadAnchorFixture, groupBySlug } from './lib/anchor-fixture';
import { FILTERS, REWRITE_JS_FALLBACK_HREF } from '../src/data/filters';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const FIXTURE_PATH = path.join(WEBSITE_ROOT, 'test/fixtures/legacy-doc-anchors.txt');
const DOCS_SRC_DIR = path.join(WEBSITE_ROOT, 'src/content/docs');

const ANCHOR_ROWS = loadAnchorFixture(FIXTURE_PATH);

function stripFrontmatter(src: string): string {
  return src.replace(/^---\r?\n[\s\S]*?\r?\n---\r?\n?/, '');
}

// Approximates what the rehype heading-id collector concatenates from a
// heading's inline children: code spans, links, and emphasis all reduce to
// their inner text before slugging.
function cleanHeadingText(text: string): string {
  return text
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/\*\*([^*]*)\*\*/g, '$1')
    .replace(/\*([^*]*)\*/g, '$1')
    .replace(/_([^_]*)_/g, '$1')
    .trim();
}

// Every id a page's markdown SOURCE will expose once rendered: one heading
// can carry at most one id (explicit or slugged), plus any raw <a id="…">
// anchors anywhere in the body. Fence-aware: a `#` inside a fenced code
// block (e.g. the `# Apache` / `# Nginx` comment lines in these pages'
// snippets) is not a heading.
function idsInSource(body: string): Set<string> {
  const ids = new Set<string>();
  const slugger = new GithubSlugger();
  let inFence = false;
  for (const line of body.split('\n')) {
    if (/^\s*(```|~~~)/.test(line)) {
      inFence = !inFence;
      continue;
    }
    if (inFence) continue;
    const heading = /^#{1,6}\s+(.*)$/.exec(line);
    if (!heading) continue;
    const rest = heading[1].trim();
    const explicit = /\{#([a-zA-Z0-9_-]+)\}\s*$/.exec(rest);
    if (explicit) {
      ids.add(explicit[1]);
      continue;
    }
    ids.add(slugger.slug(cleanHeadingText(rest)));
  }
  for (const m of body.matchAll(/<a\s+(?:id|name)=["']([a-zA-Z0-9_-]+)["']/g)) {
    ids.add(m[1]);
  }
  return ids;
}

describe('legacy doc anchors resolve at their new /docs/ location (source)', () => {
  it('fixture carries all 35 rows for the 13 converged pages', () => {
    expect(ANCHOR_ROWS.length).toBe(35);
  });

  const bySlug = groupBySlug(ANCHOR_ROWS);

  for (const [slug, rows] of bySlug) {
    describe(`src/content/docs/${slug}.md`, () => {
      const mdPath = path.join(DOCS_SRC_DIR, `${slug}.md`);
      const ids = idsInSource(stripFrontmatter(readFileSync(mdPath, 'utf8')));

      for (const { anchor } of rows) {
        it(`will expose id="${anchor}" once built`, () => {
          expect(ids.has(anchor), `expected an id "${anchor}" among ${slug}.md's headings`).toBe(
            true,
          );
        });
      }
    });
  }
});

// The filter table (src/data/filters.ts) deep-links each row into a
// per-category docs page. Four of those categories were re-pointed from the
// legacy /1.1/docs/ route to /docs/; every such href's anchor must resolve
// in that page's docs/ SOURCE, the same way the legacy-redirect fixture
// above is checked.
describe('src/data/filters.ts anchors resolve at their /docs/ location (source)', () => {
  const DOCS_HREF = /^\/docs\/([a-z0-9-]+)\/#([a-zA-Z0-9_-]+)$/;
  const allHrefs = [...FILTERS.map((f) => f.href), REWRITE_JS_FALLBACK_HREF];
  const docsRows = [...new Set(allHrefs)]
    .map((href) => {
      const m = DOCS_HREF.exec(href);
      return m ? { href, slug: m[1], anchor: m[2] } : null;
    })
    .filter((row): row is { href: string; slug: string; anchor: string } => row !== null);

  it('found /docs/ rows for the re-pointed filter categories', () => {
    expect(docsRows.length).toBeGreaterThan(0);
  });

  const bySlug = new Map<string, typeof docsRows>();
  for (const row of docsRows) {
    const list = bySlug.get(row.slug) ?? [];
    list.push(row);
    bySlug.set(row.slug, list);
  }

  for (const [slug, rows] of bySlug) {
    describe(`src/content/docs/${slug}.md`, () => {
      const mdPath = path.join(DOCS_SRC_DIR, `${slug}.md`);
      const ids = idsInSource(stripFrontmatter(readFileSync(mdPath, 'utf8')));

      for (const { href, anchor } of rows) {
        it(`${href} resolves to id="${anchor}"`, () => {
          expect(ids.has(anchor), `expected an id "${anchor}" among ${slug}.md's headings`).toBe(
            true,
          );
        });
      }
    });
  }
});
