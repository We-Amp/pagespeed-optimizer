// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The one lead form: which topic chips a result offers, and how the form
// becomes one request. Pure: no DOM, no network. A submission is one body that
// lists every ticked topic and the topics the scan flagged. Topics are opt-in:
// a chip whose check fired is offered and marked, never ticked for the visitor.

import {
  agentPassCtaApplies,
  complianceFixCtaApplies,
  consentEnforcementCtaApplies,
  edgeSeoCtaApplies,
  lensSignalFor,
  markdownOriginCtaApplies,
  pageIntegrityCtaApplies,
  responseFirewallCtaApplies,
  thirdPartyFreezeCtaApplies,
  tollboothCtaApplies,
} from './demand.mjs';
import { AUDIT_PRICE_EUR, formatEur } from '../../data/product-facts.mjs';
import type { TileState } from './status';

/** The most topics a visitor may tick in one submission. */
export const MAX_TOPICS = 4;
/** The most topics one request lists, ticked and flagged together. */
export const MAX_LISTED_TOPICS = 6;
/** The endpoint's limit for one request body, in UTF-8 bytes. */
export const MAX_BODY_BYTES = 8192;

export interface Chip {
  /** The contact topic; for gated chips it is also the wedge. */
  id: string;
  label: string;
  /** The wedge posted; empty for the ungated chips. */
  wedge: string;
  gated: boolean;
  selected: boolean;
}

export type Report = Record<string, unknown> & { url?: string; grade?: string; score?: number };

interface ChipDef {
  id: string;
  label: string;
  fires: (r: Report) => boolean;
}

// Display order follows the panels: AI readability, Risk & SEO, then Speed.
const GATED: ChipDef[] = [
  {
    id: 'tollbooth',
    label: 'Origin controls (who reads / verifies)',
    fires: (r) => tollboothCtaApplies(r.agentVerifiability),
  },
  {
    id: 'agentpass',
    label: 'Signed-agent verification (prove which agent is which)',
    fires: (r) => agentPassCtaApplies(r.signedAgentVerification),
  },
  {
    id: 'compliancefix',
    label: 'Accessibility remediation',
    fires: (r) => complianceFixCtaApplies(r.accessibility),
  },
  {
    id: 'consent-enforcement',
    label: 'Consent enforcement',
    fires: (r) => consentEnforcementCtaApplies(r.preConsentLeak),
  },
  {
    id: 'page-integrity',
    label: 'Script control',
    fires: (r) => pageIntegrityCtaApplies(r.scriptInventory),
  },
  {
    id: 'third-party-freeze',
    label: 'Pinning third-party scripts',
    fires: (r) => thirdPartyFreezeCtaApplies(r.scriptInventory),
  },
  {
    id: 'edge-seo',
    label: 'SEO fixes at the server',
    fires: (r) => edgeSeoCtaApplies(r.seoDefects),
  },
  {
    id: 'response-firewall',
    label: 'Leaked errors and outdated software',
    fires: (r) => responseFirewallCtaApplies(r.responseExposure),
  },
  {
    id: 'markdown-origin',
    label: 'Serving AI agents a Markdown copy at the origin',
    fires: (r) => markdownOriginCtaApplies(r.agentSignals),
  },
];

export const CONSULTING = { id: 'consulting', label: 'Help applying the speed fixes' };
export const AGENTS = { id: 'agents', label: 'AI agents and crawlers' };
export const AUDIT = {
  id: 'audit',
  label: `Agent-readiness audit (${formatEur(AUDIT_PRICE_EUR)} per site, excl. VAT)`,
};

/** Shown on a chip whose check fired on the scanned page; part of its accessible name. */
export const FOUND_MARKER = '(flagged by your scan)';

/**
 * The chips for a result, in display order. Gated chips appear only when
 * their gate fires; they come first and are never pre-selected, on either
 * page. The speed-help chip appears once the speed result has settled (`speed`
 * is null while it is still checking) and is pre-selected only when Speed is
 * Poor; the agents and audit chips are always offered, unselected. With no
 * report only the ungated chips remain.
 */
export function chipsFor(
  report: Report | null | undefined,
  speed: TileState | null | undefined,
): Chip[] {
  const chips: Chip[] = [];
  if (report) {
    for (const def of GATED) {
      if (def.fires(report)) {
        chips.push({
          id: def.id,
          label: def.label,
          wedge: def.id,
          gated: true,
          selected: false,
        });
      }
    }
  }
  if (speed) {
    chips.push({ ...CONSULTING, wedge: '', gated: false, selected: speed === 'poor' });
  }
  chips.push({ ...AGENTS, wedge: '', gated: false, selected: false });
  if (report) chips.push({ ...AUDIT, wedge: '', gated: false, selected: false });
  return chips;
}

/** One topic of a request: `chosen` is true when the visitor ticked it. */
export interface Topic {
  id: string;
  /** The wedge of a chip whose check fired; null for the other chips. */
  wedge: string | null;
  chosen: boolean;
}

export interface LeadForm {
  email: string;
  /** The optional count of sites, as typed. */
  sites?: string;
  note?: string;
  /** The scanned URL, used when the report has none (or no report exists). */
  url: string;
  /** Speed scores, when measured. */
  mobile?: number | null;
  desktop?: number | null;
  /** The page-load time (ms since the epoch), so the handler can judge a too-fast submit. */
  t?: number;
  /** One id per submit attempt; a retry of the same attempt sends it again. */
  submissionId: string;
}

