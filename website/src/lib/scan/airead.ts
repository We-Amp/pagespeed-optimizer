// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pure formatting for the AI readability panel: the grade, the five category
// rows (same arithmetic and wording as the original result page), the token
// line, the raw-HTML Markdown and the "Also found" sentences. No DOM here.

import { plural } from './plural';
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
  'Most AI crawlers read only the raw HTML. When the raw count is far below the count after JavaScript, those crawlers see little of your content.';

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
        plural(av.detail.aiCrawlersAllowed.length, 'AI crawler') +
        ' can read — but your origin can’t tell a real signed agent from an impostor.',
    });
  }
  if (agentPassCtaApplies(sa)) {
    out.push({
      wedge: 'agentpass',
      text:
        sa.classification === 'verifying'
          ? 'Your origin answered our signed test request differently from one with a corrupted signature, which is consistent with checking agent signatures.'
          : 'Your origin already treats signed-agent requests differently from unsigned ones.',
    });
  }
  if (complianceFixCtaApplies(ax)) {
    out.push({
      wedge: 'compliancefix',
      text:
        'Automated checks flagged ' +
        (ax.detail.total === 1
          ? '1 accessibility issue'
          : ax.detail.total + ' accessibility issues') +
        ' on this page; ' +
        (ax.detail.fixableInline === 1 ? '1 looks' : ax.detail.fixableInline + ' look') +
        ' like the kind a self-hosted optimizer can fix on your own servers, with no app changes. We don’t issue compliance certificates.',
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
    tokenLine: `Readable text in raw HTML: ${tokens(d.staticCleanTokens)} tokens · after JavaScript: ${tokens(d.renderedCleanTokens)} tokens`,
    rawMarkdown: d.staticMarkdown && d.staticMarkdown.trim() ? d.staticMarkdown : '',
    alsoFound: alsoFoundSentences(r),
  };
}
