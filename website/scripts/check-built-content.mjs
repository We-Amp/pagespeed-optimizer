#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Built-output content lint over the production build.
 *
 * Runs after `npm run build` (npm run check:content) over every .html file
 * under dist/ — the pages the node adapter wrote to dist/client/ plus
 * 404/500 — and checks what a search engine sees, not what the source
 * intends:
 *
 *   title-count          exactly one <title> per page (every page)
 *   title-length         15–65 chars after entity decoding (indexable pages)
 *   description-present  <meta name="description"> exists (indexable pages)
 *   description-length   50–165 chars after entity decoding (indexable pages)
 *   description-duplicate  no two indexable pages share one description
 *   h1-count             exactly one <h1> per page (every page)
 *   term-drift-daemon    "daemon" absent from visible text (indexable pages);
 *                        the process is the worker. <script>, <style>, <code>
 *                        and <pre> contents are stripped first, so code
 *                        samples may name anything.
 *   product-naming       "ModPageSpeed 2.0"/"mod_pagespeed 2.0" absent from
 *                        visible text (every page); the product line on this
 *                        site is mod_pagespeed 2.1, and 2.0 names the
 *                        predecessor — historical mentions belong in frozen
 *                        blog posts, which the allowlist covers.
 *   canonical            exactly one <link rel="canonical"> with an absolute
 *                        https://modpagespeed.com URL ending in "/" or a file
 *                        extension (indexable pages)
 *
 * Scope: dist/client/1.0/ is the frozen archive of the old site and is
 * skipped, like the other build-output gates skip it. Pages marked
 * <meta name="robots" content="noindex..."> skip only the rules marked
 * "indexable" above.
 *
 * Justified exceptions live in scripts/content-lint-allow.json (URL path →
 * reason) and exempt that path from every rule; the reason names the rule(s)
 * the entry covers. The lint warns about allowlist entries that match no
 * built page, so a redirect cannot leave a stale entry behind.
 *
 * Every failure prints `<url>  <rule>  <detail>`. Error-level failures exit
 * 1; warn-level rules only print (a rule calibrated to more noise than it is
 * worth is set to warn in RULES below rather than mass-editing copy).
 */
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, relative, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST = resolve(WEBSITE_ROOT, 'dist');
const SITE_ORIGIN = 'https://modpagespeed.com';

const TITLE_MIN = 15;
const TITLE_MAX = 65;
const DESCRIPTION_MIN = 50;
const DESCRIPTION_MAX = 165;

/** Rule table. `indexableOnly` rules skip noindex pages. */
export const RULES = {
  'title-count': { level: 'error', indexableOnly: false },
  'title-length': { level: 'error', indexableOnly: true },
  'description-present': { level: 'error', indexableOnly: true },
  'description-length': { level: 'error', indexableOnly: true },
  'description-duplicate': { level: 'error', indexableOnly: true },
  'h1-count': { level: 'error', indexableOnly: false },
  'term-drift-daemon': { level: 'error', indexableOnly: true },
  // Warn-only: on the 2026-10 calibration the string appeared in the visible
  // text of 79 of 331 pages — overwhelmingly the docs sidebar's "Migrate
  // from ModPageSpeed 2.0" item, a legitimate reference to the predecessor
  // line. Visible-text matching cannot tell that history from drift; making
  // the rule blocking needs a narrower scope (own-page content, not shared
  // navigation) first. Printed so a new occurrence is still seen in the log.
  'product-naming': { level: 'warn', indexableOnly: false },
  canonical: { level: 'error', indexableOnly: true },
};

const NAMED_ENTITIES = {
  amp: '&',
  lt: '<',
  gt: '>',
  quot: '"',
  apos: "'",
  nbsp: ' ',
  mdash: '—',
  ndash: '–',
  hellip: '…',
  rsquo: '’',
  lsquo: '‘',
  ldquo: '“',
  rdquo: '”',
  copy: '©',
  reg: '®',
  trade: '™',
  middot: '·',
};

