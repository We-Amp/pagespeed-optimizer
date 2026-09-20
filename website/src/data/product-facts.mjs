// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// product-facts.mjs — SINGLE SOURCE OF TRUTH for drift-prone product facts.
//
// WHY THIS FILE EXISTS
//   Prices, the launch-promo end date, version numbers, the bundled-nginx
//   version, the variant count and the BuiltWith heritage number used to be
//   hand-duplicated across src/pages/api/product.json.ts,
//   src/data/offer-jsonld.ts, src/data/launch-promo.ts and the two
//   public/llms*.txt files. Every release that touched one of them risked
//   leaving the others stale (the 1.1->1.15 renumber drifted across these
//   files before it was caught by hand).
//
// HOW DRIFT IS NOW PREVENTED
//   - product.json.ts / offer-jsonld.ts / launch-promo.ts IMPORT from here, so
//     there is exactly one copy of each value at compile time.
//   - public/llms.txt and public/llms-full.txt are GENERATED at prebuild by
//     scripts/generate-llms.mjs, which substitutes LLMS_TOKENS (below) into
//     scripts/llms-templates/*.tmpl. Hand-editing the generated .txt or
//     changing a fact here without regenerating fails the byte-equivalence
//     drift guard in test/sync/llms-generated.test.ts.
//
// This is a .mjs (not .ts) so the node prebuild generator can import it
// directly AND the Astro/Vite TypeScript consumers can too (tsconfig has
// allowJs). Keep it dependency-free and side-effect-free.

// --- Vendor / product identity / canonical site URLs ------------------------
// The identity facts the admin console also consumes — vendor and product
// names, the canonical site URLs — are DEFINED ONCE in
// ../../../shared/product-facts.mjs (the file the console syncs verbatim) and
// re-exported here unchanged, so every website importer keeps reading them
// from this module. test/sync/console-facts-sync.test.ts asserts the lockstep.
// Edit the shared file; never re-declare one of these names here.
// The import binds the names this file's own derivations (LLMS_TOKENS below)
// reference; the re-export keeps every website importer reading them from
// this module — `export { … } from` alone creates no local binding.
import {
  PRODUCT_NAME,
  SUPPORT_URL,
} from '../../../shared/product-facts.mjs';
export {
  VENDOR,
  WEBSITE,
  VENDOR_URL,
  PRODUCT_NAME,
  PRODUCT_DISPLAY_NAME,
  PRIVACY_URL,
  TERMS_URL,
  SUPPORT_URL,
} from '../../../shared/product-facts.mjs';
export const COMPANY_COUNTRY = 'The Netherlands';
export const COMPANY_FOUNDED_YEAR = 2012;
export const COMPANY_KVK = '57898138';

// --- License terms -----------------------------------------------------------
// Canonical support-terms URL. The trailing slash is canonical.
export const LICENSING_TERMS_URL = 'https://we-amp.com/licensing/';

// --- Offerings (converged line — placeholders) -------------------------------
// What is for sale on the converged line is two things: a support subscription
// and hardened attested builds. These two rows are what /pricing/ presents,
// replacing the earlier four-tier ladder (no tier names, no prices). Prices are
// NULL placeholders until pricing is published — no surface may render a price.
//   kind        'sla' (SLA-backed support) or 'attestation' (hardened builds
//               with a signed SBOM + build provenance via the subscriber
//               repository)
//   artifacts   the standard signed packages are free for everyone; the
//               hardened channel is sold (see ARTIFACT_ACCESS below)
export const SUPPORT_TIERS = [
  {
    id: 'support',
    name: 'Support subscription',
    kind: 'sla',
    prices: { annualUsd: null, monthlyUsd: null },
    note: 'support from the people who build the product, with agreed response times and SLA-backed delivery of security updates',
  },
  {
    id: 'hardened-builds',
    name: 'Hardened attested builds',
    kind: 'attestation',
    prices: { annualUsd: null, monthlyUsd: null },
    note: 'builds of the same code with a signed SBOM and build provenance, delivered through the subscriber repository',
  },
];
// The paid-artifact axis: the standard signed packages are free for everyone;
// the hardened channel (hardened builds with SBOM + signed build provenance)
// is sold as a subscription. Pricing to be announced; no surface may render an
// artifact price yet. Paid artifacts carry
// the same software license as the standard packages (SOURCE_PUBLICATION).
export const ARTIFACT_ACCESS = {
  standardPackages: 'free',
  hardenedBuilds: 'paid',
  hardenedPricing: 'to-be-announced',
};
// The support ladder as markdown bullets and one line, price-free by design
// (pure helpers — see the purity rule below).
const ladderMd = (rows) => rows.map((t) => `- ${t.name} — ${t.note}`).join('\n');
const ladderLine = (rows) => rows.map((t) => `${t.name} (${t.note})`).join('; ');
export const SUPPORT_LADDER_MD = /* @__PURE__ */ ladderMd(SUPPORT_TIERS);
export const SUPPORT_LADDER_LINE = /* @__PURE__ */ ladderLine(SUPPORT_TIERS);

