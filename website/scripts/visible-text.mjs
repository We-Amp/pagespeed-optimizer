// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// visible-text.mjs — dump the normalized visible text of a built page for
// copy-identity diffs. Reads the prerendered HTML from dist/client (no server
// needed), extracts the text of <header>, <main> and <footer> (script, style
// and template content excluded), and prints one trimmed text node per line.
// Run it on the same path before and after a change and diff the two dumps:
// the diff is the exact set of visible-string changes.
//
// Usage: node scripts/visible-text.mjs <path> [--root <dir>]
//   node scripts/visible-text.mjs /
//   node scripts/visible-text.mjs /docs/ --root dist/client

import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { parse } from 'parse5';

const args = process.argv.slice(2);
let pagePath = null;
let root = 'dist/client';
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--root') root = args[++i];
  else if (!pagePath) pagePath = args[i];
  else {
    console.error(`unexpected argument: ${args[i]}`);
    process.exit(2);
  }
}
if (!pagePath) {
  console.error('usage: node scripts/visible-text.mjs <path> [--root <dir>]');
  process.exit(2);
}

// "/" -> dist/client/index.html; "/docs/" -> dist/client/docs/index.html.
// File-like paths (with an extension) map directly.
const file = /\.[a-z0-9]+$/i.test(pagePath)
  ? join(root, pagePath)
  : join(root, pagePath, 'index.html');

let html;
try {
  html = readFileSync(resolve(file), 'utf8');
} catch {
  console.error(`cannot read ${file} — is the site built?`);
  process.exit(2);
}

const doc = parse(html);

const SKIP = new Set(['script', 'style', 'template']);
const WANT = new Set(['header', 'main', 'footer']);

function textOf(node, out) {
  if (node.nodeName === '#text') {
    const t = node.value.replace(/\s+/g, ' ').trim();
    if (t) out.push(t);
    return;
  }
  if (!node.tagName || SKIP.has(node.tagName)) return;
  for (const child of node.childNodes ?? []) textOf(child, out);
}

function* walk(node) {
  if (node.tagName && WANT.has(node.tagName)) {
    yield node;
    return; // no nested header/main/footer to find inside
  }
  for (const child of node.childNodes ?? []) yield* walk(child);
}

const lines = [];
for (const region of walk(doc)) {
  const inner = [];
  textOf(region, inner);
  lines.push(`--- <${region.tagName}> ---`, ...inner);
}
process.stdout.write(lines.join('\n') + '\n');
