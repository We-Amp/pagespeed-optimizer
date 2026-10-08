// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// filter-topics.test.ts — the per-filter topic pages (/docs/filters/<name>/).
//
// Source checks (always run):
//   - every name in filters.json has a topic, with a section on its group page,
//     a display name, a title of at most 60 and a description of at most 155
//     characters, and a FAQ of two or three questions;
//   - every group page links each filter's section to its topic page;
//   - llms.txt lists exactly the indexable topic pages.
// Build-output checks (skip themselves without dist/, like anchor-gate):
//   - one page per filter, exactly one <h1>, the title and description the
//     data computed, the visible FAQ equal to the FAQPage JSON-LD;
//   - noindex on exactly the thin pages, and those absent from the sitemap
//     while every indexable one is in it;
//   - the /docs/filters/ table and every /examples/<slug>/ page link to the
//     topic pages.

import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import {
  loadFilterTopics,
  plainText,
  THIN_WORDS,
  HUMAN_NAMES,
  PAGE_BY_CATEGORY,
} from '../../src/lib/filter-topics.mjs';
import { EXAMPLES } from '../../src/data/examples';

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const data = JSON.parse(
  readFileSync(resolve(WEBSITE_ROOT, 'src/data/reference/filters.json'), 'utf8'),
);
const allNames: string[] = [
  ...data.filters.map((f: { name: string }) => f.name),
  ...data.aliases.map((a: { name: string }) => a.name),
];
const topics = loadFilterTopics(WEBSITE_ROOT);
const byName = new Map(topics.map((t) => [t.name, t]));

describe('filter topics (source)', () => {
  it('cover every filter and compound name in filters.json, once', () => {
    expect(topics.map((t) => t.name).sort()).toEqual([...allNames].sort());
    expect(Object.keys(HUMAN_NAMES).sort()).toEqual([...allNames].sort());
  });

  for (const t of topics) {
    it(`${t.name}: title, description, FAQ and guard`, () => {
      expect(t.path).toBe(`/docs/filters/${t.name}/`);
      expect(t.title.length).toBeLessThanOrEqual(60);
      expect(t.title).toContain(t.name);
      expect(t.description.length).toBeLessThanOrEqual(155);
      expect(t.description.length).toBeGreaterThan(20);
      expect(t.faq.length).toBeGreaterThanOrEqual(2);
      expect(t.faq.length).toBeLessThanOrEqual(3);
      expect(t.section.lede.length).toBeGreaterThan(0);
      expect(t.indexable).toBe(!t.alternateSpellingOf && t.section.words >= THIN_WORDS);
    });
  }

  it('use one consistent title form per length tier', () => {
    for (const t of topics) {
      const full = `${t.human}: the ${t.name} filter for Apache and nginx`;
      if (full.length <= 60) expect(t.title).toBe(full);
    }
  });

  it('every group page links each filter section to its topic page', () => {
    const pages = new Set(Object.values(PAGE_BY_CATEGORY));
    const text = [...pages]
      .map((slug) => readFileSync(resolve(WEBSITE_ROOT, `src/content/docs/${slug}.md`), 'utf8'))
      .join('\n');
    for (const t of topics) {
      if (t.alternateSpellingOf) continue;
      expect(text, `link to ${t.path}`).toContain(`](${t.path})`);
    }
  });

  it('llms.txt lists exactly the indexable topic pages', () => {
    const llms = readFileSync(resolve(WEBSITE_ROOT, 'public/llms.txt'), 'utf8');
    const listed = [...llms.matchAll(/\(https:\/\/modpagespeed\.com\/docs\/filters\/([^/]+)\/\)/g)]
      .map((m) => m[1])
      .sort();
    expect(listed).toEqual(
      topics
        .filter((t) => t.indexable)
        .map((t) => t.name)
        .sort(),
    );
  });
});

const DIST = resolve(WEBSITE_ROOT, 'dist/client');
const BUILT = existsSync(resolve(DIST, 'docs/filters'));
if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    '[filter-topics] built output not found — skipping the built-HTML checks. ' +
      'Run "npm run build" first to exercise them.',
  );
}

