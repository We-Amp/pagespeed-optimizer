// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pure row model for the Risk & SEO panel: for each of the four lenses, the
// state chip, the one verdict sentence, and (only when the lens's gate fires)
// the "what we are building" sentences with the link and its event name.
// The verdict and card sentences of the first three lenses are the approved
// wording of the v1 results page. The gate predicates come from demand.mjs.

import {
  consentEnforcementCtaApplies,
  edgeSeoCtaApplies,
  pageIntegrityCtaApplies,
  responseFirewallCtaApplies,
  thirdPartyFreezeCtaApplies,
} from './demand.mjs';
import { RISK_LENSES, lensMeasured } from './status';

export type RowState = 'attention' | 'clean' | 'none';

export interface RiskLink {
  href: string;
  label: string;
  /** The umami event the link carries (names unchanged from v1). */
  event: string;
}

export interface RiskRow {
  key: (typeof RISK_LENSES)[number]['key'];
  name: string;
  state: RowState;
  /** The chip word, always visible. */
  chip: string;
  /** The one verdict sentence, or the not-measured note. */
  sentence: string;
  /** "What we are building" sentences; empty unless the lens's gate fires. */
  building: string[];
  link: RiskLink | null;
}

const DEFAULT_NOTE = 'Not measured: the site blocked the scanner.';
const ERROR_NOTE = 'Not measured: the scanner could not complete this check.';

const NAMES: Record<RiskRow['key'], string> = {
  preConsentLeak: 'Pre-consent leak',
  scriptInventory: 'Script inventory',
  seoDefects: 'SEO defects',
  responseExposure: 'Response exposure',
};

// The lens objects are scanner output; every field is read defensively.
/* eslint-disable @typescript-eslint/no-explicit-any */
type Lens = Record<string, any>;

// ---- Pre-consent leak (v1 wording) -------------------------------------

function pclVerdict(pcl: Lens): string {
  if (pcl.verdict === 'unknown') {
    return 'We could not render this page, so we could not observe which third parties it contacts before consent.';
  }
  if (pcl.trackerCount === 0) {
    return (
      'No tracker requests observed before any interaction (' +
      pcl.thirdPartyCount +
      ' third-party host(s) contacted, none in a tracker category). Fresh browser, no cookies, no storage; heuristic host list — not legal advice.'
    );
  }
  const trackers: Lens[] = Array.isArray(pcl.thirdPartyHosts)
    ? pcl.thirdPartyHosts.filter((h: Lens) => h.tracker)
    : [];
  const ex = trackers
    .slice(0, 3)
    .map((h) => h.host + ' (' + h.category + ')')
    .join(', ');
  const vendor = pcl.cmpDetected && pcl.cmpDetected.detected ? pcl.cmpDetected.vendor : null;
  return (
    pcl.trackerCount +
    ' tracker host(s) contacted before any interaction, in a fresh browser with no consent given' +
    (ex ? ': ' + ex : '') +
    (pcl.trackerCount > 3 ? ', …' : '') +
    '.' +
    (vendor
      ? ' A consent banner (' +
        vendor +
        ') is present, but these requests fire before it can be answered.'
      : '') +
    ' Heuristic host list — not legal advice or a compliance certificate.'
  );
}

// ---- Script inventory (v1 wording) -------------------------------------

function hostsOf(list: unknown): string {
  const seen: string[] = [];
  (Array.isArray(list) ? list : []).forEach((x: Lens) => {
    if (x && !seen.includes(x.host)) seen.push(x.host);
  });
  return seen.slice(0, 3).join(', ');
}

function siReasonText(si: Lens): string {
  if (si.verdict !== 'attention') return '';
  const reasons: string[] = Array.isArray(si.reasons) ? si.reasons : [];
  const bits: string[] = [];
  if (reasons.includes('sri-missing')) {
    bits.push(
      (si.sriMissing || []).length +
        ' script(s) from public CDNs without an integrity hash (' +
        hostsOf(si.sriMissing) +
        ')',
    );
  }
  if (reasons.includes('floating-version')) {
    bits.push(
      (si.floating || []).length +
        ' CDN script(s) with no pinned version (' +
        hostsOf(si.floating) +
        ')',
    );
  }
  if (reasons.includes('duplicate-library')) {
    bits.push(
      'the same library at more than one version: ' +
        (si.duplicates || [])
          .slice(0, 3)
          .map((d: Lens) => d.library + ' ' + (d.versions || []).join(' + '))
          .join(', '),
    );
  }
  if (reasons.includes('unlisted-origin')) {
    bits.push(
      'script hosts not on our list of known hosts: ' +
        (si.scriptHosts || [])
          .filter((h: Lens) => h.party === 'third' && !h.listed)
          .slice(0, 3)
          .map((h: Lens) => h.host)
          .join(', '),
    );
  }
  return bits.length ? ' Flagged: ' + bits.join('; ') + '.' : '';
}