// PURITY RULE FOR THIS FILE
//   Every top-level derivation below is either a plain literal or a call
//   annotated /* @__PURE__ */ whose arguments are bare identifiers. Downstream
//   bundlers (the 1.15 web console imports only names and URLs from this file)
//   can then drop the whole support-tier / apt-matrix block as dead code
//   instead of embedding the literals. Keep it that way: no bare
//   `X.map(...).join(...)` or `Object.fromEntries(X.map(...))` at top level —
//   wrap the derivation in a helper and call it through the annotation.

// Google-recommended Offer freshness date; bump ~yearly (see offer-jsonld.ts).
export const PRICE_VALID_UNTIL = '2026-12-31';

// --- Heritage (BuiltWith) ---------------------------------------------------
// Display strings (locale-independent) — these are facts as published, not
// computed, so store the rendered form to keep generation deterministic.
export const BUILTWITH_SITES = '231,341';
export const BUILTWITH_AS_OF = 'May 2026';
export const BUILTWITH_HISTORICAL = '1.9 million+';

// --- Image pipeline (optimizer worker) ---------------------------------------
export const MAX_VARIANTS = 37; // 36 raster + 1 SVG
export const RASTER_VARIANTS = 36; // 3 formats x 3 viewports x 2 densities x 2 Save-Data
// IMAGE_FORMATS (the flat 2.0 format list) is DERIVED from IMAGE_FORMAT_SUPPORT
// in the "Image format support" section below — it has to sit after V1_LINE.

// --- Versions ---------------------------------------------------------------
// The 1.15 line: the maintained continuation of Google's open-source
// mod_pagespeed, renumbered from 1.1 (forward-semver successor to the last
// upstream release, 1.14.36.1 — the final Apache-incubator release; Google's
// last stable was 1.13.35.2). NOTE: this is the marketing LINE version that appears in
// human-facing copy — it is NOT the per-release semver (that lives in the
// release manifests, releases/{1.1,2.0}.yaml). /1.1/ URL paths are
// intentionally frozen and are NOT derived from V1_LINE.
export const V1_LINE = '1.15';
export const V1_RENUMBERED_FROM = '1.1';
// 1.14.36.1 was the final UPSTREAM release — the one Apache-incubator
// (incubating) release, Aug 2020. It was NOT a Google release; Google's last
// stable was 1.13.35.2 (Feb 2018). Named "upstream" (not "Google") accordingly.
export const LAST_UPSTREAM_VERSION = '1.14.36.1';
// ModPageSpeed 2.0 GA date (the ASP.NET Core middleware went GA the same day).
export const V2_GA_DATE = '2026-05-17';
// The 2.0 marketing LINE label, the counterpart of V1_LINE. Use the line
// constants as the edition keys everywhere — never a bare string literal, and
// never the frozen /1.1/ URL path. CURRENT_LINE is the converged line every
// current-product surface derives from, the ASP.NET Core middleware included.
export const V2_LINE = '2.0';
export const CURRENT_LINE = '2.1';

