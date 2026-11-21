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

const COUNT_WORD = ['', 'One thing', 'Two things', 'Three things'];

export interface Fit {
  /** False for a hosted platform: nothing server-side is offered. */
  offers: boolean;
  /** The notice paragraph (hosted) or the muted line (everything else). */
  text: string;
  /** Platform-specific actions shown as a list under the notice; empty otherwise. */
  actions: PlatformAction[];
}

/** What the top of a result says about fit. */
export function fitFor(report: Report): Fit {
  if (serverOffersApply(report)) return { offers: true, text: FIT_LINE, actions: [] };
  const name = platformName(report);
  const head =
    (name
      ? `This site runs on ${name}, a hosted platform.`
      : 'This site runs on a hosted platform.') +
    ' mod_pagespeed installs on a web server you run yourself, so it cannot be used here, and the server topics do not apply.';
  const where = name ? `in ${name}` : 'on the platform';
  const actions = (guidanceFor(platformOf(report)?.id)?.actions ?? []).slice(0, 3);
  if (actions.length) {
    const lead = `${COUNT_WORD[actions.length]} you can do ${where}:`;
    return { offers: false, text: `${head} ${lead}`, actions };
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