function siVerdict(si: Lens): string {
  if (si.verdict === 'unknown') {
    return 'We could not render this page, so we could not inventory its scripts.';
  }
  const t = si.totals || {};
  const csp = si.csp || {};
  return (
    t.scripts +
    ' script(s) · ' +
    t.thirdPartyExternal +
    ' from other hosts · ' +
    t.thirdPartyWithIntegrity +
    ' with integrity · CSP: ' +
    (csp.enforced ? 'enforced' : csp.reportOnly ? 'report-only' : 'none') +
    '.' +
    siReasonText(si) +
    ' Heuristic host list; not an assessment against any standard.'
  );
}

// ---- SEO defects (v1 wording) ------------------------------------------

const SD_GROUPS: [string, string][] = [
  ['canonical', 'canonical'],
  ['hreflang', 'hreflang'],
  ['title', 'title'],
  ['description', 'meta description'],
  ['jsonld', 'structured data'],
];
const SD_NOT_MEASURED: Record<string, string> = {
  'canonical-js': 'canonical',
  'hreflang-js': 'hreflang',
  'title-js': 'title',
  'description-js': 'meta description',
  'jsonld-js': 'structured data',
};

function sdFiredGroups(sd: Lens): [string, string][] {
  return sd.status === 'ok' && sd.counts ? SD_GROUPS.filter((g) => sd.counts[g[0]] > 0) : [];
}

function sdServedBy(sd: Lens): string {
  const dl = sd.delivery;
  return (
    'Server header: ' +
    ((dl && dl.server) || 'none') +
    '; CDN headers seen: ' +
    ((dl && dl.cdn) || 'none') +
    '.'
  );
}

function sdPartialText(sd: Lens): string {
  if (!(sd.status === 'ok' && sd.staticCompared === false)) return '';
  const names = (Array.isArray(sd.notMeasured) ? sd.notMeasured : [])
    .map((k: string) => SD_NOT_MEASURED[k])
    .filter(Boolean);
  if (!names.length) return '';
  return (
    'The no-JavaScript fetch was refused, so only the rendered page was checked. ' +
    'Not measured: whether the ' +
    names.join(', ') +
    ' in the served HTML match the page after JavaScript.'
  );
}

function sdVerdict(sd: Lens): string {
  if (sd.verdict === 'unknown') {
    return sdPartialText(sd)
      ? 'The no-JavaScript fetch was refused, so only the rendered page was checked and it showed no defect; the comparison with the served HTML did not run.'
      : 'We could not render this page, so we could not check its head.';
  }
  if (sd.verdict === 'clean' && sd.staticCompared === false) {
    return 'No defects found on the rendered page; the comparison with the served HTML did not run. One page only; not a ranking assessment.';
  }
  if (sd.verdict === 'clean') {
    return 'No defects found: the canonical, title, meta description, hreflang and structured data in the served HTML match the page after JavaScript. One page only; not a ranking assessment.';
  }
  const counts = sd.counts || {};
  return (
    (counts.total ?? 0) +
    ' issue(s): ' +
    sdFiredGroups(sd)
      .map((g) => g[1] + ' ' + counts[g[0]])
      .join(', ') +
    '. ' +
    sdServedBy(sd) +
    ' One page only; not a ranking assessment.'
  );
}

// ---- Response exposure --------------------------------------------------
// The wording follows the scanner's own response-exposure line.

const EOL_PRODUCT: Record<string, string> = {
  drupal: 'Drupal',
  php: 'PHP',
  apache: 'Apache',
  'microsoft-iis': 'IIS',
  joomla: 'Joomla',
};
const MISSING_HEADER: Record<string, string> = {
  hsts: 'HSTS',
  nosniff: 'nosniff',
  framing: 'framing',
};

function rxVerdict(rx: Lens): string {
  const nm: string[] = Array.isArray(rx.notMeasured) ? rx.notMeasured : [];
  const notChecked = nm.length
    ? ' Not checked: ' +
      nm
        .map((k) =>
          k === 'static-html' ? 'the HTML as served without JavaScript' : 'the response headers',
        )
        .join(', ') +
      '.'
    : '';
  if (rx.verdict === 'unknown') {
    return 'We could not read enough of this response to say.' + notChecked;
  }
  const leaks = Array.isArray(rx.leaks) ? rx.leaks.length : 0;
  const tells = (Array.isArray(rx.tells) ? rx.tells : []).filter(
    (t: Lens) => t && t.version,
  ).length;
  const list: Lens[] = Array.isArray(rx.endOfLife) ? rx.endOfLife : [];
  const eol = [
    ...list
      .filter((e) => e.basis === 'eol-date' && EOL_PRODUCT[e.product])
      .map((e) => EOL_PRODUCT[e.product] + ' ' + e.line),
    ...(list.some((e) => e.basis === 'no-fixes')
      ? ['mod_pagespeed (no longer receives fixes)']
      : []),
  ];
  const missing = (Array.isArray(rx.headers?.missing) ? rx.headers.missing : [])
    .map((m: string) => MISSING_HEADER[m] || m)
    .join(', ');
  return (
    leaks +
    ' error-output pattern(s) · ' +
    tells +
    ' version tell(s) · end of life: ' +
    (eol.length ? eol.join(', ') : 'none seen') +
    ' · missing: ' +
    (missing || 'none') +
    '.' +
    notChecked +
    ' One page only; not a security assessment.'
  );
}

