// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Rewrites the generated blocks of the installed-base census post from the census
// dataset, so no number in the post is typed by hand. Blocks are delimited by
//   <!-- census:begin NAME -->  …  <!-- census:end NAME -->
// Usage: node scripts/generate-census-blocks.mjs --date 2026-09-01 [--check]
//
// The rewritten post is passed through Prettier with the site's own config, so the
// output is already what `npm run format` produces and the two never fight.
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import * as prettier from 'prettier';

const args = process.argv.slice(2);
const dateIdx = args.indexOf('--date');
const date = dateIdx >= 0 ? args[dateIdx + 1] : undefined;
const check = args.includes('--check');
if (!date) throw new Error('--date YYYY-MM-01 is required');

const root = resolve(import.meta.dirname, '..');
const post = resolve(root, 'src/content/blog/pagespeed-installed-base-2026.md');
const census = JSON.parse(readFileSync(resolve(root, `src/data/census/${date}.json`), 'utf8'));
const svg = (name) =>
  readFileSync(resolve(root, `src/data/census/charts/${date}/${name}.svg`), 'utf8').trim();
// A <figure> line opens a CommonMark HTML block that runs to the next blank line, so the
// whole SVG (which has no blank lines) reaches the page as raw HTML, untouched by Markdown.
const figure = (name) => `<figure class="census-chart">\n${svg(name)}\n</figure>`;

const n = (v) => Number(v).toLocaleString('en-US');
const pct = (v, digits = 1) => `${(Number(v) * 100).toFixed(digits)}%`;
const month = (d) =>
  new Date(`${d}T00:00:00Z`).toLocaleString('en-US', {
    month: 'long',
    year: 'numeric',
    timeZone: 'UTC',
  });
const h = census.headline;
const t = census.tables;
const bucketName = {
  'google-era': 'Google-era',
  'incubator-era': 'Incubator-era',
  current: 'Current line',
  hidden: 'Version hidden',
};
const engineName = {
  'apache-module': 'Apache module (`X-Mod-Pagespeed`)',
  'nginx-module': 'nginx module (`X-Page-Speed`)',
};
const familyName = {
  apache: 'Apache',
  nginx: 'nginx',
  cloudflare: 'Cloudflare',
  litespeed: 'LiteSpeed',
  iis: 'IIS',
  other: 'Other',
  hidden: 'Not sent',
};
const signalName = {
  both: 'Header and technology detection',
  header_only: 'Header only',
  wappalyzer_only: 'Technology detection only',
};
const table = (header, rows) =>
  [
    `| ${header.join(' | ')} |`,
    `| ${header.map(() => '---').join(' | ')} |`,
    ...rows.map((r) => `| ${r.join(' | ')} |`),
  ].join('\n');

// Series: one row per crawl that is the first, a January crawl, or the latest.
const seriesByDate = new Map();
for (const r of t.series) {
  const row = seriesByDate.get(r.date) ?? {};
  row[r.bucket] = Number(r.origins);
  seriesByDate.set(r.date, row);
}
const seriesDates = [...seriesByDate.keys()].sort();
const seriesRows = seriesDates
  .filter((d, i) => i === 0 || i === seriesDates.length - 1 || d.slice(5, 7) === '01')
  .map((d) => {
    const r = seriesByDate.get(d);
    const buckets = ['google-era', 'incubator-era', 'current', 'hidden'].map((b) => r[b] ?? 0);
    return [month(d), ...buckets.map(n), n(buckets.reduce((a, b) => a + b, 0))];
  });

// Servers: per-engine totals and the two front-server pairings the prose cites.
const serverOrigins = (engine, family) =>
  t.servers
    .filter((r) => r.engine === engine && (family === undefined || r.server_family === family))
    .reduce((a, r) => a + Number(r.origins), 0);

// Detection: header-bearing pages are "both" + "header_only"; detected are "both" + "wappalyzer_only".
const signal = (s) => Number(t.detection.find((r) => r.signal === s)?.origins ?? 0);
const headerPages = signal('both') + signal('header_only');
const detectedPages = signal('both') + signal('wappalyzer_only');

const change = Number(h.change_first_to_latest_pct);

