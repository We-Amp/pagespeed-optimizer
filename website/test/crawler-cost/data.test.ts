// website/test/crawler-cost/data.test.ts
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import crawlers from '../../src/data/ai-crawlers.json';
import referrers from '../../src/data/answer-engine-referrers.json';
import { ROBOTS_ONLY_TOKENS } from '../../src/lib/crawler-cost/classify';

const DATE = /^\d{4}-\d{2}-\d{2}$/;
const KINDS = new Set(['training', 'search', 'fetcher', 'other']);

describe('ai-crawlers.json', () => {
  it('has unique tokens, every field, valid kinds and dates', () => {
    const tokens = crawlers.map((c) => c.token.toLowerCase());
    expect(new Set(tokens).size).toBe(tokens.length);
    for (const c of crawlers) {
      expect(c.token.length).toBeGreaterThan(2);
      expect(c.operator.length).toBeGreaterThan(0);
      expect(KINDS.has(c.kind)).toBe(true);
      expect(c.docs).toMatch(/^https:\/\//);
      expect(c.added).toMatch(DATE);
    }
  });
  it('carries the tokens the goldens rely on, with their kinds', () => {
    const byToken = new Map(crawlers.map((c) => [c.token, c]));
    expect(byToken.get('GPTBot')).toMatchObject({ operator: 'OpenAI', kind: 'training' });
    expect(byToken.get('ChatGPT-User')).toMatchObject({ operator: 'OpenAI', kind: 'fetcher' });
    expect(byToken.get('OAI-SearchBot')).toMatchObject({ operator: 'OpenAI', kind: 'search' });
    expect(byToken.get('ClaudeBot')).toMatchObject({ operator: 'Anthropic', kind: 'training' });
    expect(byToken.get('Claude-User')).toMatchObject({ operator: 'Anthropic', kind: 'fetcher' });
    expect(byToken.get('PerplexityBot')).toMatchObject({ operator: 'Perplexity', kind: 'search' });
    expect(byToken.get('Perplexity-User')).toMatchObject({
      operator: 'Perplexity',
      kind: 'fetcher',
    });
    expect(byToken.get('Bytespider')).toMatchObject({ operator: 'ByteDance', kind: 'training' });
    expect(byToken.get('CCBot')).toMatchObject({ operator: 'Common Crawl' });
    expect(byToken.get('Meta-ExternalAgent')).toMatchObject({ operator: 'Meta', kind: 'training' });
  });
  it('holds no robots.txt-only control token', () => {
    const lower = new Set(crawlers.map((c) => c.token.toLowerCase()));
    for (const t of ROBOTS_ONLY_TOKENS) expect(lower.has(t.toLowerCase())).toBe(false);
  });
  it('ships the MIT notice for the seed list beside the data', () => {
    const notice = readFileSync(
      new URL('../../src/data/ai-crawlers-LICENSE.txt', import.meta.url),
      'utf8',
    );
    expect(notice).toMatch(/MIT License/);
    expect(notice).toMatch(/ai\.robots\.txt/);
  });
});

describe('answer-engine-referrers.json', () => {
  it('has lowercase hosts without www, unique, with operator, product and a checked date', () => {
    const hosts = referrers.hosts.map((h) => h.host);
    expect(new Set(hosts).size).toBe(hosts.length);
    for (const h of referrers.hosts) {
      expect(h.host).toBe(h.host.toLowerCase());
      expect(h.host.startsWith('www.')).toBe(false);
      expect(h.operator.length).toBeGreaterThan(0);
      expect(h.product.length).toBeGreaterThan(0);
      expect(h.checked).toMatch(DATE);
    }
    expect(hosts).toContain('chatgpt.com');
    expect(hosts).toContain('perplexity.ai');
    expect(hosts).toContain('claude.ai');
  });
  it('starts the query markers with utm_source=chatgpt.com', () => {
    expect(referrers.queryMarkers[0]).toEqual({
      param: 'utm_source',
      value: 'chatgpt.com',
      operator: 'OpenAI',
    });
  });
});
