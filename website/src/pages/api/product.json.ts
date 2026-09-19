// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// /api/product.json — machine-readable product summary. All drift-prone facts
// (support tiers, variant count, versions, package names, bundled nginx) are
// imported from src/data/product-facts.mjs, the single source of truth shared
// with offer-jsonld.ts and the generated public/llms*.txt files. Edit facts
// there, not here.

import type { APIRoute } from 'astro';
import {
  VENDOR,
  WEBSITE,
  PRODUCT_NAME,
  CURRENT_LINE,
  SUPPORT_TIERS,
  SUPPORT_URL,
  LICENSING_TERMS_URL,
  SOURCE_PUBLICATION,
  IMAGE_FORMATS,
  IMAGE_FORMAT_SUPPORT,
  formatsFor,
  MAX_VARIANTS,
  V1_LINE,
  V2_LINE,
  V1_RENUMBERED_FROM,
  LAST_UPSTREAM_VERSION,
  PKG_SIDECAR,
  PKG_SIDECAR_NATIVE,
  SIDECAR_NGINX_VERSION,
  SIDECAR_RIDS,
} from '../../data/product-facts.mjs';

const productData = {
  product: `${PRODUCT_NAME} ${CURRENT_LINE}`,
  vendor: VENDOR,
  website: WEBSITE,
  description:
    'Self-hosted web performance optimization: a native web-server module for Apache and nginx, with a separate optimizer worker process doing the heavy optimization work outside the web server. No application code changes required.',
  // The software license (SPDX id) and the state of the source, both from the
  // single source of truth in product-facts.mjs.
  license: SOURCE_PUBLICATION.licenseId,
  source_publication: {
    status: SOURCE_PUBLICATION.status,
    license: SOURCE_PUBLICATION.licenseId,
  },
  // What is for sale is support and the hardened build channel. Prices are
  // placeholders (null) until published.
  support: {
    url: SUPPORT_URL,
    terms: LICENSING_TERMS_URL,
    tiers: SUPPORT_TIERS.map((t) => ({
      id: t.id,
      name: t.name,
      kind: t.kind,
      annual_usd: t.prices.annualUsd,
      monthly_usd: t.prices.monthlyUsd,
      note: t.note,
    })),
  },
  // Artifact access, expressed additively (new key, existing keys unchanged):
  // the standard signed packages are free; the hardened channel (hardened
  // builds with SBOM + signed build provenance) is sold, pricing to be
  // announced. Paid artifacts carry the same software license as the
  // standard packages.
  artifacts: {
    standard_packages: 'free',
    hardened_builds: 'paid',
    hardened_pricing: 'to-be-announced',
  },
  features: {
    // `image_formats` is the flat format list. Its key name, shape (flat array
    // of strings) and value are a PUBLIC CONTRACT that unknown agent/LLM
    // clients already consume — never repurpose it into an object or re-scope
    // it. Per-edition truth is expressed ADDITIVELY in the two keys below.
    image_formats: IMAGE_FORMATS,
    // Per-edition format lists, keyed by marketing line label. Predecessor
    // keys ('1.15', '2.0') are kept — the key set only grows.
    image_formats_by_line: {
      [V1_LINE]: formatsFor(V1_LINE),
      [V2_LINE]: formatsFor(V2_LINE),
      [CURRENT_LINE]: formatsFor(CURRENT_LINE),
    },
    // The full format-first, edition-keyed, port-scoped table (the site's
    // single source of truth, src/data/product-facts.mjs IMAGE_FORMAT_SUPPORT).
    // An absent port is a port we do not claim: the 1.15 Envoy port is not
    // shipped and its AVIF status is unverified, so Envoy appears nowhere.
    // AVIF on 1.15 is opt-in (four filters outside rewrite_images/CoreFilters).
    image_format_support: IMAGE_FORMAT_SUPPORT,
    optimizations: ['images', 'css', 'js', 'html'],
    max_variants_per_image: MAX_VARIANTS,
    zero_copy_serving: true,
    early_hints_103: true,
    save_data_support: true,
    critical_css_extraction: true,
    proactive_variant_generation: true,
  },
  deployment: ['apt-yum-packages', 'docker', 'kubernetes-sidecar', 'aspnet-core-middleware'],
  // Key name is a public contract (predates the convergence). No lifecycle
  // status or date fields here — this is a published, agent-readable
  // endpoint, so those stay out the same way they stay out of llms.txt.
  related_products: [
    {
      product: `mod_pagespeed ${V1_LINE}`,
      description: `The mod_pagespeed ${V1_LINE} line, continuing the open-source mod_pagespeed project (originally developed at Google; not affiliated with or endorsed by Google): a native in-process module for Apache, nginx, and IIS. ${PRODUCT_NAME} ${CURRENT_LINE} is its drop-in upgrade (same directives). Renumbered from ${V1_RENUMBERED_FROM} (forward-semver successor to the last upstream release, ${LAST_UPSTREAM_VERSION}).`,
      nuget_sidecar: {
        package: PKG_SIDECAR,
        native_assets_package: PKG_SIDECAR_NATIVE,
        install: `dotnet add package ${PKG_SIDECAR}`,
        platforms: SIDECAR_RIDS,
        bundled_nginx: SIDECAR_NGINX_VERSION,
        topology:
          'inverse: your ASP.NET Core Kestrel app is the public front door; the bundled nginx runs on loopback behind it as the optimize-proxy',
        url: 'https://www.nuget.org/packages/WeAmp.PageSpeed.Sidecar',
      },
      links: {
        overview: 'https://modpagespeed.com/',
        docs: 'https://modpagespeed.com/1.1/docs/',
        upgrade_guide: 'https://modpagespeed.com/1.1/docs/upgrading-to-2-1/',
      },
    },
    {
      product: `ModPageSpeed ${V2_LINE}`,
      description: `The ModPageSpeed ${V2_LINE} Docker/Helm stack and ASP.NET Core middleware.`,
      links: {
        migration_guide: 'https://modpagespeed.com/docs/migrating-to-2-1/',
      },
    },
  ],
  links: {
    docs: 'https://modpagespeed.com/docs/',
    features: 'https://modpagespeed.com/features/',
    pricing: 'https://modpagespeed.com/pricing/',
    demo: 'https://modpagespeed.com/demo/',
    getting_started: 'https://modpagespeed.com/docs/getting-started/',
    install_docker: 'https://modpagespeed.com/docs/installation-docker/',
    install_module: 'https://modpagespeed.com/docs/installation-module/',
    configuration: 'https://modpagespeed.com/docs/configuration/',
    blog: 'https://modpagespeed.com/blog/',
    contact: 'https://modpagespeed.com/contact/',
    llms_txt: 'https://modpagespeed.com/llms.txt',
    llms_full_txt: 'https://modpagespeed.com/llms-full.txt',
  },
};

export const GET: APIRoute = () => {
  return new Response(JSON.stringify(productData, null, 2), {
    headers: {
      'Content-Type': 'application/json',
      'Cache-Control': 'public, max-age=86400',
    },
  });
};
