// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// generate-llms.mjs — deterministic generator for the machine-readable surfaces
// that AI agents/crawlers read: public/llms.txt, public/llms-full.txt and
// public/.well-known/ai-plugin.json.
//
//   prose/json template (scripts/llms-templates/*.tmpl, with {{TOKEN}} holes)
//     + facts (src/data/product-facts.mjs LLMS_TOKENS)
//     => public/llms*.txt + public/.well-known/ai-plugin.json
//
// Runs at prebuild (see package.json "prebuild") so the published files always
// match the single source of truth. The byte-equivalence drift guard in
// test/sync/llms-generated.test.ts imports generateContent() from here and
// fails CI if the committed public/llms*.txt differ from freshly generated
// output — i.e. if a fact changed without regenerating (`npm run gen:llms`), or
// if someone hand-edited a generated file.
//
// Usage:
//   node scripts/generate-llms.mjs            # write public/llms*.txt
//   node scripts/generate-llms.mjs --check    # exit 1 if committed files are stale (no write)

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve } from 'node:path';
import { LLMS_TOKENS } from '../src/data/product-facts.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const TEMPLATE_DIR = resolve(__dirname, 'llms-templates');
export const PUBLIC_DIR = resolve(__dirname, '..', 'public');

export const TARGETS = [
  { template: 'llms.txt.tmpl', output: 'llms.txt' },
  { template: 'llms-full.txt.tmpl', output: 'llms-full.txt' },
  // The AI-plugin manifest is JSON, but it goes through the exact same
  // template+token+byte-equivalence path (only the drift-prone version/price/
  // variant facts are tokenized; token values never contain double quotes or
  // backslashes — see the PRICING_TIERS note rule in product-facts.mjs — so
  // substitution into a JSON string is safe; the drift test JSON.parses the
  // result, which would catch a violation). Output is under .well-known/.
  { template: 'ai-plugin.json.tmpl', output: '.well-known/ai-plugin.json' },
];

const UNRESOLVED_TOKEN = /\{\{\s*[A-Za-z0-9_]+\s*\}\}/;

/**
 * Substitute every {{TOKEN}} in `template` with its value from LLMS_TOKENS.
 * Throws if any {{TOKEN}} remains unresolved (typo or missing fact) so a bad
 * token can never silently ship into a public file.
 * @param {string} template
 * @param {string} name  source filename, for error messages
 * @returns {string}
 */
export function render(template, name) {
  let out = template;
  for (const [token, value] of Object.entries(LLMS_TOKENS)) {
    out = out.replaceAll(`{{${token}}}`, value);
  }
  const leftover = out.match(UNRESOLVED_TOKEN);
  if (leftover) {
    throw new Error(
      `${name}: unresolved template token ${leftover[0]} — add it to LLMS_TOKENS ` +
        `in src/data/product-facts.mjs (or fix the typo in the template).`,
    );
  }
  return out;
}

/**
 * Render every target from its template. Returns { outputFilename: content }.
 * This is the single render path shared by the CLI and the drift-guard test.
 * @returns {Record<string, string>}
 */
export function generateContent() {
  /** @type {Record<string, string>} */
  const result = {};
  for (const { template, output } of TARGETS) {
    const tmpl = readFileSync(resolve(TEMPLATE_DIR, template), 'utf8');
    result[output] = render(tmpl, template);
  }
  return result;
}

function main() {
  const checkOnly = process.argv.includes('--check');
  const content = generateContent();
  let stale = 0;

  for (const { output } of TARGETS) {
    const outPath = resolve(PUBLIC_DIR, output);
    const rendered = content[output];

    if (checkOnly) {
      let current = '';
      try {
        current = readFileSync(outPath, 'utf8');
      } catch {
        /* missing file counts as stale */
      }
      if (current !== rendered) {
        stale++;
        console.error(`STALE: public/${output} differs from generated output. Run \`npm run gen:llms\`.`);
      } else {
        console.log(`OK: public/${output} is up to date.`);
      }
    } else {
      writeFileSync(outPath, rendered);
      console.log(`Generated public/${output} (${rendered.length} bytes).`);
    }
  }

  if (checkOnly && stale > 0) {
    process.exit(1);
  }
}

// Run the CLI only when invoked directly (`node scripts/generate-llms.mjs`),
// not when imported by the drift-guard test.
if (import.meta.url === pathToFileURL(process.argv[1] || '').href) {
  main();
}
