// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// "Your next steps": up to three plain sentences that say what to do about a
// result, most important first, each with who can do it and the panel row it
// comes from. Built only from what the report and the speed scores already
// hold. Pure: no DOM, no network.

import { serverOffersApply } from './fit';
import { plural } from './plural';
import { lensMeasured, speedStatus } from './status';

export type StepPillar = 'speed' | 'airead' | 'risk';

/** Who can act on a step. */
export const TAG = {
  editor: 'in your site editor',
  settings: 'in your site settings',
  server: 'needs server access',
  developer: 'needs a developer',
} as const;
export type StepTag = (typeof TAG)[keyof typeof TAG];

export interface Step {
  id: string;
  /** What to do, as one or two plain sentences. */
  text: string;
  tag: StepTag;
  /** The tile whose panel holds the finding. */
  pillar: StepPillar;
  /** A selector for the finding's row inside that panel; empty for the panel itself. */
  target: string;
}

export interface NextSteps {
  /** The steps shown: the first MAX_STEPS of everything that qualified. */
  steps: Step[];
  /** How many steps qualified in all. */
  total: number;
}

/** The most steps listed. */
export const MAX_STEPS = 3;
export const STEPS_HEADING = 'Your next steps';
export const NOTHING_URGENT = 'Nothing urgent on this page. Open a panel for the details.';

const PILLAR_NAME: Record<StepPillar, string> = {
  speed: 'Speed',
  airead: 'AI readability',
  risk: 'Risk & SEO',
};
/** The text of the control that opens a step's panel. */
export const showLabel = (pillar: StepPillar): string => `Show in ${PILLAR_NAME[pillar]}`;

export const NOTHING_URGENT_PARTIAL =
  'Nothing urgent in what the scan could measure. Open a panel for the details.';
export const NOTHING_MEASURED =
  'Nothing was measured, so there are no steps yet. Open a panel to try again.';

/**
 * The line shown in place of the list when it is empty. `measured` is how many
 * of the three areas have a result. When a tile still asks for attention the
 * line names its panel instead of saying nothing is urgent.
 */
export function emptyLine(attention: readonly StepPillar[], measured = 3): string {
  if (measured === 0) return NOTHING_MEASURED;
  if (attention.length === 0) return measured < 3 ? NOTHING_URGENT_PARTIAL : NOTHING_URGENT;
  const names = attention.map((p) => PILLAR_NAME[p]);
  return `The scan has no single next step for this page. Open the ${listOf(names)} ${
    names.length === 1 ? 'panel' : 'panels'
  } for what it flagged.`;
}

// The report is scanner output; every field is read defensively.
/* eslint-disable @typescript-eslint/no-explicit-any */
type Report = Record<string, any> | null | undefined;

export interface SpeedScores {
  mobile: number | null;
  desktop: number | null;
}

function listOf(items: readonly string[]): string {
  if (items.length <= 1) return items.join('');
  return `${items.slice(0, -1).join(', ')} and ${items[items.length - 1]}`;
}

const strings = (v: unknown): string[] =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string' && x !== '') : [];

const cat = (name: string): string => `[data-scan-cat="${name}"]`;

/** Under this share of its points a category counts as weak. */
const WEAK = 0.5;
/** The scan calls a page attributable from this share of the citation points. */
const ATTRIBUTABLE = 0.7;
const MAX_NAMED_CRAWLERS = 3;

function ratio(c: any): number | null {
  return c && typeof c.score === 'number' && typeof c.max === 'number' && c.max > 0
    ? c.score / c.max
    : null;
}

function robotsStep(r: Record<string, any>, offers: boolean): Step | null {
  const cr = r.access?.crawlerRules;
  if (!cr || typeof cr !== 'object' || cr.parsed !== true) return null;
  const blocked = [...new Set([...strings(cr.training?.blocked), ...strings(cr.answer?.blocked)])];
  let text = '';
  if (cr.wildcardBlocksAll === true) {
    text =
      'Your robots.txt blocks every crawler that has no rule of its own from the home page. Remove that rule if you want AI crawlers to read the site.';
  } else if (blocked.length > 0) {
    const who =
      blocked.length <= MAX_NAMED_CRAWLERS ? listOf(blocked) : `${blocked.length} AI crawlers`;
    text = `Your robots.txt blocks ${who} from the home page. Remove ${
      blocked.length === 1 ? 'that rule' : 'those rules'
    } if you want AI crawlers to read the site.`;
  }
  if (!text) return null;
  return {
    id: 'robots',
    text,
    tag: offers ? TAG.server : TAG.settings,
    pillar: 'airead',
    target: cat('Crawler access'),
  };
}

const SEO_ORDER = [
  'title-js-only',
  'title-missing',
  'title-js-changed',
  'description-missing',
  'description-js-only',
  'canonical-js-only',
  'canonical-missing',
  'description-js-changed',
  'canonical-js-changed',
  'title-multiple',
  'description-multiple',
  'canonical-multiple',
];
const SEO_THING: Record<string, string> = {
  title: 'title',
  description: 'meta description',
  canonical: 'canonical link',
};

