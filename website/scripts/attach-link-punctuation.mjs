// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Keep punctuation attached to the inline link it follows.
 *
 * The formatter puts every closing </a> on its own line and the sentence
 * punctuation after it on the next, so the HTML source holds
 * `</a>\n  .` and the page shows "Privacy policy ." with a gap, and the
 * underline running into it. The source cannot hold the adjacent form: the
 * formatter rewrites it on the next run. So the built pages are normalized
 * here, as part of `postbuild` and before the search index is built: the
 * whitespace between a link's end and a following . , ; : ) ! ? is removed, as
 * is trailing whitespace inside the link, which would be underlined.
 *
 * Text inside <pre>, <script>, <style> and <textarea> is left alone.
 */

import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));

const LITERAL = /<(pre|script|style|textarea)\b[\s\S]*?<\/\1>/gi;
const GAP = /\s*<\/a>\s+(?=[.,;:)!?])/g;

/** @param {string} html */
export function attachLinkPunctuation(html) {
  let out = '';
  let last = 0;
  for (const m of html.matchAll(LITERAL)) {
    out += html.slice(last, m.index).replace(GAP, '</a>') + m[0];
    last = m.index + m[0].length;
  }
  return out + html.slice(last).replace(GAP, '</a>');
}

/** @param {string} dir @returns {string[]} */
function htmlFiles(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? htmlFiles(p) : p.endsWith('.html') ? [p] : [];
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  if (!existsSync(DIST)) {
    console.error('attach-link-punctuation: no dist/client; run astro build first');
    process.exit(1);
  }
  let changed = 0;
  for (const file of htmlFiles(DIST)) {
    const before = readFileSync(file, 'utf8');
    const after = attachLinkPunctuation(before);
    if (after !== before) {
      writeFileSync(file, after);
      changed++;
    }
  }
  console.log(`attach-link-punctuation: normalized ${changed} pages`);
}