/** Decode the entities Astro emits into title/description/body text. */
export function decodeEntities(text) {
  return text.replace(/&(#x?[0-9a-f]+|[a-z]+);/gi, (whole, name) => {
    if (name[0] === '#') {
      const code = name[1] === 'x' || name[1] === 'X' ? parseInt(name.slice(2), 16) : parseInt(name.slice(1), 10);
      return Number.isFinite(code) && code > 0 ? String.fromCodePoint(code) : whole;
    }
    return NAMED_ENTITIES[name.toLowerCase()] ?? whole;
  });
}

/** Collapse whitespace runs to one space and trim — the SERP-visible form. */
export function collapseWhitespace(text) {
  return text.replace(/\s+/g, ' ').trim();
}

const length = (s) => [...s].length;

/**
 * Attribute value of a tag, honoring whichever quote delimiter opened it
 * (Astro leaves a literal ' inside a double-quoted attribute, so a character
 * class of "both quotes" would cut the value short).
 */
function attrValue(tag, name) {
  return new RegExp(`\\b${name}=(["'])([\\s\\S]*?)\\1`, 'i').exec(tag)?.[2] ?? '';
}

/** Title count and decoded text of the first <title> in <head>. */
export function titleInfo(html) {
  const head = /<head\b[^>]*>([\s\S]*?)<\/head\s*>/i.exec(html)?.[1] ?? html;
  const matches = [...head.matchAll(/<title\b[^>]*>([\s\S]*?)<\/title\s*>/gi)];
  return {
    count: matches.length,
    text: decodeEntities(collapseWhitespace(matches[0]?.[1] ?? '')),
  };
}

/** Description tag count and decoded content of the first one. */
export function descriptionInfo(html) {
  const tags = [...html.matchAll(/<meta\b[^>]*>/gi)]
    .map((m) => m[0])
    .filter((tag) => attrValue(tag, 'name').toLowerCase() === 'description');
  return {
    count: tags.length,
    text: decodeEntities(collapseWhitespace(tags[0] ? attrValue(tags[0], 'content') : '')),
  };
}

/** A page is non-indexable when robots says so, however the value is ordered. */
export function isNoindex(html) {
  return [...html.matchAll(/<meta\b[^>]*>/gi)]
    .map((m) => m[0])
    .some((tag) => attrValue(tag, 'name').toLowerCase() === 'robots' && /\bnoindex\b/i.test(attrValue(tag, 'content')));
}

/** Remove script/style/pre/code contents and comments: not visible text. */
export function stripNonVisible(html) {
  return html
    .replace(/<script\b[^>]*>[\s\S]*?<\/script\s*>/gi, ' ')
    .replace(/<style\b[^>]*>[\s\S]*?<\/style\s*>/gi, ' ')
    .replace(/<pre\b[^>]*>[\s\S]*?<\/pre\s*>/gi, ' ')
    .replace(/<code\b[^>]*>[\s\S]*?<\/code\s*>/gi, ' ')
    .replace(/<!--[\s\S]*?-->/g, ' ');
}

/** The text a visitor reads: body, tags stripped, entities decoded. */
export function visibleText(html) {
  const body = /<body\b[^>]*>([\s\S]*?)<\/body\s*>/i.exec(html)?.[1] ?? html;
  return collapseWhitespace(decodeEntities(stripNonVisible(body).replace(/<[^>]+>/g, ' ')));
}

/** Count <h1> opening tags outside script/style/comments. */
export function h1Count(html) {
  return (stripNonVisible(html).match(/<h1(?=[\s/>])/gi) ?? []).length;
}

/** hrefs of every <link rel="canonical"> ('' when the tag has no href). */
export function canonicalHrefs(html) {
  return [...html.matchAll(/<link\b[^>]*>/gi)]
    .map((m) => m[0])
    .filter((tag) => /\bcanonical\b/i.test(attrValue(tag, 'rel')))
    .map((tag) => attrValue(tag, 'href'));
}

