// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// font-budget.test.ts — guards the two font decisions that keep the mobile
// Lighthouse score and the layout stable:
//
//   1. The self-hosted Inter subset stays small. It is preloaded on every page,
//      so its bytes sit on the critical path of the largest contentful paint.
//      The budget is the shipped size plus 10%; a regenerated subset that grows
//      past it (a wider character set, a restored weight range, an extra
//      layout feature) has to be a deliberate decision, made here.
//   2. global.css carries metric-matched fallback faces for Inter and IBM Plex
//      Mono, and the font stacks name them right after the web font, so the
//      swap from the local fallback to the web font does not shift text.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, it, expect } from 'vitest';

const path = (rel: string) => fileURLToPath(new URL(rel, import.meta.url));
const css = readFileSync(path('../src/styles/global.css'), 'utf8');

// 41,952 bytes shipped, times 1.1.
const INTER_BUDGET_BYTES = 46_147;

function fontFace(family: string): string {
  const faces = css.match(/@font-face\s*\{[^}]*\}/g) ?? [];
  const face = faces.find((f) => new RegExp(`font-family:\\s*'${family}'`).test(f));
  expect(face, `@font-face for '${family}' in global.css`).toBeDefined();
  return face as string;
}

describe('Inter subset budget', () => {
  it('the woff2 stays under budget', () => {
    const size = statSync(path('../public/fonts/inter-variable-subset.woff2')).size;
    expect(size).toBeLessThanOrEqual(INTER_BUDGET_BYTES);
  });

  it('the @font-face and the preload name the same file', () => {
    expect(fontFace('Inter')).toContain("url('/fonts/inter-variable-subset.woff2')");
    const head = readFileSync(path('../src/layouts/parts/HeadMeta.astro'), 'utf8');
    expect(head).toContain('href="/fonts/inter-variable-subset.woff2"');
  });
});

describe('metric-matched font fallbacks', () => {
  for (const family of [
    'Inter Fallback',
    'Inter Fallback Roboto',
    'IBM Plex Mono Fallback',
    'IBM Plex Mono Fallback Liberation',
  ]) {
    it(`${family} carries size-adjust and the vertical metric overrides`, () => {
      const face = fontFace(family);
      expect(face).toMatch(/src:\s*local\(/);
      for (const descriptor of [
        'size-adjust',
        'ascent-override',
        'descent-override',
        'line-gap-override',
      ]) {
        expect(face, descriptor).toMatch(new RegExp(`${descriptor}:\\s*\\d+(\\.\\d+)?%`));
      }
    });
  }

  it('the fallbacks cover macOS, Windows, Linux and Android system fonts', () => {
    const sources = (family: string) => fontFace(family).match(/local\('([^']+)'\)/g) ?? [];
    const inter = [...sources('Inter Fallback'), ...sources('Inter Fallback Roboto')].join(' ');
    for (const local of ['Arial', 'Helvetica', 'Liberation Sans', 'Roboto']) {
      expect(inter).toContain(`local('${local}')`);
    }
    const mono = [
      ...sources('IBM Plex Mono Fallback'),
      ...sources('IBM Plex Mono Fallback Liberation'),
    ].join(' ');
    for (const local of ['Menlo', 'DejaVu Sans Mono', 'Liberation Mono', 'Droid Sans Mono']) {
      expect(mono).toContain(`local('${local}')`);
    }
  });

  it('the font stacks name the fallbacks right after their web font', () => {
    expect(css).toMatch(/--font-sans:\s*'Inter',\s*'Inter Fallback',\s*'Inter Fallback Roboto',/);
    expect(css).toMatch(
      /--font-mono:\s*'IBM Plex Mono',\s*'IBM Plex Mono Fallback',\s*'IBM Plex Mono Fallback Liberation',/,
    );
  });

  it('the Inter license ships next to the font', () => {
    const license = readFileSync(path('../public/fonts/Inter-OFL.txt'), 'utf8');
    expect(license).toContain('The Inter Project Authors');
    expect(license).toContain('SIL OPEN FONT LICENSE Version 1.1');
  });
});

// Every IBM Plex Mono weight is a file the page downloads. The site uses two,
// 400 and 600; a mono element asking for 500 would silently render at 400 (the
// nearest face), so the class lists may not pair font-mono with font-medium.
describe('IBM Plex Mono weights', () => {
  it('global.css declares the 400 and 600 faces only', () => {
    const faces = (css.match(/@font-face\s*\{[^}]*\}/g) ?? []).filter((f) =>
      /font-family:\s*'IBM Plex Mono'/.test(f),
    );
    const weights = faces.map((f) => f.match(/font-weight:\s*(\d+)/)?.[1]).sort();
    expect(weights).toEqual(['400', '600']);
  });

  it('no class list pairs font-mono with font-medium', () => {
    const offenders: string[] = [];
    const walk = (dir: string) => {
      for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const full = `${dir}/${entry.name}`;
        if (entry.isDirectory()) walk(full);
        else if (/\.(astro|ts|mjs)$/.test(entry.name)) {
          const text = readFileSync(full, 'utf8');
          for (const m of text.matchAll(/class(?:Name)?=["'`]([^"'`]*)["'`]/g)) {
            const classes = m[1].split(/\s+/);
            if (classes.includes('font-mono') && classes.includes('font-medium')) {
              offenders.push(`${full}: ${m[1]}`);
            }
          }
        }
      }
    };
    walk(path('../src'));
    expect(offenders).toEqual([]);
  });
});