/** The request body. */
export type LeadPayload = Record<string, unknown> & {
  topic: string;
  wedge: string;
  message: string;
  topics: Topic[];
  lensSignals: Record<string, unknown>;
  facts: Record<string, string | number>;
};

/** A new submission id: 32 characters from [0-9a-f]. */
export function newSubmissionId(): string {
  const bytes = new Uint8Array(16);
  globalThis.crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

/**
 * The topics of a request. First every ticked chip, in display order; with
 * nothing ticked, the general question takes that place. Then every chip whose
 * check fired and that the visitor left unticked. The list holds at most
 * MAX_LISTED_TOPICS entries, and a ticked chip is never the one left out.
 */
export function topicsFor(chips: Chip[], ticked: readonly string[]): Topic[] {
  const picked = chips.filter((c) => ticked.includes(c.id));
  const chosen: Topic[] =
    picked.length > 0
      ? picked.map((c) => ({ id: c.id, wedge: c.gated ? c.wedge : null, chosen: true }))
      : [{ id: AGENTS.id, wedge: null, chosen: true }];
  const found: Topic[] = chips
    .filter((c) => c.gated && !ticked.includes(c.id))
    .map((c) => ({ id: c.id, wedge: c.wedge, chosen: false }));
  return [...chosen, ...found.slice(0, Math.max(0, MAX_LISTED_TOPICS - chosen.length))];
}

const present = (v: unknown): v is string => typeof v === 'string' && v.trim() !== '';

/** Flat facts about the result; a value that is not known is left out. */
export function factsFor(form: LeadForm, report: Report | null): Record<string, string | number> {
  const facts: Record<string, string | number> = {};
  if (report && present(report.grade)) facts.grade = report.grade;
  if (report && typeof report.score === 'number') facts.score = report.score;
  if (typeof form.mobile === 'number') facts.speedMobile = form.mobile;
  if (typeof form.desktop === 'number') facts.speedDesktop = form.desktop;
  const sites = parseInt((form.sites ?? '').trim(), 10);
  if (Number.isInteger(sites) && sites > 0) facts.sites = sites;
  // A cached or failed result may have no platform reading at all.
  const platform = report?.platform as
    | { id?: unknown; name?: unknown; originControl?: unknown; evidence?: unknown }
    | null
    | undefined;
  if (platform && typeof platform === 'object') {
    if (present(platform.id)) facts.platform = platform.id;
    if (present(platform.name)) facts.platformName = platform.name;
    if (present(platform.originControl) && platform.originControl !== 'unknown') {
      facts.originControl = platform.originControl;
    }
    const evidence = Array.isArray(platform.evidence) ? platform.evidence : [];
    const server = evidence
      .map((e) => (e && typeof e === 'object' ? (e as { id?: unknown }).id : null))
      .find((id): id is string => typeof id === 'string' && id.startsWith('server-'));
    if (server && server.length > 'server-'.length) facts.server = server.slice('server-'.length);
  }
  return facts;
}

const encoder = new TextEncoder();
/** The size of a request body as sent, in UTF-8 bytes. */
export function bodyBytes(payload: unknown): number {
  return encoder.encode(JSON.stringify(payload)).length;
}

/**
 * The one request for a submission. `ticked` holds the ids of the ticked
 * chips. The message is the visitor's note and nothing else. Signals are sent
 * for the ticked topics that have one. A body over MAX_BODY_BYTES loses its
 * signals, largest first, and then the unticked topics; the email address, the
 * note and the ticked topics always stay.
 */
export function payloadForSubmission(
  chips: Chip[],
  ticked: readonly string[],
  form: LeadForm,
  report: Report | null,
): LeadPayload {
  const topics = topicsFor(chips, ticked);
  const first = topics[0];
  const lensSignals: Record<string, unknown> = {};
  if (report) {
    for (const topic of topics) {
      if (!topic.chosen || !topic.wedge) continue;
      let signal: unknown = null;
      try {
        signal = lensSignalFor(topic.wedge, report);
      } catch {
        // The result lacks the part this signal is read from: send none.
      }
      if (signal) lensSignals[topic.wedge] = signal;
    }
  }
  const payload: LeadPayload = {
    email: form.email,
    name: '',
    url: (report && report.url) || form.url,
    ...(form.t === undefined ? {} : { t: form.t }),
    origin: 'scan-result',
    submissionId: form.submissionId,
    topic: first.id,
    wedge: first.wedge ?? '',
    message: (form.note ?? '').trim(),
    topics,
    lensSignals,
    facts: factsFor(form, report),
  };
  while (bodyBytes(payload) > MAX_BODY_BYTES) {
    const largest = Object.keys(payload.lensSignals)
      .map((wedge) => ({ wedge, size: bodyBytes(payload.lensSignals[wedge]) }))
      .sort((a, b) => b.size - a.size)[0];
    if (largest) {
      delete payload.lensSignals[largest.wedge];
    } else if (payload.topics.some((t) => !t.chosen)) {
      payload.topics = payload.topics.filter((t) => t.chosen);
    } else {
      break;
    }
  }
  return payload;
}
