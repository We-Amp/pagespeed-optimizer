# CLAUDE.md — modpagespeed.com (`website/`)

This directory is the modpagespeed.com site: the marketing pages, the product
documentation, the blog, the examples gallery, the demo and calculator pages, and
the machine-readable surfaces (`/llms.txt`, `/api/product.json`, the Helm chart
repository under `/charts/`). The root `CLAUDE.md` covers the rest of the repository;
this file covers only the site.

**This tree is the site's source of truth.** Every change to the site is made here
and lands on `main` through a pull request, like any other change in this repository.
Nothing is imported from elsewhere and nothing here is overwritten by an export.
Deploys are done by the operator from this tree; CI in this repository builds and
tests the site but never deploys it.

## Stack

- Astro 7 with the MDX integration, Tailwind CSS 4 (`@tailwindcss/vite`), TypeScript.
- Server output through `@astrojs/node` in standalone mode: `npm run build` writes the
  static assets and prerendered pages to `dist/client/` and the server entry to
  `dist/server/entry.mjs`. The `Dockerfile` here packages that output into the
  container the site runs from.
- Prettier (`prettier-plugin-astro`) and ESLint (`eslint-plugin-astro`); `astro check`
  for types.

## Commands

Run everything from `website/`.

| Command | Purpose |
|---|---|
| `npm ci` | Install. npm is canonical; `package-lock.json` is the only lockfile. |
| `npm run dev` | Dev server at `http://localhost:4321`. |
| `PRICING_ALLOW_STALE=1 npm run build` | Production build. The prebuild step fetches localized prices; without network or credentials it refuses unless this variable is set, in which case it builds from the committed `src/data/fastspring-pricing.json`. CI always sets it. |
| `npm run format` / `npm run format:check` | Prettier over `src/`. Run before staging. |
| `npm run lint` | ESLint plus `astro check`. |
| `npx vitest run` | Unit tests and the sync/content gates in `test/`. Fast; run this first. |
| `npx playwright test` | Browser suite in `tests/`. Starts the dev server itself (`webServer` in `playwright.config.ts`); run `npx playwright install chromium` once. |
| `npm run gen:llms` | Regenerate `public/llms.txt`, `public/llms-full.txt` and `public/.well-known/ai-plugin.json`. |

The prebuild step also regenerates the blog title cards (`public/og-cards/`) and
the agent files, so a build never ships stale generated output.

Astro 7 detects an AI-agent session and then starts `astro dev` as a background
process (`npx astro dev status|logs|stop` manage it). Playwright's `webServer` sees
that parent exit and fails with "Process from config.webServer exited early", so in
such a session start the dev server first (`npm run dev`, which backgrounds itself), run
`npx playwright test` with `CI` unset so it reuses the running server, and finish with
`npx astro dev stop`. A human terminal and CI are not affected.

## Directory map

```
src/pages/                   File-based routes: one .astro (or .ts endpoint) per route.
                             The live route set is `git ls-files 'website/src/pages/*'`;
                             do not keep a hand-written site map anywhere.
src/content/docs/            Product documentation (.md, .mdx) -> /docs/<slug>/
src/content/blog/            Blog posts (.md) -> /blog/<slug>/; frozen once published
src/content/how-it-works/    Architecture explainers -> /how-it-works/<slug>/
src/content/releases-2.1/    release.yaml: the version manifest of the current line
src/content/releases-2.0/    Manifests of the predecessor lines (frozen; read-only
src/content/releases-1.1/    data for the pages that still describe them)
src/content.config.ts        Collection schemas: the frontmatter contracts below
src/components/              UI components. src/components/release/ holds the
                             release-aware components; its README.md is the manual.
src/data/                    product-facts.mjs (single-source facts; re-exports the
                             repo-root shared/product-facts.mjs), filters.ts,
                             examples.ts + examples-data.json, demo-metrics.json,
                             fastspring-pricing.json, JSON-LD builders
src/layouts/BaseLayout.astro The one layout: <head>, nav, footer, JSON-LD, the
                             critical-CSS dark-mode overrides, the provenance notice
src/lib/release.ts           getRelease(line) and artifactUrl(): typed manifest access
public/                      Served as-is: fonts, images, og-cards/, charts/ (the
                             Helm repository: packaged charts + index.yaml), robots.txt,
                             llms.txt, llms-full.txt, .well-known/ai-plugin.json
scripts/                     Build-time generators: fetch-pricing.js, generate-llms.mjs,
                             generate-title-cards.mjs; templates in scripts/llms-templates/
test/                        vitest: unit tests and the gates listed below (CI runs these)
tests/                       Playwright specs (the browser suite)
```

## Content model

**Docs** (`src/content/docs/*.md|mdx`): `title`, `description`, `order` (position
within its group), `group` (sidebar section), `lastUpdated` (drives sitemap lastmod
and `dateModified`), optional `datePublished`, optional `faq` (q/a pairs rendered as
FAQPage JSON-LD; every pair must also appear verbatim in the page body).

**Blog** (`src/content/blog/*.md`): `title`, `description`, `date`, `author`, `tags`,
`draft`, optional `lastUpdated`, `coverImage`, `product`, `pinned`, `howTo`, `faq`.
A published post is frozen content: it keeps the version strings and URLs of its day,
and it is exempt from the version guards for that reason.

**How it works** (`src/content/how-it-works/*.md`): `title`, `description`, `order`,
optional `summary`, `lastUpdated`, `datePublished`, `faq`.

