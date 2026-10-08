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

import { readFileSync, statSync } from 'node:fs';
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
    const layout = readFileSync(path('../src/layouts/BaseLayout.astro'), 'utf8');
    expect(layout).toContain('href="/fonts/inter-variable-subset.woff2"');
  });
});

describe('metric-matched font fallbacks', () => {
  for (const family of ['Inter Fallback', 'IBM Plex Mono Fallback']) {
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

  it('the font stacks name each fallback right after its web font', () => {
    expect(css).toMatch(/--font-sans:\s*'Inter',\s*'Inter Fallback',/);
    expect(css).toMatch(/--font-mono:\s*'IBM Plex Mono',\s*'IBM Plex Mono Fallback',/);
  });
});
