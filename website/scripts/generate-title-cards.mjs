// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// generate-title-cards.mjs — build-time programmatic OG/social title cards.
//
// Why this exists:
//   Every blog post historically shared `/og-default.png` for both the
//   BlogPosting JSON-LD `image` field and the `og:image` meta tag. Google
//   Search Console flagged the snippet weakness — same image across the
//   entire blog drops CTR and Discover surfacing. Per-post hand-designed
//   cards do not scale. This script generates one PNG per post at
//   `public/og-cards/{slug}.png`, 1200x630 (standard OG aspect), using
//   `satori` to render an SVG tree and `@resvg/resvg-js` to rasterize.
//
// Frontmatter contract:
//   - If a post sets `coverImage: /path/to/img.png` in frontmatter, the
//     renderer uses that path verbatim and the generator skips it.
//   - Otherwise the generator synthesizes /og-cards/{slug}.png and the
//     renderer points at that file.
//
// Determinism / incrementality:
//   - Each PNG is regenerated only when its source markdown's mtime is
//     newer than the PNG (or the PNG is missing). Re-running the script
//     in a clean tree is a no-op modulo stdout chatter.
//   - Output is committed-friendly via `public/og-cards/` (already part of
//     Astro's static `public` tree — gets copied into `dist/` verbatim).
//
// Devdep-only:
//   This script is invoked from the `prebuild` npm script and runs only at
//   build time. Neither `satori` nor `@resvg/resvg-js` ever ships in a
//   runtime bundle.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import satori from 'satori';
import { Resvg } from '@resvg/resvg-js';
// satori speaks OpenType (TTF/OTF), not WOFF2. Inter ships in this repo as
// a WOFF2 variable font, so we decompress to a TTF buffer in-memory at
// build time. wawoff2 is a thin WASM wrapper around Google's woff2 tool
// and adds ~140KB of devDeps.
import wawoff from 'wawoff2';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const websiteRoot = path.resolve(__dirname, '..');
const blogDir = path.join(websiteRoot, 'src/content/blog');
// satori's vendored opentype.js cannot parse variable-font fvar tables, so
// we use static-weight Inter from @fontsource/inter (devDep) instead of
// the variable woff2 the site ships at runtime. Both files happen to be
// WOFF2 and need to be decompressed to TTF via wawoff2 before satori can
// consume them.
const fontsourceDir = path.join(
  websiteRoot,
  'node_modules/@fontsource/inter/files',
);
const fontPaths = {
  400: path.join(fontsourceDir, 'inter-latin-400-normal.woff2'),
  700: path.join(fontsourceDir, 'inter-latin-700-normal.woff2'),
};
const outDir = path.join(websiteRoot, 'public/og-cards');

const WIDTH = 1200;
const HEIGHT = 630;

// Minimal YAML frontmatter parser. The blog frontmatter shape is
// deliberately simple — string/date scalars + a string array for tags. We
// avoid pulling in `gray-matter` for one job.
function parseFrontmatter(src) {
  if (!src.startsWith('---')) return {};
  const end = src.indexOf('\n---', 3);
  if (end === -1) return {};
  const block = src.slice(3, end).trim();
  const data = {};
  for (const rawLine of block.split('\n')) {
    const line = rawLine.replace(/\s+$/, '');
    if (!line || line.startsWith('#')) continue;
    const colonIdx = line.indexOf(':');
    if (colonIdx === -1) continue;
    const key = line.slice(0, colonIdx).trim();
    let value = line.slice(colonIdx + 1).trim();
    if (!value) {
      data[key] = '';
      continue;
    }
    // Strip surrounding quotes.
    if (
      (value.startsWith("'") && value.endsWith("'")) ||
      (value.startsWith('"') && value.endsWith('"'))
    ) {
      value = value.slice(1, -1);
    }
    data[key] = value;
  }
  return data;
}

function formatMonthYear(isoDate) {
  // Frontmatter date is `YYYY-MM-DD`. Avoid TZ shenanigans by parsing
  // components rather than passing through `new Date(string)`.
  const m = /^(\d{4})-(\d{2})-(\d{2})/.exec(isoDate || '');
  if (!m) return '';
  const months = [
    'January',
    'February',
    'March',
    'April',
    'May',
    'June',
    'July',
    'August',
    'September',
    'October',
    'November',
    'December',
  ];
  const monthName = months[Number(m[2]) - 1] ?? '';
  return monthName ? `${monthName} ${m[1]}` : '';
}