/** null when the canonical URL is well-formed, else why it is not. */
export function canonicalProblem(href) {
  let url;
  try {
    url = new URL(href);
  } catch {
    return `not an absolute URL: "${href}"`;
  }
  if (url.origin !== SITE_ORIGIN) return `not on ${SITE_ORIGIN}: "${href}"`;
  if (!url.pathname.endsWith('/') && !/\.[a-z0-9]+$/i.test(url.pathname)) {
    return `path does not end in "/" or a file extension: "${href}"`;
  }
  return null;
}

const DAEMON_RE = /\bdaemon\b/i;
const NAMING_RE = /\b(?:mod_pagespeed|modpagespeed)\s*2\.0\b/i;

/** A ~100-char window around the match, so the failure names the copy. */
function around(text, match) {
  const start = Math.max(0, match.index - 45);
  return text.slice(start, Math.min(text.length, match.index + match[0].length + 55)).trim();
}

/**
 * Lint one page. Returns [{rule, message}] without the url (the caller
 * knows it); duplicate descriptions need the whole page set and are
 * computed by lintPages.
 */
export function lintPage(html) {
  const failures = [];
  const noindex = isNoindex(html);
  const indexable = !noindex;
  const fails = (rule, message) => failures.push({ rule, message });

  const title = titleInfo(html);
  if (title.count !== 1) {
    fails('title-count', title.count === 0 ? 'no <title>' : `${title.count} <title> elements`);
  } else if (indexable) {
    const n = length(title.text);
    if (n < TITLE_MIN || n > TITLE_MAX) {
      fails('title-length', `${n} chars (want ${TITLE_MIN}-${TITLE_MAX}): "${title.text}"`);
    }
  }

  const description = descriptionInfo(html);
  if (indexable) {
    if (description.count === 0 || description.text === '') {
      fails('description-present', 'no <meta name="description"> content');
    } else {
      const n = length(description.text);
      if (n < DESCRIPTION_MIN || n > DESCRIPTION_MAX) {
        fails(
          'description-length',
          `${n} chars (want ${DESCRIPTION_MIN}-${DESCRIPTION_MAX}): "${description.text}"`,
        );
      }
    }
  }

  const h1s = h1Count(html);
  if (h1s !== 1) fails('h1-count', h1s === 0 ? 'no <h1>' : `${h1s} <h1> elements`);

  if (indexable) {
    const text = visibleText(html);
    const daemon = DAEMON_RE.exec(text);
    if (daemon) fails('term-drift-daemon', `…${around(text, daemon)}…`);
  }
  const namingText = visibleText(html);
  const naming = NAMING_RE.exec(namingText);
  if (naming) {
    fails('product-naming', `"mod_pagespeed 2.0" names the predecessor line: …${around(namingText, naming)}…`);
  }

  if (indexable) {
    const hrefs = canonicalHrefs(html);
    if (hrefs.length !== 1) {
      fails('canonical', hrefs.length === 0 ? 'no <link rel="canonical">' : `${hrefs.length} <link rel="canonical">`);
    } else {
      const problem = canonicalProblem(hrefs[0]);
      if (problem) fails('canonical', problem);
    }
  }

  return failures;
}

/**
 * Lint a whole build. pages: [{url, html}]; allowlist: {urlPath: reason}.
 * Returns {failures: [{url, rule, level, message}], warnings: [...]}.
 */
export function lintPages(pages, allowlist = {}) {
  const failures = [];
  const warnings = [];

  const known = new Set(pages.map((p) => p.url));
  for (const [url, reason] of Object.entries(allowlist)) {
    if (!known.has(url)) {
      warnings.push({ url, rule: 'allowlist', message: `allowlist entry matches no built page: ${reason}` });
    }
  }

  const active = pages.filter((p) => {
    if (allowlist[p.url]) {
      warnings.push({ url: p.url, rule: 'allowlist', message: `exempted: ${allowlist[p.url]}` });
      return false;
    }
    return true;
  });

  const byDescription = new Map();
  for (const page of active) {
    for (const { rule, message } of lintPage(page.html)) {
      const level = RULES[rule]?.level ?? 'error';
      const failure = { url: page.url, rule, level, message };
      if (level === 'warn') warnings.push(failure);
      else failures.push(failure);
    }
    if (!isNoindex(page.html)) {
      const d = descriptionInfo(page.html).text;
      if (d) {
        const group = byDescription.get(d) ?? [];
        group.push(page.url);
        byDescription.set(d, group);
      }
    }
  }
  for (const [d, urls] of byDescription) {
    if (urls.length > 1) {
      const failure = {
        url: urls[0],
        rule: 'description-duplicate',
        level: RULES['description-duplicate'].level,
        message: `identical description on ${urls.length} pages (${urls.join(', ')}): "${d.slice(0, 120)}"`,
      };
      if (failure.level === 'warn') warnings.push(failure);
      else failures.push(failure);
    }
  }

  return { failures, warnings };
}