const blocks = {
  headline: `In the ${month(h.crawl_date)} crawl, HTTP Archive saw PageSpeed answering on **${n(h.latest_total)} origins**. ${n(h.versioned_total)} of them report a version; ${n(h.hidden_total)} send the header with the version hidden. In ${month(h.first_date)}, the first crawl in this census, the count was ${n(h.first_total)}: it has ${change < 0 ? 'fallen' : 'risen'} by ${Math.abs(change)}% since. The peak in this window was ${n(h.peak_total)} origins in ${month(h.peak_date)}.`,
  'security-numbers': `${pct(h.share_out_of_fixes)} of the origins that report a version run a build that no longer receives fixes: ${pct(h.share_google_of_versioned)} a Google-era build last updated in 2018, ${pct(h.share_incubator_of_versioned)} an incubator-era build last updated in 2020. ${n(h.current)} origins report the current line, which receives fixes. Update recommended.`,
  'series-chart': figure('series'),
  'series-table': table(
    ['Crawl', 'Google-era', 'Incubator-era', 'Current line', 'Version hidden', 'Total'],
    seriesRows,
  ),
  'distribution-chart': figure('distribution'),
  'ranks-chart': figure('ranks'),
  'networks-chart': figure('networks'),
  'distribution-table': table(
    ['Reported version', 'Era', 'Origins'],
    t.distribution.slice(0, 12).map((r) => [r.version, bucketName[r.bucket], n(r.origins)]),
  ),
  // The latest crawl has no unranked origins; an empty row is omitted rather than shown as 0 of 0.
  'ranks-table': table(
    ['Popularity rank', 'PageSpeed origins', 'All origins', 'Share'],
    t.ranks
      .filter((r) => Number(r.all_origins) > 0)
      .map((r) => [r.rank, n(r.pagespeed_origins), n(r.all_origins), pct(r.share, 2)]),
  ),
  'networks-table': table(
    ['Network', 'Share of PageSpeed origins'],
    t.networks_top10.map((r) => [r.network, `${r.share_pct}%`]),
  ),
  'servers-numbers': `On the desktop crawl, ${n(serverOrigins('apache-module'))} origins send \`X-Mod-Pagespeed\`, the Apache module's header, and ${n(serverOrigins('nginx-module'))} send \`X-Page-Speed\`, the nginx module's header. The Apache module answers behind an nginx front on ${n(serverOrigins('apache-module', 'nginx'))} origins and behind Cloudflare on ${n(serverOrigins('apache-module', 'cloudflare'))}.`,
  'servers-table': table(
    ['Module header', 'Server header family', 'Origins'],
    t.servers.map((r) => [
      engineName[r.engine] ?? r.engine,
      familyName[r.server_family] ?? r.server_family,
      n(r.origins),
    ]),
  ),
  'detection-numbers': `In the ${month(h.crawl_date)} desktop crawl, ${n(headerPages)} root pages send a PageSpeed header. Technology detection flags ${n(detectedPages)} pages, ${signal('wappalyzer_only') === 1 ? 'one' : n(signal('wappalyzer_only'))} of them without the header.`,
  'detection-table': table(
    ['Signal', 'Root pages'],
    t.detection.map((r) => [signalName[r.signal] ?? r.signal, n(r.origins)]),
  ),
  'cooccurrence-table': table(
    ['Segment', 'WordPress share'],
    t.cooccurrence.map((r) => [
      r.segment === 'pagespeed' ? 'PageSpeed-serving origins' : 'All origins',
      pct(r.wordpress_share),
    ]),
  ),
};

let text = readFileSync(post, 'utf8');
const original = text;
for (const [name, body] of Object.entries(blocks)) {
  const re = new RegExp(`(<!-- census:begin ${name} -->)[\\s\\S]*?(<!-- census:end ${name} -->)`);
  if (!re.test(text)) throw new Error(`post has no block "${name}"`);
  text = text.replace(re, (_, begin, end) => `${begin}\n${body}\n${end}`);
}
text = await prettier.format(text, {
  ...(await prettier.resolveConfig(post)),
  filepath: post,
});
if (check) {
  if (text !== original) {
    console.error(
      'census blocks are stale; run: node scripts/generate-census-blocks.mjs --date ' + date,
    );
    process.exit(1);
  }
  console.log('census blocks are current');
} else {
  writeFileSync(post, text);
  console.log(`rewrote ${Object.keys(blocks).length} blocks in ${post}`);
}
