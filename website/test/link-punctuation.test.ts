// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// link-punctuation.test.ts — the formatter writes a closing </a> and the
// punctuation after it on separate lines, which renders as "Privacy policy ."
// with the underline running into the gap. scripts/attach-link-punctuation.mjs
// closes that gap in the built pages (postbuild). Two layers:
//
//   1. Unit (always runs): the normalizer joins the pieces and leaves
//      literal blocks alone.
//   2. Built output (runs after `npm run build`, skips itself otherwise, like
//      meta-length-gate.test.ts): no page has </a>, whitespace, then . , ; : ) ! ?

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { attachLinkPunctuation } from '../scripts/attach-link-punctuation.mjs';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));
const GAP = /<\/a>\s+[.,;:)!?]/;

function htmlFiles(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? htmlFiles(p) : p.endsWith('.html') ? [p] : [];
  });
}

describe('attachLinkPunctuation (unit)', () => {
  it('joins a link and the punctuation after it, inside and outside the link', () => {
    const out = attachLinkPunctuation('<p>See <a href="/p/">\n  Privacy policy\n</a>\n.\n</p>');
    expect(out).toBe('<p>See <a href="/p/">\n  Privacy policy</a>.\n</p>');
  });

  it('leaves words after a link and literal blocks alone', () => {
    const html = '<a href="/x/">x</a> and <pre><a>y</a>\n.</pre>';
    expect(attachLinkPunctuation(html)).toBe(html);
  });
});

describe.skipIf(!existsSync(DIST))('built pages (after npm run build)', () => {
  it('no page separates a link from the punctuation that follows it', () => {
    const offenders = htmlFiles(DIST).filter((f) => GAP.test(readFileSync(f, 'utf8')));
    expect(offenders.map((f) => path.relative(DIST, f))).toEqual([]);
  });
});
