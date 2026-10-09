// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// palette-lint.test.ts — the cool-palette ban, enforced. The site's palette
// moved to the instrument tokens (see src/styles/global.css): no blue-family
// Tailwind utilities and no blue rgba() literals anywhere in src/ or the
// top-level public/*.svg marks. The warm remainder (stone-/emerald-/amber-)
// is being retired package by package; this test prints the remaining counts
// per file as a table without failing on them, so the number only goes down.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const websiteRoot = fileURLToPath(new URL('..', import.meta.url));

const SCAN_EXTENSIONS = new Set([
  '.astro',
  '.css',
  '.ts',
  '.tsx',
  '.js',
  '.mjs',
  '.cjs',
  '.svg',
  '.md',
  '.mdx',
]);

function scanFiles(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    if (statSync(full).isDirectory()) {
      if (entry === 'node_modules' || entry.startsWith('.')) continue;
      out.push(...scanFiles(full));
    } else if (SCAN_EXTENSIONS.has(entry.slice(entry.lastIndexOf('.')))) {
      out.push(full);
    }
  }
  return out;
}

const files = [
  ...scanFiles(join(websiteRoot, 'src')),
  // The top-level public marks only; public/1.0 is the frozen archive and
  // public/images carries third-party logos with their own brand colours.
  ...readdirSync(join(websiteRoot, 'public'))
    .filter((f) => f.endsWith('.svg'))
    .map((f) => join(websiteRoot, 'public', f)),
];

// A Tailwind utility from the banned cool families: blue-*, sky-*, indigo-*,
// purple-* (the full shade number, so the violation listing reads blue-500,
// not blue-5).
const BANNED_UTILITY = /\b(?:blue|sky|indigo|purple)-\d+/;

// A decimal-channel rgba() whose blue channel clearly dominates — e.g. the
// retired accent rgba(29, 78, 216, …). Near-neutral overlays like
// rgba(7, 8, 10, 0.72) stay legal: dominance has to exceed 24 per channel.
const RGBA = /\brgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,/g;

// The warm remainder, reported but not (yet) banned.
const WARM = /\b(?:stone|emerald|amber)-\d+/g;

interface Violation {
  file: string;
  line: number;
  match: string;
}

const violations: Violation[] = [];
const warmCounts = new Map<string, number>();

for (const file of files) {
  const rel = relative(websiteRoot, file);
  const text = readFileSync(file, 'utf8');
  const lines = text.split('\n');

  lines.forEach((line, i) => {
    const util = line.match(BANNED_UTILITY);
    if (util) {
      violations.push({ file: rel, line: i + 1, match: util[0] });
    }
    for (const m of line.matchAll(RGBA)) {
      const [r, g, b] = [Number(m[1]), Number(m[2]), Number(m[3])];
      if (b > r + 24 && b > g + 24) {
        violations.push({ file: rel, line: i + 1, match: m[0] });
      }
    }
  });

  const warm = text.match(WARM);
  if (warm) warmCounts.set(rel, warm.length);
}

describe('palette lint', () => {
  it('no blue-family utilities or blue rgba() literals in src/ or public/*.svg', () => {
    const listing = violations.map((v) => `  ${v.file}:${v.line}  ${v.match}`).join('\n');
    expect(violations, `cool-palette violations:\n${listing}`).toEqual([]);
  });

  it('prints the remaining warm-palette counts (informational)', () => {
    const rows = [...warmCounts.entries()].sort((a, b) => b[1] - a[1]);
    if (rows.length === 0) {
      console.log('\nremaining stone-/emerald-/amber- matches: none\n');
      return;
    }
    const width = Math.max(...rows.map(([f]) => f.length), 4);
    const total = rows.reduce((n, [, c]) => n + c, 0);
    console.log(
      '\nremaining stone-/emerald-/amber- matches (not a failure):\n' +
        rows.map(([f, c]) => `  ${f.padEnd(width)}  ${c}`).join('\n') +
        `\n  ${'total'.padEnd(width)}  ${total}\n`,
    );
  });
});
