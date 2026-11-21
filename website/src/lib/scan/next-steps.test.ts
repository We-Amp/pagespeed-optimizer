// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  MAX_STEPS,
  NOTHING_MEASURED,
  NOTHING_URGENT,
  NOTHING_URGENT_PARTIAL,
  TAG,
  emptyLine,
  nextSteps,
  showLabel,
} from './next-steps';

const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url),
      'utf8',
    ),
  ).report;

const POOR = { mobile: 40, desktop: 95 };
const GOOD = { mobile: 95, desktop: 99 };
const CONTENT =
  'Put this page’s main text in the HTML the server sends, so it does not depend on JavaScript.';
const TITLE_CHANGED =
  'JavaScript changes this page’s title after it loads. Put the final one in the HTML the server sends.';
const SPEED =
  'Speed is poor on mobile: open the Speed panel for the audits PageSpeed Insights flagged.';
const A11Y =
  'Fix 7 accessibility issues the scan found: Images must have alternative text. These are automated checks, not an accessibility audit.';
const pairs = (r: ReturnType<typeof nextSteps>) => r.steps.map((s) => [s.text, s.tag]);

describe('nextSteps over the fixtures', () => {
  it('full report (no platform reading): content, SEO, then speed when it is poor', () => {
    const r = nextSteps(fixture('full'), POOR);
    expect(pairs(r)).toEqual([
      [CONTENT, 'needs a developer'],
      [TITLE_CHANGED, 'needs a developer'],
      [SPEED, 'needs a developer'],
    ]);
    // The accessibility step also qualifies; three are shown.
    expect(r.total).toBe(4);
    expect(r.steps.map((s) => [s.pillar, s.target])).toEqual([
      ['airead', '[data-scan-cat="Content fidelity"]'],
      ['risk', '[data-risk-row="seoDefects"]'],
      ['speed', ''],
    ]);
  });

  it('full report before the speed result: the accessibility step takes the third place', () => {
    const r = nextSteps(fixture('full'), null);
    expect(pairs(r)).toEqual([
      [CONTENT, 'needs a developer'],
      [TITLE_CHANGED, 'needs a developer'],
      [A11Y, 'in your site editor'],
    ]);
    expect(r.total).toBe(3);
    expect(nextSteps(fixture('full'), GOOD)).toEqual(r);
  });

  it('hosted report: the same findings, and no step that needs server access', () => {
    const r = nextSteps(fixture('hosted'), GOOD);
    expect(pairs(r)).toEqual([
      [CONTENT, 'needs a developer'],
      [TITLE_CHANGED, 'needs a developer'],
      [A11Y, 'in your site editor'],
    ]);
    expect(r.steps.some((s) => s.tag === TAG.server)).toBe(false);
  });

  it('hosted report with blocking robots.txt rules: the step is tagged for the site settings', () => {
    const report = fixture('hosted');
    report.access.crawlerRules.training.blocked = ['GPTBot'];
    const robots = nextSteps(report, GOOD).steps[1];
    expect(robots).toMatchObject({
      id: 'robots',
      text: 'Your robots.txt blocks GPTBot from the home page. Remove that rule if you want AI crawlers to read the site.',
      tag: 'in your site settings',
      target: '[data-scan-cat="Crawler access"]',
    });
  });

  it('unknown platform: the robots.txt step names the blocked crawlers and needs server access', () => {
    const r = nextSteps(fixture('unknown'), GOOD);
    expect(pairs(r)).toEqual([
      [CONTENT, 'needs a developer'],
      [
        'Your robots.txt blocks GPTBot, ClaudeBot and OAI-SearchBot from the home page. Remove those rules if you want AI crawlers to read the site.',
        'needs server access',
      ],
      [TITLE_CHANGED, 'needs a developer'],
    ]);
    expect(r.total).toBe(4);
  });

  it('clean report: no steps, with or without a speed result', () => {
    expect(nextSteps(fixture('clean'), GOOD)).toEqual({ steps: [], total: 0 });
    expect(nextSteps(fixture('clean'), null)).toEqual({ steps: [], total: 0 });
    expect(pairs(nextSteps(fixture('clean'), POOR))).toEqual([[SPEED, 'needs a developer']]);
  });

  it('blocked report: one step about the missing text, nothing built on the empty page', () => {
    const r = nextSteps(fixture('blocked'), null);
    expect(pairs(r)).toEqual([
      [
        'The scan found almost no text on this page, even after running JavaScript. Check that the page is public and does not block automated visitors, then scan it again.',
        'needs server access',
      ],
    ]);
    expect(r.total).toBe(1);
  });

  it('never lists more than three steps', () => {
    for (const name of ['full', 'hosted', 'unknown', 'clean', 'blocked']) {
      expect(nextSteps(fixture(name), POOR).steps.length).toBeLessThanOrEqual(MAX_STEPS);
    }
  });
});

