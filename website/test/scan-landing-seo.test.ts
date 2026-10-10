// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// scan-landing-seo.test.ts — deliberate goldens for the SEO estate of the two
// scan landing pages, /analyze/ and /ai-readability/. A rework of the scan UI
// must not move their title, description, canonical, H1 or structured data,
// so the values below are pinned as literals (not imported from source
// constants): a source edit cannot silently move the golden.
//
// Runs against the built output in dist/client and skips itself before
// `npm run build`, like meta-length-gate.test.ts and anchor-gate.test.ts.
//
// To change a golden on purpose: edit the literal here in the same PR as the
// content change, and state the reason in the PR description.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const DIST = path.join(fileURLToPath(new URL('..', import.meta.url)), 'dist/client');
const BUILT = existsSync(DIST);
if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    `[scan-landing-seo] built output not found at ${DIST} — skipping. Run "npm run build" first.`,
  );
}

interface Golden {
  route: string;
  title: string;
  description: string;
  canonical: string;
  h1: string;
  types: string[]; // distinct JSON-LD @type values, sorted
}

const GOLDENS: Golden[] = [
  {
    route: '/analyze/',
    title: 'Free PageSpeed Insights test, with the fix attached',
    description:
      'Run a free PageSpeed Insights test, no signup. Each failing audit is mapped to the mod_pagespeed filter that fixes it, or labeled when none can.',
    canonical: 'https://modpagespeed.com/analyze/',
    h1: 'PageSpeed Insights, with the fix attached.',
    types: [
      'Answer',
      'BreadcrumbList',
      'FAQPage',
      'ListItem',
      'Offer',
      'Organization',
      'Question',
      'WebApplication',
    ],
  },
  {
    route: '/ai-readability/',
    title: 'AI readability checker: what AI crawlers read on your page',
    description:
      'Free scanner: see what AI crawlers read on your page. Most skip JavaScript, so client-rendered content is invisible to them. Fix it in the HTML you serve.',
    canonical: 'https://modpagespeed.com/ai-readability/',
    h1: 'See what AI crawlers miss. Optimize the HTML you serve, on your own servers.',
    types: [
      'Answer',
      'BreadcrumbList',
      'FAQPage',
      'ListItem',
      'Offer',
      'Question',
      'WebApplication',
    ],
  },
];

const decode = (s: string) =>
  s
    .replace(/&#39;|&#x27;|&apos;/g, "'")
    .replace(/&quot;/g, '"')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&');

const page = (route: string) => readFileSync(path.join(DIST, route, 'index.html'), 'utf8');

function metaContent(html: string, attr: 'name' | 'property', key: string): string | undefined {
  const m = html.match(new RegExp(`<meta ${attr}="${key}" content="([^"]*)"`));
  return m ? decode(m[1]) : undefined;
}

function collectTypes(node: unknown, out: Set<string>): void {
  if (Array.isArray(node)) {
    node.forEach((n) => collectTypes(n, out));
  } else if (node && typeof node === 'object') {
    for (const [k, v] of Object.entries(node)) {
      if (k === '@type') [v].flat().forEach((t) => out.add(String(t)));
      else collectTypes(v, out);
    }
  }
}

function jsonLdTypes(html: string): string[] {
  const out = new Set<string>();
  for (const m of html.matchAll(
    /<script type="application\/ld\+json"[^>]*>([\s\S]*?)<\/script>/g,
  )) {
    collectTypes(JSON.parse(m[1]), out);
  }
  return [...out].sort();
}

describe.skipIf(!BUILT)('scan landing pages: SEO goldens (built output)', () => {
  for (const g of GOLDENS) {
    describe(g.route, () => {
      const html = BUILT ? page(g.route) : '';

      it('title', () => {
        const m = html.match(/<title>([^<]*)<\/title>/);
        expect(m && decode(m[1])).toBe(g.title);
      });

      it('meta description', () => {
        expect(metaContent(html, 'name', 'description')).toBe(g.description);
      });

      it('canonical, without a query string', () => {
        const m = html.match(/<link rel="canonical" href="([^"]*)"/);
        expect(m && decode(m[1])).toBe(g.canonical);
        expect(g.canonical).not.toContain('?');
      });

      it('og:title and twitter:title match the title', () => {
        expect(metaContent(html, 'property', 'og:title')).toBe(g.title);
        expect(metaContent(html, 'name', 'twitter:title')).toBe(g.title);
      });

      it('exactly one h1, with the golden text', () => {
        const h1s = [...html.matchAll(/<h1[^>]*>([\s\S]*?)<\/h1>/g)];
        expect(h1s).toHaveLength(1);
        expect(decode(h1s[0][1].replace(/<[^>]*>/g, '').trim())).toBe(g.h1);
      });

      it('JSON-LD @type list', () => {
        expect(jsonLdTypes(html)).toEqual(g.types);
      });
    });
  }

  it('/analyze/ keeps its FAQPage structured data', () => {
    expect(jsonLdTypes(page('/analyze/'))).toContain('FAQPage');
  });

  it('/pagespeed-insights/ still links to /analyze/ from both CTAs', () => {
    const html = page('/pagespeed-insights/');
    for (const event of ['cta_psi_analyze_hero', 'cta_psi_analyze']) {
      expect(html).toMatch(new RegExp(`<a href="/analyze/"[^>]*data-umami-event="${event}"`));
    }
  });
});
