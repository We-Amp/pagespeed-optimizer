// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The lifecycle sentence for a detected mod_pagespeed version, keyed on the
// version band. Pure; an unparsable version has no sentence.

const CURRENT = 'the current release';

/** The frozen 2.0 line receives security fixes only until this date. */
export const FROZEN_UNTIL = '2027-02-07';

/** Parse "major.minor[.rest]"; null when the string does not start that way. */
export function parseVersion(version: unknown): { major: number; minor: number } | null {
  if (typeof version !== 'string') return null;
  const m = /^\s*v?(\d{1,4})\.(\d{1,4})(?![\d])/.exec(version);
  return m ? { major: Number(m[1]), minor: Number(m[2]) } : null;
}

/**
 * Below 1.15: no longer receives fixes. 1.15 up to the 1.x lines before 2.0:
 * supported. 2.0.x: frozen, security fixes only. 2.1 and later: no sentence.
 */
export function lifecycleSentence(version: unknown): string {
  const v = parseVersion(version);
  if (!v) return '';
  const label = `mod_pagespeed ${String(version).trim()}`;
  if (v.major < 1 || (v.major === 1 && v.minor < 15)) {
    return `${label}: this line no longer receives fixes. Upgrade in place to ${CURRENT}.`;
  }
  if (v.major === 1) {
    return `${label}: supported line. Update to ${CURRENT} for the latest fixes.`;
  }
  if (v.major === 2 && v.minor === 0) {
    return `${label}: frozen line, security fixes only until ${FROZEN_UNTIL}. Upgrade in place to ${CURRENT}.`;
  }
  return '';
}