// --- Image format support (format-first, edition-keyed, port-scoped) --------
//
// CANONICAL SOURCE OF TRUTH for "which image format does which product edition
// encode, on which ports". Every image-format claim on the site — copy, table
// cell, JSON API, llms.txt — must trace back to this table.
//
// READ BEFORE EDITING
//
//   1. AN ABSENT PORT IS A PORT WE DO NOT CLAIM. This is an ALLOWLIST of
//      VERIFIED support, never a best-effort sketch. Adding a port here
//      licenses every downstream surface to claim it, so only add one after
//      verifying it against the product source.
//
//   2. ENVOY IS DELIBERATELY ABSENT FROM EVERY ROW. The 1.15 Envoy port is not
//      shipped, is excluded from CI, and its AVIF status is UNVERIFIED. Envoy
//      must never be claimed — or implied — to encode AVIF. A guard test in
//      test/content-accuracy.test.ts asserts that 'Envoy' appears in no port
//      list here. Do NOT "fix" that test by adding Envoy to this table.
//
//   3. EDITIONS ARE THE MARKETING LINE LABELS (V1_LINE = '1.15', V2_LINE =
//      '2.0'), consistent with how V1_LINE is defined above — NOT the frozen
//      /1.1/ URL path, and NOT a per-release semver.
//
//   4. RENDERING RULE (approved editorial policy, mps2 #1000): rendered marketing copy
//      fixes EDITION scoping ONLY. Ports are modelled here because the model
//      must be precise, but NO helper below emits an edition label glued to a
//      port list, and no copy may render ports, opt-in caveats, Accept-header
//      notes, or Experimental labels. editionClause() is the sanctioned copy
//      helper and never emits a port. Ports stay separately retrievable via
//      portsFor() for non-copy consumers (the JSON API) only.
//
// FACTS ENCODED HERE (verified 2026-07):
//   - The 1.15 line ships AVIF encoding on Apache, nginx (standard and
//     lite) and the native IIS module; the AV1 encoder is statically linked.
//     AVIF is OPT-IN: its four filters sit outside rewrite_images and outside
//     CoreFilters. The IIS module is x64-only and labelled Experimental. None
//     of that nuance is rendered into copy — see rule 4.
//   - The 2.0 line has AVIF in the nginx worker and the ASP.NET Core
//     middleware.
//   - SVG auto-vectorization stays 2.0-only (as do Jpegli and ML-predicted
//     quality, which are not image FORMATS and so are not modelled here).
//   - Ships on BOTH lines, never fence to 2.0: variant-aware caching and
//     zero-copy serving. 1.15 varies its cache on client capability
//     (image format, mobile UA, Save-Data, small-screen) and ships zero-copy
//     serving on nginx, Apache and IIS as of v1.15.0+r19, opt-in via
//     CycloneZeroCopy. What IS 2.0-only is narrower: tablet/desktop viewport
//     classes, pixel density, transfer-encoding alternates, and proactive
//     generation of the full variant matrix. This comment previously made the
//     wrong claim and no guard caught it -- see rule f3.
// The rows below reference these lists directly (no spread copy): a spread is a
// side effect to bundlers and would pin the whole table into consumers that
// import only names and URLs. portsFor() copies on read, so nothing shares a
// mutable array with a caller.
const PORTS_1_15 = ['Apache', 'nginx', 'IIS']; // no Envoy — see rule 2 above
const PORTS_2_0 = ['nginx', 'ASP.NET Core'];
// Converged 2.1: the module ships on Apache and nginx; WebP + AVIF encode in
// the module (carried from the 1.x line). SVG auto-vectorization runs in the
// optimizer worker, whose module wiring is Apache-first — VERIFY the SVG port
// list at the GA pin before this ships (parked-PR checklist item).
const PORTS_2_1 = ['Apache', 'nginx'];
const PORTS_2_1_WORKER = ['Apache'];

export const IMAGE_FORMAT_SUPPORT = [
  {
    format: 'WebP',
    editions: {
      [V1_LINE]: { ports: PORTS_1_15 },
      [V2_LINE]: { ports: PORTS_2_0 },
      [CURRENT_LINE]: { ports: PORTS_2_1 },
    },
  },
  {
    format: 'AVIF',
    editions: {
      [V1_LINE]: { ports: PORTS_1_15 },
      [V2_LINE]: { ports: PORTS_2_0 },
      [CURRENT_LINE]: { ports: PORTS_2_1 },
    },
  },
  {
    format: 'SVG',
    editions: {
      [V2_LINE]: { ports: PORTS_2_0 },
      [CURRENT_LINE]: { ports: PORTS_2_1_WORKER },
    },
  },
];

// format -> row lookup, e.g. FORMAT_SUPPORT.AVIF.editions['1.15'].ports.
const byFormat = (rows) => Object.fromEntries(rows.map((r) => [r.format, r]));
export const FORMAT_SUPPORT = /* @__PURE__ */ byFormat(IMAGE_FORMAT_SUPPORT);