function seoStep(r: Record<string, any>): (Step & { defect: string }) | null {
  const sd = r.seoDefects;
  if (!lensMeasured(sd) || sd.verdict !== 'attention') return null;
  const defects = strings(sd.defects);
  const top = SEO_ORDER.find((id) => defects.includes(id)) ?? '';
  const base = { id: 'seo', pillar: 'risk' as const, target: '[data-risk-row="seoDefects"]' };
  const [group, ...rest] = top.split('-');
  const kind = rest.join('-');
  const thing = SEO_THING[group];
  if (thing && kind === 'missing') {
    return { ...base, defect: top, tag: TAG.editor, text: `This page has no ${thing}. Add one.` };
  }
  if (thing && kind === 'multiple') {
    return {
      ...base,
      defect: top,
      tag: TAG.editor,
      text: `This page has more than one ${thing}. Keep one.`,
    };
  }
  if (thing && kind === 'js-only') {
    return {
      ...base,
      defect: top,
      tag: TAG.developer,
      text: `This page’s ${thing} is only added by JavaScript. Put it in the HTML the server sends.`,
    };
  }
  if (thing && kind === 'js-changed') {
    return {
      ...base,
      defect: top,
      tag: TAG.developer,
      text: `JavaScript changes this page’s ${thing} after it loads. Put the final one in the HTML the server sends.`,
    };
  }
  const total = Number(sd.counts?.total) || defects.length;
  if (total <= 0) return null;
  return {
    ...base,
    defect: '',
    tag: TAG.developer,
    text: `The scan flagged ${plural(total, 'SEO issue')} in the head of this page. Open the Risk & SEO panel for the list.`,
  };
}

const ARTICLE = /Article|BlogPosting/i;

function citationStep(r: Record<string, any>, seoDefect: string): Step | null {
  const share = ratio(r.categories?.['Citation readiness']);
  const s = r.signals;
  if (share === null || share >= ATTRIBUTABLE || !s || typeof s !== 'object') return null;
  const missing: string[] = [];
  if (!s.title && seoDefect !== 'title-missing') missing.push('title');
  if (s.metaDescription === false && seoDefect !== 'description-missing') {
    missing.push('meta description');
  }
  if (s.canonical === false && seoDefect !== 'canonical-missing') missing.push('canonical link');
  // An author and a date belong on an article, so only an article is asked for them.
  if (strings(s.jsonLdTypes).some((t) => ARTICLE.test(t))) {
    if (s.author === false) missing.push('author name');
    if (s.published === false && s.modified === false) missing.push('publication date');
  }
  if (missing.length === 0) return null;
  return {
    id: 'citation',
    text: `This page has no ${listOf(missing)}. Add ${missing.length === 1 ? 'it' : 'them'}.`,
    tag: TAG.editor,
    pillar: 'airead',
    target: cat('Citation readiness'),
  };
}

function accessibilityStep(r: Record<string, any>): Step | null {
  const ax = r.accessibility;
  const total = Number(ax?.detail?.total) || 0;
  if (!ax || ax.available !== true || total <= 0) return null;
  const top = Array.isArray(ax.detail.topRules) ? ax.detail.topRules[0] : null;
  const help = top && typeof top.help === 'string' ? top.help.trim().replace(/\.$/, '') : '';
  return {
    id: 'accessibility',
    text: `Fix ${plural(total, 'accessibility issue')} the scan found${
      help ? `: ${help}` : ''
    }. These are automated checks, not an accessibility audit.`,
    tag: TAG.editor,
    pillar: 'airead',
    target: '[data-wedge="compliancefix"]',
  };
}

function speedStep(speed: SpeedScores | null | undefined): Step | null {
  if (!speed || speedStatus(speed.mobile, speed.desktop).state !== 'poor') return null;
  const where = typeof speed.mobile === 'number' ? 'mobile' : 'desktop';
  return {
    id: 'speed',
    text: `Speed is poor on ${where}: open the Speed panel for the audits PageSpeed Insights flagged.`,
    tag: TAG.developer,
    pillar: 'speed',
    target: '',
  };
}

/**
 * The steps for a result. `report` is null when the scan gave none and
 * `speed` is null while the speed result is not in. A step that needs server
 * access is left out on a hosted platform.
 */
export function nextSteps(report: Report, speed?: SpeedScores | null): NextSteps {
  const all: Step[] = [];
  const add = (step: Step | null) => {
    if (step) all.push(step);
  };
  if (report && typeof report === 'object') {
    const offers = serverOffersApply(report);
    const fidelity = report.categories?.['Content fidelity'];
    const thin = fidelity?.flag === 'thin';
    const share = ratio(fidelity);
    if (thin) {
      // The scan read next to nothing, so the other rows describe an empty or
      // a blocked page: the one useful step is to find out why.
      add({
        id: 'no-content',
        text: 'The scan found almost no text on this page, even after running JavaScript. Check that the page is public and does not block automated visitors, then scan it again.',
        tag: offers ? TAG.server : TAG.settings,
        pillar: 'airead',
        target: cat('Content fidelity'),
      });
    } else if (fidelity?.flag === 'csr-gap' || (share !== null && share < WEAK)) {
      add({
        id: 'content',
        text: 'Put this page’s main text in the HTML the server sends, so it does not depend on JavaScript.',
        tag: TAG.developer,
        pillar: 'airead',
        target: cat('Content fidelity'),
      });
    }
    add(robotsStep(report, offers));
    const seo = thin ? null : seoStep(report);
    add(seo);
    add(speedStep(speed));
    if (!thin) {
      const structured = ratio(report.categories?.['Structured data']);
      if (structured !== null && structured < WEAK) {
        add({
          id: 'structured-data',
          text: 'Add structured data that says what this page is: your organization, a product, an article or a list of questions and answers.',
          tag: TAG.editor,
          pillar: 'airead',
          target: cat('Structured data'),
        });
      }
      add(citationStep(report, seo?.defect ?? ''));
      add(accessibilityStep(report));
    }
  } else {
    add(speedStep(speed));
  }
  const kept = all
    .filter((s) => s.tag !== TAG.server || serverOffersApply(report))
    .map((s) => ({ id: s.id, text: s.text, tag: s.tag, pillar: s.pillar, target: s.target }));
  return { steps: kept.slice(0, MAX_STEPS), total: kept.length };
}