describe('nextSteps conditions', () => {
  const base = () => fixture('clean');

  it('gives no steps without a report, except a poor speed result', () => {
    expect(nextSteps(null, null)).toEqual({ steps: [], total: 0 });
    expect(nextSteps(undefined, GOOD)).toEqual({ steps: [], total: 0 });
    expect(pairs(nextSteps(null, POOR))).toEqual([[SPEED, 'needs a developer']]);
    expect(nextSteps(null, { mobile: null, desktop: 30 }).steps[0].text).toBe(
      'Speed is poor on desktop: open the Speed panel for the audits PageSpeed Insights flagged.',
    );
  });

  it('counts a weak content score without the flag, and leaves a strong one alone', () => {
    const r = base();
    r.categories['Content fidelity'] = { score: 14, max: 30, flag: null };
    expect(nextSteps(r).steps[0]).toMatchObject({ id: 'content', text: CONTENT });
    r.categories['Content fidelity'] = { score: 15, max: 30, flag: null };
    expect(nextSteps(r).total).toBe(0);
  });

  it('counts crawlers past three and words the wildcard rule on its own', () => {
    const r = base();
    r.access.crawlerRules = {
      parsed: true,
      training: { allowed: [], blocked: ['GPTBot', 'ClaudeBot', 'CCBot'] },
      answer: { allowed: [], blocked: ['GPTBot', 'OAI-SearchBot'] },
      wildcardBlocksAll: false,
    };
    expect(nextSteps(r).steps[0].text).toBe(
      'Your robots.txt blocks 4 AI crawlers from the home page. Remove those rules if you want AI crawlers to read the site.',
    );
    r.access.crawlerRules.wildcardBlocksAll = true;
    expect(nextSteps(r).steps[0].text).toBe(
      'Your robots.txt blocks every crawler that has no rule of its own from the home page. Remove that rule if you want AI crawlers to read the site.',
    );
    // A file the scan could not parse, or one that blocks nothing, gives no step.
    r.access.crawlerRules = { parsed: false, reason: 'missing' };
    expect(nextSteps(r).total).toBe(0);
    r.access.crawlerRules = {
      parsed: true,
      training: { allowed: ['GPTBot'], blocked: [] },
      answer: { allowed: [], blocked: [] },
      wildcardBlocksAll: false,
    };
    expect(nextSteps(r).total).toBe(0);
  });

  it('asks for structured data only when that category is under half', () => {
    const r = base();
    r.categories['Structured data'] = { score: 9, max: 20 };
    expect(nextSteps(r).steps[0]).toMatchObject({
      id: 'structured-data',
      text: 'Add structured data that says what this page is: your organization, a product, an article or a list of questions and answers.',
      tag: 'in your site editor',
    });
    r.categories['Structured data'] = { score: 10, max: 20 };
    expect(nextSteps(r).total).toBe(0);
  });

  it('names what the citation signals miss; author and date only on an article', () => {
    const r = base();
    r.categories['Citation readiness'] = { score: 2, max: 10 };
    r.signals = {
      jsonLdTypes: ['Organization'],
      title: 'T',
      metaDescription: false,
      canonical: false,
      author: false,
      published: false,
      modified: false,
    };
    expect(nextSteps(r).steps[0]).toMatchObject({
      id: 'citation',
      text: 'This page has no meta description and canonical link. Add them.',
      tag: 'in your site editor',
      target: '[data-scan-cat="Citation readiness"]',
    });
    r.signals.jsonLdTypes = ['NewsArticle'];
    r.signals.metaDescription = true;
    r.signals.canonical = true;
    expect(nextSteps(r).steps[0].text).toBe(
      'This page has no author name and publication date. Add them.',
    );
    r.signals.modified = true;
    expect(nextSteps(r).steps[0].text).toBe('This page has no author name. Add it.');
    // Nothing to name: no step, however low the score.
    r.signals.author = true;
    expect(nextSteps(r).total).toBe(0);
    // An attributable page is left alone.
    r.signals.author = false;
    r.categories['Citation readiness'] = { score: 7, max: 10 };
    expect(nextSteps(r).total).toBe(0);
  });

  it('words the top SEO defect and does not repeat it as a citation step', () => {
    const r = base();
    const seo = (defects: string[]) => {
      r.seoDefects = {
        status: 'ok',
        verdict: 'attention',
        defects,
        counts: { total: defects.length },
      };
      return nextSteps(r).steps.find((s) => s.id === 'seo');
    };
    expect(seo(['canonical-js-only', 'title-js-only'])).toMatchObject({
      text: 'This page’s title is only added by JavaScript. Put it in the HTML the server sends.',
      tag: 'needs a developer',
    });
    expect(seo(['description-missing'])).toMatchObject({
      text: 'This page has no meta description. Add one.',
      tag: 'in your site editor',
    });
    expect(seo(['canonical-multiple'])).toMatchObject({
      text: 'This page has more than one canonical link. Keep one.',
      tag: 'in your site editor',
    });
    expect(seo(['hreflang-invalid', 'jsonld-invalid'])).toMatchObject({
      text: 'The scan flagged 2 SEO issues in the head of this page. Open the Risk & SEO panel for the list.',
      tag: 'needs a developer',
    });
    r.categories['Citation readiness'] = { score: 2, max: 10 };
    r.signals = { jsonLdTypes: [], title: 'T', metaDescription: false, canonical: true };
    seo(['description-missing']);
    expect(nextSteps(r).steps.map((s) => s.id)).toEqual(['seo']);
    // A lens that did not run gives no step.
    r.seoDefects = { status: 'blocked', verdict: 'unknown', defects: ['title-missing'] };
    expect(nextSteps(r).steps.some((s) => s.id === 'seo')).toBe(false);
  });

  it('counts one accessibility issue in the singular and copes without a top rule', () => {
    const r = base();
    r.accessibility = { available: true, detail: { total: 1, topRules: [] } };
    expect(nextSteps(r).steps[0].text).toBe(
      'Fix 1 accessibility issue the scan found. These are automated checks, not an accessibility audit.',
    );
    r.accessibility = { available: false, detail: { total: 4 } };
    expect(nextSteps(r).total).toBe(0);
  });
});

describe('emptyLine and showLabel', () => {
  it('says nothing is urgent only when no tile asks for attention', () => {
    expect(emptyLine([])).toBe('Nothing urgent on this page. Open a panel for the details.');
    expect(emptyLine([])).toBe(NOTHING_URGENT);
    expect(emptyLine([], 2)).toBe(NOTHING_URGENT_PARTIAL);
    expect(emptyLine([], 0)).toBe(NOTHING_MEASURED);
    expect(emptyLine(['risk'])).toBe(
      'The scan has no single next step for this page. Open the Risk & SEO panel for what it flagged.',
    );
    expect(emptyLine(['airead', 'risk'])).toBe(
      'The scan has no single next step for this page. Open the AI readability and Risk & SEO panels for what it flagged.',
    );
  });
  it('names the panel a step opens', () => {
    expect(showLabel('airead')).toBe('Show in AI readability');
    expect(showLabel('risk')).toBe('Show in Risk & SEO');
    expect(showLabel('speed')).toBe('Show in Speed');
  });
});