export const RX_BUILDING =
  'A response-firewall pack for mod_pagespeed 2.1 is planned. It would mask leaked stack traces, strip version tells and add missing security headers in the response, on your own server. No pack ships today.';

// ---- Rows ----------------------------------------------------------------

function notMeasuredRow(key: RiskRow['key'], lens: Lens): RiskRow {
  const fallback = lens.status === 'error' ? ERROR_NOTE : DEFAULT_NOTE;
  const note = typeof lens.note === 'string' && lens.note.trim() ? lens.note.trim() : fallback;
  return {
    key,
    name: NAMES[key],
    state: 'none',
    chip: 'Not measured',
    sentence: note,
    building: [],
    link: null,
  };
}

function buildingFor(
  key: RiskRow['key'],
  lens: Lens,
): { building: string[]; link: RiskLink | null } {
  switch (key) {
    case 'preConsentLeak':
      return consentEnforcementCtaApplies(lens)
        ? {
            building: [
              'With consent enforcement at the origin, third-party tags stay inert at the server until the consent cookie grants them.',
            ],
            link: {
              href: '/platform/consent/',
              label: 'Consent enforcement at the origin →',
              event: 'airead-cta-consent',
            },
          }
        : { building: [], link: null };
    case 'scriptInventory': {
      const building: string[] = [];
      if (pageIntegrityCtaApplies(lens)) {
        building.push(
          'We are building a server module that keeps a list of approved scripts at your origin.',
        );
      }
      if (thirdPartyFreezeCtaApplies(lens)) {
        const unpinned = lens.totals.thirdPartyExternal - lens.totals.thirdPartyWithIntegrity;
        building.push(
          unpinned +
            ' third-party script(s) on this page load without an integrity hash, so their content can change upstream without notice. We are building a way to serve each one at a version you approved until you accept the update.',
        );
      }
      return building.length
        ? {
            building,
            link: {
              href: '/platform/',
              label: 'Origin modules we are building →',
              event: 'airead-cta-page-integrity',
            },
          }
        : { building: [], link: null };
    }
    case 'seoDefects':
      return edgeSeoCtaApplies(lens)
        ? {
            building: [
              'Issues like these are the kind a rule on the server could correct in the HTML it serves, without a CMS release. We are building that rule set.',
            ],
            link: {
              href: '/platform/edge-seo/',
              label: 'Edge SEO at the origin →',
              event: 'airead-cta-edge-seo',
            },
          }
        : { building: [], link: null };
    case 'responseExposure':
      return responseFirewallCtaApplies(lens)
        ? {
            building: [RX_BUILDING],
            link: {
              href: '/platform/#pack-response-firewall',
              label: 'Transform packs: early access →',
              event: 'airead-cta-response-firewall',
            },
          }
        : { building: [], link: null };
  }
}

const VERDICT: Record<RiskRow['key'], (lens: Lens) => string> = {
  preConsentLeak: pclVerdict,
  scriptInventory: siVerdict,
  seoDefects: sdVerdict,
  responseExposure: rxVerdict,
};

/**
 * The rows to show, in panel order. A missing or disabled lens has no row; a
 * lens that did not run (blocked, error) or could not reach a verdict is a
 * muted not-measured row.
 */
export function riskRows(report: Record<string, unknown> | null | undefined): RiskRow[] {
  const rows: RiskRow[] = [];
  if (!report) return rows;
  for (const { key, flagged } of RISK_LENSES) {
    const lens = report[key] as Lens | null | undefined;
    if (!lens || typeof lens !== 'object' || lens.status === 'disabled') continue;
    if (lens.status !== 'ok') {
      rows.push(notMeasuredRow(key, lens));
      continue;
    }
    if (!lensMeasured(lens)) {
      // ok but no verdict: the lens's own "could not render" sentence.
      rows.push({
        key,
        name: NAMES[key],
        state: 'none',
        chip: 'Not measured',
        sentence: VERDICT[key]({ ...lens, verdict: 'unknown' }),
        building: [],
        link: null,
      });
      continue;
    }
    const attention = lens.verdict === flagged;
    const { building, link } = buildingFor(key, lens);
    rows.push({
      key,
      name: NAMES[key],
      state: attention ? 'attention' : 'clean',
      chip: attention ? 'Attention' : 'Clean',
      sentence: VERDICT[key](lens),
      building,
      link,
    });
  }
  return rows;
}