// Build the satori VDOM. We hand-author it with plain objects (satori
// supports both JSX and the `{ type, props }` object form; the object form
// keeps this file free of a JSX transform).
function buildTree({ title, author, when }) {
  return {
    type: 'div',
    props: {
      style: {
        width: WIDTH,
        height: HEIGHT,
        display: 'flex',
        flexDirection: 'column',
        justifyContent: 'space-between',
        // stone-950 → stone-900 vertical gradient, kept very subtle so the
        // typography carries the card.
        backgroundImage: 'linear-gradient(180deg, #0c0a09 0%, #1c1917 100%)',
        padding: 80,
        fontFamily: 'Inter',
        color: '#fafaf9',
      },
      children: [
        // Top row: ModPageSpeed 2.0 wordmark. Satori can rasterize inline
        // SVG, but a typographic wordmark is more legible at OG sizes and
        // avoids a second asset-loading code path.
        {
          type: 'div',
          props: {
            style: {
              display: 'flex',
              alignItems: 'center',
              gap: 16,
              fontSize: 28,
              fontWeight: 700,
              letterSpacing: '-0.01em',
              color: '#fafaf9',
            },
            children: [
              // Blue accent dot, the same blue-700 used as the section bar
              // and as a link color throughout the site.
              {
                type: 'div',
                props: {
                  style: {
                    width: 14,
                    height: 14,
                    borderRadius: 7,
                    backgroundColor: '#1d4ed8',
                  },
                },
              },
              'ModPageSpeed 2.0',
            ],
          },
        },
        // Middle stack: blue bar + title + caption. The bar is the
        // single piece of visual identity that ties cards back to the
        // ModPageSpeed brand without depending on a logo asset.
        {
          type: 'div',
          props: {
            style: {
              display: 'flex',
              flexDirection: 'column',
              gap: 24,
              marginTop: 'auto',
            },
            children: [
              {
                type: 'div',
                props: {
                  style: {
                    width: 80,
                    height: 6,
                    backgroundColor: '#1d4ed8',
                    borderRadius: 3,
                  },
                },
              },
              {
                type: 'div',
                props: {
                  style: {
                    fontSize: 64,
                    fontWeight: 700,
                    lineHeight: 1.1,
                    letterSpacing: '-0.02em',
                    color: '#fafaf9',
                    // Cap at ~3 lines — satori wraps but does not natively
                    // truncate; the layout above keeps the column wide
                    // enough that 3 lines is rare.
                  },
                  children: title,
                },
              },
              when || author
                ? {
                    type: 'div',
                    props: {
                      style: {
                        fontSize: 24,
                        fontWeight: 400,
                        color: '#a8a29e', // stone-400
                      },
                      children: [author, when].filter(Boolean).join(' · '),
                    },
                  }
                : null,
            ].filter(Boolean),
          },
        },
      ],
    },
  };
}

async function main() {
  async function loadFont(p) {
    if (!fs.existsSync(p)) throw new Error(`Inter font not found at ${p}`);
    const woff2Buf = fs.readFileSync(p);
    // wawoff.decompress returns a Uint8Array (sfnt/TTF bytes).
    const ttfBytes = await wawoff.decompress(woff2Buf);
    return Buffer.from(ttfBytes);
  }
  const font400 = await loadFont(fontPaths[400]);
  const font700 = await loadFont(fontPaths[700]);

  fs.mkdirSync(outDir, { recursive: true });

  const entries = fs
    .readdirSync(blogDir)
    .filter((f) => f.endsWith('.md'))
    .sort();

  let generated = 0;
  let skipped = 0;

  for (const filename of entries) {
    const slug = filename.replace(/\.md$/, '');
    const srcPath = path.join(blogDir, filename);
    const outPath = path.join(outDir, `${slug}.png`);

    const raw = fs.readFileSync(srcPath, 'utf8');
    const fm = parseFrontmatter(raw);

    // Posts with an explicit coverImage opt out — the operator wants that
    // path used as-is.
    if (fm.coverImage) {
      console.log(`skipped (explicit coverImage): og-cards/${slug}.png`);
      skipped++;
      continue;
    }

    // mtime-based incremental: skip if PNG exists AND is newer than the
    // markdown AND newer than this script itself (so editing the template
    // forces regeneration).
    if (fs.existsSync(outPath)) {
      const outStat = fs.statSync(outPath);
      const srcStat = fs.statSync(srcPath);
      const scriptStat = fs.statSync(__filename);
      if (
        outStat.mtimeMs > srcStat.mtimeMs &&
        outStat.mtimeMs > scriptStat.mtimeMs
      ) {
        console.log(`skipped (cached): og-cards/${slug}.png`);
        skipped++;
        continue;
      }
    }

    const tree = buildTree({
      title: fm.title || slug,
      author: fm.author || 'Otto van der Schaaf',
      when: formatMonthYear(fm.date),
    });

    // satori treats `weight` as a discrete lookup, so 400 and 700 are
    // declared as separate static-weight font entries.
    const svg = await satori(tree, {
      width: WIDTH,
      height: HEIGHT,
      fonts: [
        { name: 'Inter', data: font400, weight: 400, style: 'normal' },
        { name: 'Inter', data: font700, weight: 700, style: 'normal' },
      ],
    });

    const png = new Resvg(svg, { fitTo: { mode: 'width', value: WIDTH } })
      .render()
      .asPng();
    fs.writeFileSync(outPath, png);
    console.log(`generated: og-cards/${slug}.png`);
    generated++;
  }

  console.log(
    `\ntitle cards: ${generated} generated, ${skipped} skipped, ${entries.length} total`,
  );
}

await main().catch((err) => {
  console.error('generate-title-cards failed:', err);
  process.exit(1);
});
