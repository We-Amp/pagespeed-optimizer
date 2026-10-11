// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The lifecycle sentence for a detected module version, keyed on the version
// band. Pure; an unparsable version, or one at or past the current package
// version, has no sentence. "Current" is the package version the site already
// publishes, never a literal here.

import { MODULE_PACKAGE_VERSION } from '../../data/product-facts.mjs';

export interface ParsedVersion {
  major: number;
  minor: number;
  /** The leading numeric parts, e.g. [1, 17, 0] for "1.17.0-rc1". */
  parts: number[];
}

/**
 * Parse the leading dotted numbers of a version ("v1.1.0+r24", "1.17.0-rc1",
 * "2.0.3"); null when it does not start with "major.minor". Suffixes after the
 * numbers (+rN, -rc, -dev) are ignored. Components are compared as integers,
 * so 1.1 and 1.14 are different lines.
 */
export function parseVersion(version: unknown): ParsedVersion | null {
  if (typeof version !== 'string') return null;
  const m = /^\s*v?(\d{1,4}(?:\.\d{1,5}){1,3})(?![\d.])/.exec(version);
  if (!m) return null;
  const parts = m[1].split('.').map(Number);
  return { major: parts[0], minor: parts[1], parts };
}

function atLeast(a: number[], b: number[]): boolean {
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const d = (a[i] ?? 0) - (b[i] ?? 0);
    if (d !== 0) return d > 0;
  }
  return true;
}

/**
 * - 2.1 and later, and any 1.x at or past the current package version: none.
 * - 2.0.x (ModPageSpeed): a pointer to the migration guide, no support claim.
 * - 1.1.x (our line, renumbered to 1.15.0) and 1.15 up to below current:
 *   a newer release is available.
 * - 1.0 through 1.14 (the archived upstream line, 1.1 excepted): no fixes.
 */
export function lifecycleSentence(
  version: unknown,
  current: string = MODULE_PACKAGE_VERSION,
): string {
  const v = parseVersion(version);
  if (!v) return '';
  const label = String(version).trim();
  if (v.major >= 3) return '';
  if (v.major === 2) {
    return v.minor === 0
      ? `ModPageSpeed ${label}: Migrate from ModPageSpeed 2.0 explains whether your setup moves to 2.1 today.`
      : '';
  }
  const cur = parseVersion(current);
  const renumbered = v.major === 1 && v.minor === 1;
  if (!renumbered && cur && atLeast(v.parts, cur.parts)) return '';
  if (renumbered || (v.major === 1 && v.minor >= 15)) {
    return `mod_pagespeed ${label}: a newer release is available. Update recommended.`;
  }
  return `mod_pagespeed ${label}: this line no longer receives fixes. Move to the current release; configuration carries over.`;
}
