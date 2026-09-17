// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// @ts-check
import { defineConfig } from 'astro/config';
import tailwindcss from '@tailwindcss/vite';
import sitemap from '@astrojs/sitemap';
import mdx from '@astrojs/mdx';
import node from '@astrojs/node';
import remarkCustomHeadingId from 'remark-custom-heading-id';
import remarkDirective from 'remark-directive';
import remarkCallouts from './src/lib/remark-callouts.mjs';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const publicDir = resolve(__dirname, 'public');

/**
 * Build a `URL path -> ISO date` map from content-collection frontmatter so the
 * sitemap can emit a real per-entry `lastmod`. A build-time lastmod on every URL
 * trains crawlers to ignore the freshness signal; a per-entry date from
 * `lastUpdated`/`date`/`datePublished` is a true signal. Defensive throughout:
 * any failure returns null, and the serializer then omits lastmod for that URL.
 * @param {string} filePath
 * @returns {string | null}
 */
function extractFrontmatterDate(filePath) {
  try {
    const src = readFileSync(filePath, 'utf8');
    const fm = src.match(/^---\r?\n([\s\S]*?)\r?\n---/);
    if (!fm) return null;
    const block = fm[1];
    const pick = (/** @type {string} */ key) => {
      const m = block.match(new RegExp(`^${key}:\\s*['"]?(\\d{4}-\\d{2}-\\d{2})`, 'm'));
      return m ? m[1] : null;
    };
    const date = pick('lastUpdated') ?? pick('date') ?? pick('datePublished');
    return date ? new Date(date).toISOString() : null;
  } catch {
    return null;
  }
}

/** @returns {Map<string, string>} */
function buildLastmodMap() {
  /** @type {Map<string, string>} */
  const map = new Map();
  const collections = [
    { dir: 'src/content/blog', prefix: '/blog/' },
    { dir: 'src/content/docs', prefix: '/docs/' },
    { dir: 'src/content/docs-1.1', prefix: '/1.1/docs/' },
  ];
  for (const { dir, prefix } of collections) {
    /** @type {string[]} */
    let files = [];
    try {
      files = readdirSync(resolve(__dirname, dir));
    } catch {
      continue;
    }
    for (const file of files) {
      if (!/\.mdx?$/.test(file)) continue;
      const iso = extractFrontmatterDate(resolve(__dirname, dir, file));
      if (iso) map.set(`${prefix}${file.replace(/\.mdx?$/, '')}/`, iso);
    }
  }
  return map;
}

const lastmodMap = buildLastmodMap();

// Slugs of /examples/<slug>/ pages that have generated before/after data. Detail
// pages without data are noindex (see examples/[slug].astro) and are kept out of
// the sitemap until the generator populates them — self-healing on next build.
const examplesWithData = (() => {
  try {
    const data = JSON.parse(
      readFileSync(resolve(__dirname, 'src/data/examples-data.json'), 'utf8'),
    );
    return new Set(
      Object.entries(data.examples || {})
        .filter(([, v]) => v && !v.error && v.settled)
        .map(([slug]) => slug),
    );
  } catch {
    return new Set();
  }
})();

/** True when the last path segment has no file extension. */
function isExtensionless(/** @type {string} */ url) {
  return !basename(url).includes('.');
}

/**
 * Rewrite extensionless /1.0/ request URLs to their .html counterparts.
 * @param {import('http').IncomingMessage} req
 * @param {import('http').ServerResponse} _res
 * @param {() => void} next
 */
function rewriteLegacyUrl(req, _res, next) {
  const url = req.url || '';
  if (url.startsWith('/1.0/') && !url.endsWith('/') && isExtensionless(url)) {
    const htmlPath = join(publicDir, url + '.html');
    if (existsSync(htmlPath)) {
      req.url = url + '.html';
    }
  }
  next();
}

/**
 * Vite plugin: rewrite extensionless /1.0/ URLs to .html.
 * The legacy docs use extensionless hrefs but the files have .html extensions.
 * This runs in the dev/preview server; production uses Astro middleware.
 * @returns {import('vite').Plugin}
 */
