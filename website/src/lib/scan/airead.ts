// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pure formatting for the AI readability panel: the grade, the five category
// rows (same arithmetic and wording as the original result page), the token
// line, the raw-HTML Markdown and the "Also found" sentences. No DOM here.

import { agentPassCtaApplies, complianceFixCtaApplies, tollboothCtaApplies } from './demand.mjs';

interface Category {
  score: number;
  max: number;
  flag?: string;
  verdict?: string;
}
interface Report {
  url?: string;
  grade?: string;
  score?: number;
  categories?: Record<string, Category>;
  diff?: {
    staticCleanTokens?: number;
    renderedCleanTokens?: number;
    staticMarkdown?: string;
  };
  agentVerifiability?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  signedAgentVerification?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
  accessibility?: any; // eslint-disable-line @typescript-eslint/no-explicit-any
}

export interface CategoryRow {
  name: string;
  score: number;
  max: number;
  /** Bar width in percent. */
  pct: number;
  /** The flag chip text, or an empty string. */
  flag: string;
  verdict: string;
}

export interface AireadModel {
  grade: string;
  score: number;
  /** strong, mixed, weak or poor. */
  word: string;
  url: string;
  categories: CategoryRow[];
  tokenLine: string;
  rawMarkdown: string;
  alsoFound: { wedge: 'tollbooth' | 'agentpass' | 'compliancefix'; text: string }[];
}

const WORDS: Record<string, string> = {
  A: 'strong',
  B: 'strong',
  C: 'mixed',
  D: 'weak',
  F: 'poor',
};

export const TOKEN_CAPTION =
  'The readable words in your raw HTML are what most AI crawlers get. A big shortfall before JavaScript runs means your content barely exists until your scripts run.';

/** The reason sentences for the three gated topics, worded as on the original page. */
export function alsoFoundSentences(r: Report): AireadModel['alsoFound'] {
  const out: AireadModel['alsoFound'] = [];
  const av = r.agentVerifiability;
  const ax = r.accessibility;
  const sa = r.signedAgentVerification;
  if (tollboothCtaApplies(av)) {
    out.push({
      wedge: 'tollbooth',
      text:
        'About ' +
        av.detail.exposurePct +
        '% of your content is already in the raw HTML ' +
        av.detail.aiCrawlersAllowed.length +
        ' AI crawler(s) can read — but your origin can’t tell a real signed agent from an impostor.',
    });
  }
  if (agentPassCtaApplies(sa)) {
    out.push({
      wedge: 'agentpass',
      text:
        sa.classification === 'verifying'
          ? 'Your origin responded differently to our validly-signed probe than to a corrupted signature — behaviour consistent with cryptographic verification.'
          : 'Your origin already treats signed-agent requests differently from unsigned ones.',
    });
  }
  if (complianceFixCtaApplies(ax)) {
    out.push({
      wedge: 'compliancefix',
      text:
        (ax.detail.fixableInline === 1
          ? '1 of these issues looks'
          : ax.detail.fixableInline + ' of these issues look') +
        ' like the kind a self-hosted optimizer can address server-side, on your own servers, with no app changes.',
    });
  }
  return out;
}

export function formatAiread(r: Report): AireadModel {
  const categories = Object.entries(r.categories ?? {}).map(([name, c]) => ({
    name,
    score: c.score,
    max: c.max,
    pct: Math.round((c.score / c.max) * 100),
    flag:
      c.flag === 'csr-gap' ? 'invisible without JS' : c.flag === 'thin' ? 'no content found' : '',
    verdict: c.verdict ?? '',
  }));
  const d = r.diff ?? {};
  const tokens = (n: number | undefined) => '~' + (n ?? 0).toLocaleString();
  return {
    grade: String(r.grade),
    score: Number(r.score),
    word: WORDS[String(r.grade)] || '',
    url: r.url ?? '',
    categories,
    tokenLine: `Raw HTML: ${tokens(d.staticCleanTokens)} tokens · after JavaScript: ${tokens(d.renderedCleanTokens)} tokens`,
    rawMarkdown: d.staticMarkdown && d.staticMarkdown.trim() ? d.staticMarkdown : '',
    alsoFound: alsoFoundSentences(r),
  };
}
