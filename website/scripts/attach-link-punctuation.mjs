// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Keep punctuation attached to the inline link or inline code it follows.
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
 * Inline <code> is rewritten the same way; the content of <pre>, <script>,
 * <style> and <textarea> is not.
 */

import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));

const OPEN = /<!--|<(pre|script|style|textarea)(?=[\s/>])[^>]*>/gi;
// ASCII whitespace only: a non-breaking space before punctuation is deliberate.
// The lookbehind makes a match start at the beginning of a whitespace run, so
// long runs stay linear.
const GAP = /(?<![ \t\n\r\f])[ \t\n\r\f]*<\/(a|code)>[ \t\n\r\f]+(?=[.,;:)!?])/g;

/** @param {string} html */
export function attachLinkPunctuation(html) {
  const fix = (/** @type {string} */ text) => text.replace(GAP, '</$1>');
  let out = '';
  let pos = 0;
  OPEN.lastIndex = 0;
  for (let open = OPEN.exec(html); open; open = OPEN.exec(html)) {
    out += fix(html.slice(pos, open.index));
    // A comment may mention a raw-text tag; it runs to the next -->.
    const close = open[1] ? new RegExp(`</${open[1]}(?=[\\s/>])[^>]*>`, 'gi') : /-->/g;
    close.lastIndex = OPEN.lastIndex;
    const end = close.exec(html);
    // An unclosed raw-text element runs to the end of the document.
    const stop = end ? close.lastIndex : html.length;
    out += html.slice(open.index, stop);
    pos = stop;
    OPEN.lastIndex = stop;
  }
  return out + fix(html.slice(pos));
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