/** Edition labels that encode `format`, in table order. e.g. ['1.15', '2.0']. */
export function editionsFor(format) {
  return Object.keys(FORMAT_SUPPORT[format]?.editions ?? {});
}
/** Formats `edition` encodes, in table order. e.g. formatsFor('2.0') -> WebP, AVIF, SVG. */
export function formatsFor(edition) {
  return IMAGE_FORMAT_SUPPORT.filter((r) => edition in r.editions).map((r) => r.format);
}
/** Ports of `edition` that encode `format`. NOT for rendered copy — see rule 4. */
export function portsFor(format, edition) {
  return [...(FORMAT_SUPPORT[format]?.editions?.[edition]?.ports ?? [])];
}

/** 'a', 'a and b', 'a, b, and c'. */
function joinList(items) {
  if (items.length <= 1) return items[0] ?? '';
  if (items.length === 2) return `${items[0]} and ${items[1]}`;
  return `${items.slice(0, -1).join(', ')}, and ${items[items.length - 1]}`;
}

/**
 * Render the image-format clause the vs/* comparison rows need: the bare list
 * of formats the converged line encodes, with no line enumeration:
 *
 *   editionClause(['WebP', 'AVIF'])         -> 'WebP and AVIF'
 *   editionClause(['WebP', 'AVIF', 'SVG'])  -> 'WebP, AVIF, and SVG'
 *   editionClause(['SVG'])                  -> 'SVG'
 *
 * Never emits a port or an edition label (rule 4). A format the converged line
 * does not encode, and an unknown format, are skipped rather than guessed. The
 * edition/port model stays retrievable for non-copy consumers (the JSON API)
 * through editionsFor() and portsFor(), whose shapes do not move.
 */
export function editionClause(formats = IMAGE_FORMAT_SUPPORT.map((r) => r.format)) {
  return joinList(formats.filter((format) => editionsFor(format).includes(CURRENT_LINE)));
}

// BACK-COMPAT: the flat format list, unchanged in name, shape and value
// (['WebP', 'AVIF', 'SVG']). Its consumers — src/pages/api/product.json.ts and
// src/pages/self-hosted-image-optimization.astro — must render byte-identically.
// Derived from the converged line; the value is identical to the old
// 2.0-derived list, so the public contract does not move.
export const IMAGE_FORMATS = /* @__PURE__ */ formatsFor(CURRENT_LINE);

// --- NuGet packages ---------------------------------------------------------
export const PKG_ASPNETCORE = 'WeAmp.PageSpeed.AspNetCore'; // the ASP.NET Core middleware
export const PKG_SIDECAR = 'WeAmp.PageSpeed.Sidecar'; // mod_pagespeed 1.15 sidecar
export const PKG_SIDECAR_NATIVE = 'WeAmp.PageSpeed.Sidecar.NativeAssets.Linux';
export const SIDECAR_NGINX_VERSION = '1.30.2'; // nginx bundled inside the 1.15 sidecar
export const SIDECAR_RIDS = ['linux-x64', 'linux-arm64'];
export const ASPNETCORE_RIDS = ['linux-x64', 'linux-arm64', 'osx-arm64', 'win-x64'];
// Target frameworks the ASP.NET Core middleware ships for.
// Authoritative for the .NET runtime requirement (.NET 8 or .NET 10) — NOT .NET 9.
// Mirrors aspnet-getting-started.mdx ("targets net8.0 and net10.0"). Keep this in
// lockstep with the package's <TargetFrameworks>; the content-accuracy guard asserts it.
export const ASPNETCORE_TFMS = ['net8.0', 'net10.0'];

// --- Stock nginx targets for the signed apt/yum 1.15 nginx packages ----------
// The nginx-module-pagespeed dynamic module is prebuilt and signed per distro,
// pinned to that distro's stock nginx. Each .so is exact-version-pinned to its
// distro's nginx (nginx's --with-compat does NOT relax the version check), so
// upgrading nginx past the stock version needs a matching rebuild (contact us).
// amd64 + arm64 for every row.
export const STOCK_NGINX_UBUNTU = '1.24.0'; // Ubuntu 24.04 noble
export const STOCK_NGINX_ALMA = '1.20.1'; // AlmaLinux 9 (rpm/Apache target)