/** URL path of a built file: dist/client/docs/a/index.html -> /docs/a/. */
export function urlOf(distRoot, file) {
  const segments = relative(distRoot, file).split(/[\\/]/);
  if (segments[0] === 'client') segments.shift();
  let directory = false;
  if (segments[segments.length - 1] === 'index.html') {
    segments.pop();
    directory = true;
  }
  const url = `/${segments.map(encodeURIComponent).join('/')}`;
  return directory && !url.endsWith('/') ? `${url}/` : url;
}

/** Collect every *.html under distRoot except the frozen /1.0/ archive. */
export function collectPages(distRoot) {
  const out = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      const p = join(dir, name);
      if (statSync(p).isDirectory()) {
        if (name === '1.0' && relative(distRoot, dir) === 'client') continue;
        walk(p);
      } else if (name.endsWith('.html')) {
        out.push(p);
      }
    }
  };
  walk(distRoot);
  return out;
}

export function loadAllowlist(file) {
  if (!existsSync(file)) return {};
  const parsed = JSON.parse(readFileSync(file, 'utf8'));
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new Error(`${file}: expected a JSON object of {urlPath: reason}`);
  }
  return parsed;
}

function summary(failures, warnings, pageCount) {
  const byRule = new Map();
  for (const list of [failures, warnings]) {
    for (const f of list) {
      const entry = byRule.get(f.rule) ?? { error: 0, warn: 0 };
      if (list === failures) entry.error += 1;
      else entry.warn += 1;
      byRule.set(f.rule, entry);
    }
  }
  const lines = [`content-lint: ${pageCount} pages checked`];
  for (const [rule, { level }] of Object.entries(RULES)) {
    const n = byRule.get(rule) ?? { error: 0, warn: 0 };
    lines.push(`  ${rule.padEnd(22)} ${String(n.error).padStart(3)} errors  ${String(n.warn).padStart(3)} warnings  (${level})`);
  }
  const allow = byRule.get('allowlist') ?? { warn: 0 };
  if (allow.warn) lines.push(`  ${'allowlist'.padEnd(22)} ${String(allow.warn).padStart(3)} exemptions/stale notes`);
  return lines.join('\n');
}

async function main() {
  if (!existsSync(DIST)) {
    console.error(`check-built-content: ${DIST} does not exist; run \`npm run build\` first.`);
    process.exit(1);
  }
  const files = collectPages(DIST);
  if (files.length === 0) {
    console.error(`check-built-content: no .html files under ${DIST}.`);
    process.exit(1);
  }
  const allowlist = loadAllowlist(join(WEBSITE_ROOT, 'scripts', 'content-lint-allow.json'));
  const pages = files.map((file) => ({
    url: urlOf(DIST, file),
    html: readFileSync(file, 'utf8'),
  }));

  const { failures, warnings } = lintPages(pages, allowlist);

  const print = (f) => console.log(`  ${f.url}  ${f.rule}  ${f.message}`);
  if (warnings.length) {
    console.log('warnings (do not fail the build):');
    warnings.forEach(print);
  }
  if (failures.length) {
    console.log('failures:');
    failures.forEach(print);
  }
  console.log(summary(failures, warnings, pages.length));
  if (failures.length > 0) {
    console.error(`content-lint: ${failures.length} error(s).`);
    process.exit(1);
  }
  console.log('content-lint: OK');
}

// Run as a CLI only when invoked directly (the vitest suite imports the rest).
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  await main();
}
