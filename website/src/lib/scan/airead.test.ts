// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The AI readability panel's formatter: grade, score, word and category rows
// for the fixture reports, and the lines it adds below them.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { formatAiread } from './airead';

const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      fileURLToPath(new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url)),
      'utf8',
    ),
  ).report;

// The grade, score, word and five category rows the panel shows for the two
// fixture reports.
const GRADING = {
  full: {
    grade: 'C',
    score: 64,
    word: 'mixed',
    categories: [
      {
        name: 'Content fidelity',
        score: 12,
        max: 30,
        pct: 40,
        flag: 'invisible without JS',
        verdict:
          "Only ~38% of your content exists without JavaScript — AI tools that don't render (and free CDN markdown features) see an almost-empty page. THIS is the big one.",
      },
      {
        name: 'Token efficiency',
        score: 8,
        max: 20,
        pct: 40,
        flag: '',
        verdict:
          'Bloated: the page is ~98× more HTML than actual content (52,300 tokens raw vs ~534 clean). Agents pay for — and choke on — the difference.',
      },
      {
        name: 'Structured data',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        verdict:
          'Strong machine-readable metadata (JSON-LD (Organization, WebSite, Product), Open Graph, canonical, meta description).',
      },
      {
        name: 'Crawler access',
        score: 18,
        max: 20,
        pct: 90,
        flag: '',
        verdict: 'AI crawlers are allowed to fetch the page. robots.txt allows AI crawlers.',
      },
      {
        name: 'Citation readiness',
        score: 6,
        max: 10,
        pct: 60,
        flag: '',
        verdict:
          'Thin attribution metadata (canonical URL, title, date) — an LLM citing you may get the source/author/date wrong.',
      },
    ],
  },
  clean: {
    grade: 'A',
    score: 100,
    word: 'strong',
    categories: [
      {
        name: 'Content fidelity',
        score: 30,
        max: 30,
        pct: 100,
        flag: '',
        verdict:
          'Content is fully present in the server HTML — readable by every AI tool, no rendering required.',
      },
      {
        name: 'Token efficiency',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        verdict: 'Lean: ~14× HTML-to-content ratio — cheap for an agent to ingest.',
      },
      {
        name: 'Structured data',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        verdict:
          'Strong machine-readable metadata (JSON-LD (Organization, WebSite, Article), Open Graph, canonical, meta description).',
      },
      {
        name: 'Crawler access',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        verdict:
          'AI crawlers are allowed to fetch the page. robots.txt allows AI crawlers; llms.txt present.',
      },
      {
        name: 'Citation readiness',
        score: 10,
        max: 10,
        pct: 100,
        flag: '',
        verdict: 'Attributable: canonical URL, title, author, date.',
      },
    ],
  },
} as const;

describe.each(['full', 'clean'] as const)('grading (%s report)', (name) => {
  const m = formatAiread(fixture(name));
  const want = GRADING[name];

  it('grade, score and word', () => {
    expect({ grade: m.grade, score: m.score, word: m.word }).toEqual({
      grade: want.grade,
      score: want.score,
      word: want.word,
    });
  });

  it('the five category rows', () => {
    expect(m.categories).toEqual(want.categories);
  });
});

describe('formatAiread extras', () => {
  it('flags the content gap on the full report only', () => {
    expect(formatAiread(fixture('full')).categories.map((c) => c.flag)).toContain(
      'invisible without JS',
    );
    expect(formatAiread(fixture('clean')).categories.every((c) => c.flag === '')).toBe(true);
  });
  it('fires the gate sentences on the full report only', () => {
    expect(formatAiread(fixture('full')).alsoFound.map((f) => f.wedge)).toEqual([
      'tollbooth',
      'agentpass',
      'compliancefix',
      'markdown-origin',
    ]);
    expect(formatAiread(fixture('clean')).alsoFound.map((f) => f.wedge)).toEqual([
      'markdown-origin',
    ]);
  });
  it('words the Markdown row by outcome and tolerates a missing or failed lens', () => {
    const full = formatAiread(fixture('full')).alsoFound.at(-1)!;
    expect(full.text).toBe(
      'This page did not return Markdown when asked for it (Accept: text/markdown). The mod_pagespeed optimizer worker can serve a rendered Markdown copy at the same URL, on your own server. It is experimental and off by default. The module alone never renders Markdown; with AgentOptimize on it only adds Vary: Accept.',
    );
    expect(full.link).toEqual({ label: 'How it works', href: '/docs/agent-optimize/' });
    const clean = formatAiread(fixture('clean')).alsoFound[0];
    expect(clean.text).toBe('This page returns Markdown on request.');
    expect(clean.link).toBeUndefined();
    const absent = fixture('clean');
    delete absent.agentSignals;
    expect(formatAiread(absent).alsoFound).toEqual([]);
    const failed = fixture('full');
    failed.agentSignals.status = 'error';
    expect(formatAiread(failed).alsoFound.map((f) => f.wedge)).not.toContain('markdown-origin');
    const unknown = fixture('full');
    unknown.agentSignals.markdown.negotiated = null;
    expect(formatAiread(unknown).alsoFound.map((f) => f.wedge)).not.toContain('markdown-origin');
  });
  it('pluralises the AI crawler count from the number', () => {
    const full = fixture('full');
    const text = (r: Parameters<typeof formatAiread>[0]) => formatAiread(r).alsoFound[0].text;
    expect(text(full)).toContain('9 AI crawlers can read');
    full.agentVerifiability.detail.aiCrawlersAllowed = ['GPTBot'];
    expect(text(full)).toContain('1 AI crawler can read');
  });
  it('builds the token line from the report', () => {
    expect(formatAiread(fixture('full')).tokenLine).toBe(
      'Readable text in raw HTML: ~190 tokens · after JavaScript: ~540 tokens',
    );
  });
});