// The signed apt matrix for nginx-module-pagespeed (Debian + Ubuntu), each row
// pinned to the distro's stock nginx. Order is the install-doc display order.
// Debian 11 (bullseye) is deliberately absent: the repository's bullseye
// suite tops out at the final 1.15.0 module packages (glibc 2.34, needed by
// the 2.1 serving components, is not available there) — see
// migrating-to-2-1.md's Debian 11 platform note and release-notes.mdx's
// current-release operator note.
export const NGINX_APT_DISTROS = [
  { distro: 'Debian 12 bookworm', nginx: '1.22.1' },
  { distro: 'Debian 13 trixie', nginx: '1.26.3' },
  { distro: 'Ubuntu 22.04 jammy', nginx: '1.18.0' },
  { distro: 'Ubuntu 24.04 noble', nginx: STOCK_NGINX_UBUNTU },
];
export const NGINX_APT_ARCHES = 'amd64 + arm64';
// Compact prose listing of the prebuilt apt matrix, e.g.
// "Debian 11 bullseye (nginx 1.18.0), Debian 12 bookworm (nginx 1.22.1), …".
const aptMatrix = (rows) => rows.map((d) => `${d.distro} (nginx ${d.nginx})`).join(', ');
export const NGINX_APT_MATRIX = /* @__PURE__ */ aptMatrix(NGINX_APT_DISTROS);

// --- Software license -------------------------------------------------------
// Single source of truth for the license the software is distributed under,
// mirrored by the machine-readable /api/product.json `license` and
// `source_publication` fields AND the human /license/ page, so those surfaces
// cannot drift on the license name, its SPDX id, or the status. The sync test
// (test/sync/source-publication-sync.test.ts) pins the record and fails if a
// page re-states any of it as a literal.
export const SOURCE_PUBLICATION = {
  status: 'published', // source repositories went public 2026-09-16
  license: 'Apache License 2.0', // full name (first mention in prose)
  licenseId: 'Apache-2.0', // SPDX id (product.json `license` / `source_publication.license`)
};
// Publication-derived phrasing. "Licensed under the Apache License 2.0" is
// true today; "open source" is true only once the public repositories exist
// (`status: 'published'`). Copy that asserts either MUST go through these, so
// the claim flips with the fact and never by hand — the sync test scans for
// hard-coded "open source" / "open-source" claims about the current product.
export const SOURCE_IS_PUBLISHED = SOURCE_PUBLICATION.status === 'published';
const licenseClause = (sp, published) =>
  published ? `open source under the ${sp.license}` : `licensed under the ${sp.license}`;
export const LICENSE_CLAUSE = /* @__PURE__ */ licenseClause(
  SOURCE_PUBLICATION,
  SOURCE_IS_PUBLISHED,
);
const capitalize = (s) => s.charAt(0).toUpperCase() + s.slice(1);
export const LICENSE_CLAUSE_CAP = /* @__PURE__ */ capitalize(LICENSE_CLAUSE);
// The one-line CTA footer most pages carry. Derived, so the publication flip
// reaches every page at once.
const freeToRun = (cap) => `${cap} — free in development and in production.`;
export const FREE_TO_RUN_LINE = /* @__PURE__ */ freeToRun(LICENSE_CLAUSE_CAP);

/**
 * Token map consumed by scripts/generate-llms.mjs. Every value is the EXACT
 * string that must appear in the generated public/llms*.txt at the
 * corresponding {{TOKEN}} site. Derive from the typed constants above so there
 * is one source — never hardcode a second copy of a number here.
 */
const llmsTokens = () => ({
  PRODUCT_NAME,
  CURRENT_LINE,
  V2_LINE,
  LICENSE_NAME: SOURCE_PUBLICATION.license,
  LICENSE_ID: SOURCE_PUBLICATION.licenseId,
  LICENSE_CLAUSE,
  LICENSE_CLAUSE_CAP,
  SUPPORT_LADDER_MD,
  SUPPORT_LADDER_LINE,
  SUPPORT_URL,
  LICENSING_TERMS_URL,
  MAX_VARIANTS: String(MAX_VARIANTS),
  RASTER_VARIANTS: String(RASTER_VARIANTS),
  SIDECAR_NGINX_VERSION,
  BUILTWITH_SITES,
  BUILTWITH_AS_OF,
  BUILTWITH_HISTORICAL,
  LAST_UPSTREAM_VERSION,
  V1_LINE,
  V1_RENUMBERED_FROM,
  V2_GA_DATE,
  STOCK_NGINX_UBUNTU,
  STOCK_NGINX_ALMA,
  NGINX_APT_MATRIX,
  NGINX_APT_ARCHES,
  COMPANY_KVK,
  COMPANY_FOUNDED_YEAR: String(COMPANY_FOUNDED_YEAR),
});
export const LLMS_TOKENS = /* @__PURE__ */ llmsTokens();