**Versions.** `src/content/releases-2.1/release.yaml` is the single version source for
the current line: `release.semver`, `release.tag`, `released_on`, the artifact channels
and the compatibility matrix. Rule: **never hand-pin a version or a `/releases/vX.Y.Z/`
URL in non-frozen content.** Pages call `getRelease('2.1')` from `src/lib/release.ts`;
MDX docs use the components in `src/components/release/` (`<Version />`,
`<CompatMatrix />`, `<ImageTag />`, ...). Plain `.md` cannot render components: a docs
page that needs version content becomes `.mdx`. The one tolerated literal is a
`ghcr.io/we-amp/pagespeed-*:X.Y.Z` image tag inside a `.md` install snippet, and CI
checks that it equals the manifest semver. Both rules are enforced by the `Website`
workflow (`.github/workflows/website.yml`), which also fails when the manifest lags
the latest stable `v2.*` tag of the public mod_pagespeed repository by more than a
few hours.

## Gates in `test/`

`npx vitest run` runs them all; the two build-output gates skip themselves when
`dist/` is absent and run for real after `npm run build`.

- `content-accuracy.test.ts` — denylist of known-wrong product claims (phantom
  versions, wrong platform or edition claims) over content, pages, data, templates and
  the generated agent files; the canary fixtures prove every rule still fires.
- `anchor-gate.test.ts` — after a build: every deep-link anchor listed in
  `test/fixtures/legacy-doc-anchors.txt` resolves in `dist/client/docs/`.
- `anchor-source-gate.test.ts` — the same anchor contract computed from the markdown
  source, so it needs no build and always runs.
- `release-notes-ids-gate.test.ts` — after a build: every id in
  `test/fixtures/release-notes-ids.txt` exists on the built `/docs/release-notes/` page.
- `sync/airead-gate-sync.test.ts` — the gate predicates inlined into
  `/ai-readability/` still match their golden contract.
- `sync/console-facts-sync.test.ts` — the repo-root `shared/product-facts.mjs` (also
  consumed by the management console) and `src/data/product-facts.mjs` stay in lockstep.
- `sync/filters-sync.test.ts` — `src/data/filters.ts` matches the filter table in
  `src/content/docs/filter-reference.md` row for row.
- `sync/llms-generated.test.ts` — the committed `public/llms*.txt` and
  `ai-plugin.json` are byte-equal to a fresh render of the templates and facts.
- `sync/source-publication-sync.test.ts` — license and publication facts agree across
  `product-facts.mjs`, `/api/product.json` and `/license/`, and retired commercial
  wording (license keys, trials, per-site pricing) stays out.
- `sync/terms-version-sync.test.ts` — the terms-of-service version stamp is identical
  on `/terms/` and `/buy/`.

## Regenerating the agent files

`public/llms.txt`, `public/llms-full.txt` and `public/.well-known/ai-plugin.json` are
generated: `scripts/generate-llms.mjs` fills the `{{TOKEN}}` holes in
`scripts/llms-templates/*.tmpl` from `LLMS_TOKENS` in `src/data/product-facts.mjs`.
When a docs change alters something these files describe (features, configuration,
install steps, versions, support terms), edit the fact or the template, run
`npm run gen:llms`, and commit the regenerated files. Never hand-edit them: the
byte-equivalence gate rejects it.

## Copy rules

These bind every customer-facing string: pages, docs, blog, templates, metadata.

- The product is **mod_pagespeed 2.1** (lowercase, underscore). "ModPageSpeed 2.0" and
  "mod_pagespeed 1.15" name the predecessor lines and appear only as predecessors;
  1.15 upgrades to 2.1 in place, so never frame them as two products to choose between.
- **"worker", never "daemon".** Also: optimize (not supercharge), self-hosted (not
  on-prem), variant (not copy), original (not unoptimized), serves (not delivers).
- Sentence-case headings. Short sentences, active voice, no filler; name trade-offs.
- No internal identifiers: no decision-record numbers, machine names, private
  repository names or work-item ids. `tools/ci/check-public-hygiene.sh` (run from the
  repository root) is the gate and CI fails on a hit.
- Provenance: mod_pagespeed is an open-source project originally developed at Google;
  mod_pagespeed 2.1 is developed by We-Amp B.V. and is not affiliated with or endorsed
  by Google. Say nothing beyond that about the relationship, and never disparage the
  original authors' work.
- Prices, license and publication claims come from `src/data/product-facts.mjs`
  (`SUPPORT_TIERS`, `LICENSE_CLAUSE`, `SOURCE_PUBLICATION`), never from a literal. No
  "free trial" and no license keys anywhere on the site.
- Dark mode: the site runs behind its own critical-CSS inlining, which extracts without
  the `.dark` class. Every `dark:` utility on an above-the-fold element needs a matching
  inline override in the `<style is:inline>` block of `BaseLayout.astro`.

## CI and deploys

The `Website` workflow runs on every push and pull request that touches `website/**`:
the two content guards, `npm ci`, `npx vitest run`, a production build with
`PRICING_ALLOW_STALE=1`, and, in a second job, the Playwright suite against the dev
server. A manual workflow (`refresh-examples-gallery.yml`) re-measures the examples
gallery against the live demo and files an issue on drift. Two tools outside this
directory write into it: `tools/examples-gallery/generate.mjs` (writes
`src/data/examples-data.json`) and `deploy/helm/package-chart.sh` (writes
`public/charts/`). Deploys are the operator's step, from this tree, never from CI.
