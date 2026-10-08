// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// agent-files.mjs — the few things the generated agent files (public/llms.txt
// and public/llms-full.txt, written by scripts/generate-llms.mjs) cannot derive
// from the docs collection, the release manifests or product-facts.mjs: the
// summary and the about paragraphs that open both files, and the pages outside
// the docs collection that the index lists. Everything else in those files is
// generated; a page added to src/content/docs/ appears in them on the next build
// with nothing to edit here.
//
// {{TOKEN}} holes are filled from LLMS_TOKENS in product-facts.mjs; an unknown
// token fails the generator. Copy rules apply (website/CLAUDE.md): the product
// is mod_pagespeed 2.1, "worker" never "daemon", predecessor lines only as
// predecessors, and nothing about the Google relationship beyond the
// provenance sentence.

/** The blockquote under the H1: the one-paragraph summary of the llms.txt convention. */
export const SUMMARY =
  'Self-hosted web performance optimization for Apache and nginx. {{LICENSE_CLAUSE_CAP}}. {{PRODUCT_NAME}} {{CURRENT_LINE}} converges the mod_pagespeed lineage and the engine from the 2.0 re-architecture into one product: a native web-server module with a separate optimizer worker.';

/**
 * The paragraphs between the summary and the first link section. Plain
 * Markdown, no headings (the llms.txt convention reserves H2 for link lists).
 */
export const ABOUT = `We-Amp B.V. develops {{PRODUCT_NAME}} {{CURRENT_LINE}}, the converged continuation of the open-source mod_pagespeed project, originally developed at Google. It automatically optimizes web content (images, CSS, JavaScript, HTML): a native in-process module for Apache and nginx handles serving, and a separate optimizer worker does the heavy optimization work outside the web server. Drop-in compatible with the classic module: the same configuration directives and filters carry over, and no application code changes are required. {{PRODUCT_NAME}} {{CURRENT_LINE}} converges the {{V1_LINE}} module line (renumbered from {{V1_RENUMBERED_FROM}}, the forward-semver successor to the last upstream release, {{LAST_UPSTREAM_VERSION}}) with the ModPageSpeed {{V2_LINE}} engine, which continues as the optimizer worker; both predecessor lines upgrade to it in place.

Heritage: mod_pagespeed, the open-source optimization core this product continues, powers {{BUILTWITH_SITES}} live sites today (per BuiltWith, {{BUILTWITH_AS_OF}}; {{BUILTWITH_HISTORICAL}} across its history). These are not We-Amp customers; they are the proof the optimization core works at scale.

What it does:

- Image optimization in the optimizer worker: WebP, AVIF and SVG transcoding, viewport-aware resizing, learned per-format quality prediction with SSIMULACRA2 verification, content-aware classification and denoising, up to {{MAX_VARIANTS}} variants per image ({{RASTER_VARIANTS}} raster plus one SVG)
- CSS optimization: minification, heuristic critical-CSS extraction with an optional headless-browser pipeline, non-blocking stylesheet loading
- JavaScript optimization: safe minification and browser-analysis-driven script deferral, no AST transforms
- HTML optimization: critical CSS injection, 103 Early Hints, lazy-loading attributes, image dimensions, LCP image preload, font preloading, preconnect hints
- Variant-aware caching keyed on a 32-bit capability mask (image format, viewport, pixel density, Save-Data, transfer encoding), served zero-copy from the shared Cyclone cache
- The module's classic filter set: CSS and JavaScript combining, image spriting, in-place resource optimization, domain mapping, and the built-in admin console

{{PRODUCT_NAME}} {{CURRENT_LINE}} is {{LICENSE_CLAUSE}}: free to install and run, in development and in production, with free standard signed packages. What We-Amp sells is support, in three tiers, with hardened builds included from Priority up:

{{SUPPORT_LADDER_MD}}
- {{PRICING_ON_REQUEST}} {{RESPONSE_TARGETS_LINE}}
- No bandwidth fees, no per-request charges, no feature gates; the software itself costs nothing. Support terms: {{LICENSING_TERMS_URL}}

Every documentation page below is also served as Markdown at its URL with a \`.md\` suffix (for example https://modpagespeed.com/docs/getting-started.md), and https://modpagespeed.com/llms-full.txt carries this index followed by the full text of every page.

We-Amp B.V., The Netherlands (founded {{COMPANY_FOUNDED_YEAR}}, KvK {{COMPANY_KVK}}); {{COMMERCIAL_EMAIL}} for support, hardened builds, partnerships and consulting; security@modpagespeed.com for vulnerability reports only. License: {{LICENSE_NAME}}; the LICENSE, NOTICE and THIRD-PARTY-NOTICES files ship with every distribution. mod_pagespeed is an open-source project originally developed at Google. {{PRODUCT_NAME}} {{CURRENT_LINE}} is developed by We-Amp B.V. and is not affiliated with or endorsed by Google.`;

/**
 * The filters table is a page of its own (src/pages/docs/filters.astro), not a
 * collection entry; the index lists it with the Reference group, as the docs
 * hub does.
 */