function attr(html: string, re: RegExp): string | null {
  const m = re.exec(html);
  return m ? m[1] : null;
}

function decode(s: string): string {
  return s
    .replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&#x27;/g, "'")
    .replace(/&amp;/g, '&');
}

function textOf(html: string): string {
  return decode(html.replace(/<[^>]+>/g, ''))
    .replace(/\s+/g, ' ')
    .trim();
}

describe.skipIf(!BUILT)('filter topics (built html)', () => {
  if (!BUILT) return;
  const sitemap = readdirSync(DIST)
    .filter((f) => /^sitemap-\d+\.xml$/.test(f))
    .map((f) => readFileSync(resolve(DIST, f), 'utf8'))
    .join('\n');

  it('has a page for every filter and nothing else under /docs/filters/', () => {
    const dirs = readdirSync(resolve(DIST, 'docs/filters'), { withFileTypes: true })
      .filter((d) => d.isDirectory())
      .map((d) => d.name)
      .sort();
    expect(dirs).toEqual([...allNames].sort());
  });

  for (const t of topics) {
    describe(t.path, () => {
      const html = readFileSync(resolve(DIST, `docs/filters/${t.name}/index.html`), 'utf8');

      it('has exactly one h1, the data title and description', () => {
        expect(html.match(/<h1[\s>]/g)?.length).toBe(1);
        expect(decode(attr(html, /<title>([^<]*)<\/title>/) ?? '')).toBe(t.title);
        expect(decode(attr(html, /<meta name="description" content="([^"]*)"/) ?? '')).toBe(
          t.description,
        );
      });

      it('shows the FAQ its FAQPage JSON-LD carries', () => {
        const blocks = [
          ...html.matchAll(/<script type="application\/ld\+json"[^>]*>([\s\S]*?)<\/script>/g),
        ].map((m) => JSON.parse(m[1]));
        const faqLd = blocks.flat().find((b) => b['@type'] === 'FAQPage');
        expect(faqLd).toBeTruthy();
        const ld = faqLd.mainEntity.map((q: any) => ({ q: q.name, a: q.acceptedAnswer.text }));
        const dl = /<dl[^>]*data-topic-faq[^>]*>([\s\S]*?)<\/dl>/.exec(html)?.[1] ?? '';
        const visible = [
          ...dl.matchAll(/<dt[^>]*>([\s\S]*?)<\/dt>\s*<dd[^>]*>([\s\S]*?)<\/dd>/g),
        ].map((m) => ({ q: textOf(m[1]), a: textOf(m[2]) }));
        expect(visible).toEqual(ld);
        expect(ld).toEqual(t.faq.map((f) => ({ q: f.q, a: plainText(f.a) })));
      });

      it(
        t.indexable ? 'is indexable and in the sitemap' : 'is noindex and not in the sitemap',
        () => {
          const noindex = /<meta name="robots" content="noindex,follow"/.test(html);
          expect(noindex).toBe(!t.indexable);
          expect(sitemap.includes(`https://modpagespeed.com${t.path}<`)).toBe(t.indexable);
        },
      );
    });
  }

  it('the /docs/filters/ table links every row to its topic page', () => {
    const html = readFileSync(resolve(DIST, 'docs/filters/index.html'), 'utf8');
    for (const name of allNames) {
      expect(html, name).toMatch(
        new RegExp(`<tr[^>]*id="${name}"[^>]*>[\\s\\S]*?href="/docs/filters/${name}/"`),
      );
    }
  });

  it('every example page links to its full guide', () => {
    for (const ex of EXAMPLES) {
      const html = readFileSync(resolve(DIST, `examples/${ex.slug}/index.html`), 'utf8');
      const href = attr(html, /<a[^>]*href="([^"]+)"[^>]*data-full-guide/);
      expect(href, ex.slug).toMatch(/^\/docs\/filters\/[a-z0-9_]+\/$/);
      expect(byName.has(href!.split('/')[3]), ex.slug).toBe(true);
    }
  });
});
