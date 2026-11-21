// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Whether the server-side parts of a result apply to the scanned site, and the
// line that says so at the top of the result. The scan names a hosted platform
// or it names nothing: it never says a site runs on its own server. Pure.

import { originTopicsApply } from './demand.mjs';
import { guidanceFor, type PlatformAction } from '../../data/platform-guidance';

interface Platform {
  id?: unknown;
  name?: unknown;
  originControl?: unknown;
}
type Report = Record<string, unknown> | null | undefined;

function platformOf(report: Report): Platform | null {
  const p = report?.platform;
  return p && typeof p === 'object' ? (p as Platform) : null;
}

/**
 * The one test for everything that needs a web server the visitor runs: the
 * gated topics, the audit, the download row, the "planned" sentences and the
 * configuration. False only when the scan found a hosted platform whose
 * owners have no control of the server.
 */
export function serverOffersApply(report: Report): boolean {
  return originTopicsApply(platformOf(report));
}

/** The platform's display name, or an empty string when the scan gave none. */
export function platformName(report: Report): string {
  const name = platformOf(report)?.name;
  return typeof name === 'string' ? name.trim() : '';
}

export const FIT_LINE = 'The server topics below apply only if you run the web server yourself.';

const GENERIC_ACTIONS =
  'put your services, location and contact details in page text, not in images; fill in each page’s title and description; check that the SEO settings do not block AI crawlers.';

/** The first action on every hosted platform; it needs no platform setting. */
export const PAGE_TEXT_ACTION =
  'Put your services, location and contact details in page text, not in images.';
/** How many of a platform's own actions follow it. */
const MAX_PLATFORM_ACTIONS = 2;

const COUNT_WORD = ['', 'One thing', 'Two things', 'Three things'];

/** One listed action; a platform's own action links the help page it was checked against. */
export type FitAction = Pick<PlatformAction, 'text'> & Partial<PlatformAction>;

export interface Fit {
  /** False for a hosted platform: nothing server-side is offered. */
  offers: boolean;
  /** The notice paragraph (hosted) or the muted line; empty when there is nothing to say. */
  text: string;
  /** The actions listed under the notice; empty when the notice names them itself. */
  actions: FitAction[];
}

/**
 * What the top of a result says about fit. Without a report (the scan failed)
 * it says nothing: the muted line belongs to a result, whose audit line and
 * topics are the server offers it is about.
 */
export function fitFor(report: Report): Fit {
  if (!report) return { offers: true, text: '', actions: [] };
  if (serverOffersApply(report)) return { offers: true, text: FIT_LINE, actions: [] };
  const name = platformName(report);
  const head =
    (name
      ? `This site runs on ${name}, a hosted platform.`
      : 'This site runs on a hosted platform.') +
    ' mod_pagespeed installs on a web server you run yourself, so it cannot be used here, and the server topics do not apply.';
  const where = name ? `in ${name}` : 'on the platform';
  const own = (guidanceFor(platformOf(report)?.id)?.actions ?? []).slice(0, MAX_PLATFORM_ACTIONS);
  if (own.length) {
    const actions: FitAction[] = [{ text: PAGE_TEXT_ACTION }, ...own];
    return {
      offers: false,
      text: `${head} ${COUNT_WORD[actions.length]} you can do ${where}:`,
      actions,
    };
  }
  return {
    offers: false,
    text: `${head} Three things you can do ${where}: ${GENERIC_ACTIONS}`,
    actions: [],
  };
}

/** The notice as one string, for the live region. */
export function fitSpoken(fit: Fit): string {
  return [fit.text, ...fit.actions.map((a) => a.text)].join(' ');
}