export const FILTERS_PAGE = {
  path: '/docs/filters/',
  title: 'PageSpeed filters reference',
  description:
    'The image, CSS, JavaScript, HTML, and caching filters, what each one does, and which run by default.',
};

/**
 * Pages outside the docs collection, by index section. Paths are
 * site-relative; the generator makes them absolute. Keep each description to
 * one line, in the voice of the page it points at.
 * @type {Array<{ section: string, items: Array<{ path: string, title: string, description: string }> }>}
 */
export const SURFACES = [
  {
    section: 'Tools',
    items: [
      {
        path: '/demo/',
        title: 'Live demo',
        description:
          'Watch {{PRODUCT_NAME}} {{CURRENT_LINE}} optimize real sites: measured CSS/JS minification and WebP/AVIF transcoding, with modeled load-time projections.',
      },
      {
        path: '/examples/',
        title: 'Optimization examples',
        description:
          'Every optimization the module performs, shown live and before/after per filter.',
      },
      {
        path: '/analyze/',
        title: 'Analyze a page',
        description:
          'Score any URL with PageSpeed Insights and see which findings the product addresses.',
      },
      {
        path: '/ai-readability/',
        title: 'RenderPeek, the free AI-readability checker',
        description:
          'Paste a URL and see what an AI agent reads on your page: the readable content with and without JavaScript, markup bloat, and how well the page is described for machines. No signup.',
      },
      {
        path: '/calculator/',
        title: 'Savings calculator',
        description:
          'Estimate bandwidth and cost savings from automatic image optimization, CSS/JS minification, and variant-aware caching.',
      },
      {
        path: '/pagespeed-markers/',
        title: 'PageSpeed markers',
        description:
          'The response headers and URL markers that show the product is active on a page.',
      },
    ],
  },
  {
    section: 'Download, pricing and support',
    items: [
      {
        path: '/download/',
        title: 'Download',
        description:
          'Signed apt/yum packages for Apache and nginx, Docker and Helm for Kubernetes, the ASP.NET Core middleware and the IIS module.',
      },
      {
        path: '/download/apt-yum/',
        title: 'The apt/yum repository',
        description:
          'The signed apt and yum repository for the Apache and nginx modules and the optimizer worker: one line sets it up.',
      },
      {
        path: '/features/',
        title: 'Features',
        description:
          'Self-hosted WebP/AVIF/SVG transcoding, heuristic critical CSS, and variant-aware caching: the full feature set.',
      },
      {
        path: '/support/',
        title: 'Support',
        description:
          'What a support subscription buys, the three tiers, hardened builds and how to verify what ships today, the hosting partner program and consulting.',
      },
      {
        path: '/pricing/',
        title: 'Pricing',
        description:
          'What We-Amp sells: {{SUPPORT_LADDER_LINE}}. The software is free to run. Pricing on request, with a quote form.',
      },
      {
        path: '/license/',
        title: 'Software license',
        description:
          '{{PRODUCT_NAME}} {{CURRENT_LINE}} is {{LICENSE_CLAUSE}}: free to install and run, in development and in production.',
      },
      {
        path: '/security/',
        title: 'Security',
        description:
          'Security policy: how to report a vulnerability, the threat model, supply-chain security and the safe harbor for researchers.',
      },
      {
        path: '/contact/',
        title: 'Contact',
        description:
          'Setup help, support subscriptions, hardened builds, the hosting partner program, consulting and IISpeed license transfers; replies within one business day (CET). Security disclosure has its own channel.',
      },
      {
        path: '/mod-pagespeed-still-maintained/',
        title: 'Is mod_pagespeed still maintained?',
        description:
          'Yes: who maintains it, what shipped, and how the archived open-source module and ngx_pagespeed move to the current product.',
      },
      {
        path: '/blog/',
        title: 'Blog',
        description:
          'Engineering posts and Core Web Vitals fix guides; each post keeps the version strings of its day.',
      },
      {
        path: '/alternatives/',
        title: 'Alternatives',
        description:
          'Structured comparisons for readers coming from mod_pagespeed, ngx_pagespeed, the Google PageSpeed module and IISpeed.',
      },
      {
        path: '/vs/',
        title: 'Comparisons',
        description:
          'Side-by-side comparisons with Cloudflare APO, Cloudinary, imgproxy, NitroPack, Thumbor and WP Rocket.',
      },
      { path: '/terms/', title: 'Terms', description: 'Terms of service.' },
      { path: '/privacy/', title: 'Privacy', description: 'Privacy policy.' },
    ],
  },
  {
    section: 'Machine-readable',
    items: [
      {
        path: '/api/product.json',
        title: 'Product summary (JSON)',
        description:
          'Product, license, support tiers, image-format support per line and the related packages, as data.',
      },
      {
        path: '/llms-full.txt',
        title: 'llms-full.txt',
        description: 'This index followed by the full Markdown text of every documentation page.',
      },
      {
        path: '/.well-known/ai-plugin.json',
        title: 'AI plugin manifest',
        description: 'The model-facing description of the product.',
      },
    ],
  },
];
