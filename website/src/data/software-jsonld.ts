// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The one schema.org node for the product. Every page that describes the
// product builds it here, so its name, alternate names, version, platforms
// and publisher are the same everywhere:
//
//   productJsonLd(extra)  a top-level node (with @context) for a page's jsonLd
//   productNode(extra)    the same node without @context, for an @graph
//   productRef()          a short reference for an article's `about`
//
// `extra` carries what a page says about the product in its own words (a
// description, an image, an offer URL, a feature list). It cannot change the
// identity fields, which come from product-facts.mjs and the release manifest.
//
// Naming (owner ruling, 2026-10-20): the product is plain PRODUCT_DISPLAY_NAME,
// with no version. The version is the exact three-part release from the
// manifest, in `softwareVersion` only. The names people searched for earlier
// stay in `alternateName`, which is what keeps those queries resolving to this
// node.

import { getRelease } from '../lib/release';
import { offerJsonLd } from './offer-jsonld';
import { ORGANIZATION_REF, PRODUCT_BRAND } from './organization';
import { PRODUCT_DISPLAY_NAME, WEBSITE } from './product-facts.mjs';

// The platforms it ships for: the Apache and nginx packages on Linux, the IIS
// module and the ASP.NET Core packages on Windows.
export const PRODUCT_OPERATING_SYSTEMS = ['Linux', 'Windows'];

export const PRODUCT_JSONLD_ID = `${WEBSITE}/#software`;

// Old and alternate names, in rough order of how often people search them.
// Google's own product names ("Google PageSpeed", "PageSpeed Insights") are
// deliberately absent: they are Google's, and /pagespeed-insights/ exists to
// tell them apart.
export const PRODUCT_ALTERNATE_NAMES = [
  // The major line, the form the copy uses to contrast with 1.x.
  `${PRODUCT_DISPLAY_NAME} 2`,
  // The nginx module's historical name, still the most searched.
  'ngx_pagespeed',
  // The package names.
  'mod-pagespeed',
  'nginx-module-pagespeed',
  'PageSpeed module',
  // The camel spelling, and the 2.0 predecessor under it. Both appear here
  // as an alternateName only.
  /* alternateName */ 'ModPageSpeed',
  'ModPageSpeed 2.0',
];

// Where the product lives besides this site.
export const PRODUCT_SAME_AS = [
  'https://github.com/We-Amp/mod_pagespeed',
  'https://github.com/We-Amp/pagespeed-optimizer',
];

export async function productSoftwareVersion(): Promise<string> {
  return (await getRelease('2.1')).release.semver;
}

export async function productNode(extra: ProductExtra = {}) {
  const { alternateName: moreNames = [], ...rest } = extra;
  return {
    // Defaults a page may replace.
    operatingSystem: PRODUCT_OPERATING_SYSTEMS,
    url: `${WEBSITE}/`,
    downloadUrl: `${WEBSITE}/download/`,
    releaseNotes: `${WEBSITE}/docs/release-notes/`,
    sameAs: PRODUCT_SAME_AS,
    offers: offerJsonLd({ url: `${WEBSITE}/download/` }),
    ...rest,
    // Identity fields last, so a page cannot override them. A page may add
    // alternate names (a package name, say), never drop the shared ones.
    '@type': ['Product', 'SoftwareApplication'],
    '@id': PRODUCT_JSONLD_ID,
    name: PRODUCT_DISPLAY_NAME,
    alternateName: [...PRODUCT_ALTERNATE_NAMES, ...moreNames],
    softwareVersion: await productSoftwareVersion(),
    applicationCategory: 'WebApplication',
    applicationSubCategory: 'Web performance optimization',
    license: 'https://www.apache.org/licenses/LICENSE-2.0',
    isAccessibleForFree: true,
    brand: PRODUCT_BRAND,
    publisher: ORGANIZATION_REF,
  };
}

export type ProductExtra = Record<string, unknown> & { alternateName?: string[] };

export async function productJsonLd(extra: ProductExtra = {}) {
  return { '@context': 'https://schema.org', ...(await productNode(extra)) };
}

// The short form, for a TechArticle's `about` and the like: the `@id` names
// the full node; the name and version are there for readers that do not
// follow references.
export async function productRef() {
  return {
    '@type': 'SoftwareApplication',
    '@id': PRODUCT_JSONLD_ID,
    name: PRODUCT_DISPLAY_NAME,
    softwareVersion: await productSoftwareVersion(),
    applicationCategory: 'WebApplication',
  };
}