function legacyDocsRewrite() {
  return {
    name: 'legacy-docs-rewrite',
    enforce: 'pre',
    configureServer(server) {
      server.middlewares.use(rewriteLegacyUrl);
    },
    configurePreviewServer(server) {
      server.middlewares.use(rewriteLegacyUrl);
    },
  };
}

// https://astro.build/config
export default defineConfig({
  // Astro 7 trims leading/trailing whitespace inside element text content,
  // which collides words wherever an inline tag sits on its own line
  // ("contributor to" + <a>link</a> renders as "contributor tolink"). This
  // site is served through ModPageSpeed, which optimizes HTML at the edge,
  // so build-time compression is redundant here. See #1026 and corp PR #782.
  compressHTML: false,
  adapter: node({ mode: 'standalone' }),
  site: 'https://modpagespeed.com',
  security: {
    checkOrigin: true,
  },
  // 301 the old default-template slug to the descriptive origin-story slug.
  // The post was renamed hello-world.md -> why-i-rebuilt-mod-pagespeed.md;
  // this preserves any accrued links/bookmarks to the original URL.
  redirects: {
    '/blog/hello-world/': '/blog/why-i-rebuilt-mod-pagespeed/',
    // /vs/getpagespeed/ retired 2026-06-20 (reverses the D6 single-named-
    // page exception): we win the maintainer/alternative SERP on facts-about-us,
    // and a leader-names-challenger comparison page only lent the competitor
    // visibility. Forward the evaluation intent to the no-competitor-named page.
    '/vs/getpagespeed/': '/alternatives/ngx-pagespeed/',
    // The activation guide became the software-license page when the product
    // stopped needing activation; keep inbound links and bookmarks working.
    '/docs/license-activation/': '/docs/license/',
    // The per-site claim and Community attestation flows retired with the
    // converged line; receipts and portal mails still link here, so both
    // forward to the software-license page.
    '/claim-license/': '/license/',
    '/community-license/': '/license/',
  },
  markdown: {
    // remarkDirective parses `:::caution[…]:::` container syntax; remarkCallouts
    // then turns those nodes into styled callout asides. Order matters — the
    // directive parser must run before the transform that consumes its nodes.
    remarkPlugins: [remarkCustomHeadingId, remarkDirective, remarkCallouts],
    // Dual-theme Shiki. `defaultColor: false` emits CSS variables instead of
    // baking one theme's colors into the HTML — the styles in global.css then
    // switch between --shiki-light and --shiki-dark based on `.dark` on <html>.
    // Without this, light-mode pages render code blocks in github-dark, which
    // produces a jarring high-contrast block against light prose.
    shikiConfig: {
      themes: {
        light: 'github-light',
        dark: 'github-dark',
      },
      defaultColor: false,
    },
  },
  integrations: [
    // MDX for docs pages that interpolate release manifest values via the
    // release-aware components in src/components/release/. Plain .md docs
    // continue to work; only the version-coupled docs use .mdx (§4).
    mdx(),
    sitemap({
      // Exclude legacy 1.0 docs, the noindex checkout, and the noindex
      // /go/ redirect shims. Also drop /examples/<slug>/ detail pages that have
      // no generated data yet (they're noindex until the generator populates
      // them); the /examples/ index hub always stays in.
      filter: (page) => {
        if (
          page.includes('/1.0/') ||
          page.includes('/buy/') ||
          page.includes('/go/') ||
          page.includes('/error/')
        )
          return false;
        const m = new URL(page).pathname.match(/^\/examples\/([^/]+)\/$/);
        if (m) return examplesWithData.has(m[1]);
        return true;
      },
      // Per-entry lastmod from content frontmatter where available. Pages with
      // no real content date (home, pricing, examples, dateless docs) omit
      // lastmod entirely — an always-changing build-time "now" makes search
      // engines distrust the freshness signal sitewide.
      serialize(item) {
        let path;
        try {
          path = new URL(item.url).pathname;
        } catch {
          path = '';
        }
        const iso = lastmodMap.get(path);
        if (iso) item.lastmod = iso;
        return item;
      },
    }),
  ],
  vite: {
    plugins: [legacyDocsRewrite(), tailwindcss()],
  },
});
