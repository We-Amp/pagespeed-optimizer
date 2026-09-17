# CLAUDE.md — ModPageSpeed 2.0 Website

## Project Overview

Commercial website for ModPageSpeed 2.0 at modpagespeed.com. Built with Astro +
Tailwind CSS. Serves as marketing site, documentation hub, demo tool, and license
management portal.

See `../business-plan.md` for the full business plan.

## Tech Stack

- **Framework:** Astro 5 (static-first, island architecture for interactive bits)
- **CSS:** Tailwind CSS 4 (via `@tailwindcss/vite` plugin)
- **Language:** TypeScript (strict mode)
- **Linting:** ESLint with `eslint-plugin-astro`
- **Formatting:** Prettier with `prettier-plugin-astro`
- **Type checking:** `astro check` (via `@astrojs/check`)

## Commands

```bash
# Development
npm run dev              # Start dev server (localhost:4321)
npm run build            # Production build to dist/
npm run preview          # Preview production build

# Quality (run BEFORE staging/committing)
npm run format           # Format all source files with Prettier
npm run format:check     # Check formatting without writing
npm run lint             # Run ESLint + astro check
npm run lint:fix         # Auto-fix ESLint issues

# Tests
npm run test:unit        # vitest run — FAST unit/drift loop; reach for this first
npm run test:gate        # vitest run — targeted ai-readability gate-sync drift test
npm run test             # playwright test — SLOW E2E; needs `npx playwright install` + a running server
npm run test:ui          # playwright test --ui — interactive E2E runner
```

Run `npm run test:unit` first: it is the fast loop and covers the generated-file
drift gates (e.g. `llms.txt`) you most often need. `npm run test` is the heavy
Playwright path and requires browsers plus a server, so use it only for E2E.

## Pre-Commit Workflow

Pre-commit hooks run Prettier and ESLint on `website/` files automatically.

**Before staging files**, run formatting to avoid the hook modifying files:

```bash
cd website && npm run format
```

**Before committing**, verify everything passes:

```bash
cd website && npm run lint
```

**When updating docs content** (pricing, features, config flags, installation steps),
regenerate `public/llms.txt` and `public/llms-full.txt` with `npm run gen:llms` (run
from `website/`) — never hand-edit them. They are generated from site data, and a
byte-equivalence drift test (`test/sync/llms-generated.test.ts`) rejects manual edits.

## Directory Structure

```
website/
├── src/
│   ├── layouts/         # Base layout (header, footer, nav)
│   ├── components/      # Reusable UI components
│   ├── pages/           # File-based routing (each .astro = a page)
│   │   ├── index.astro           # Landing page
│   │   ├── features.astro        # Feature breakdown
│   │   ├── pricing.astro         # Pricing + annual toggle
│   │   ├── demo.astro            # Live demo tool
│   │   ├── calculator.astro      # Savings calculator
│   │   ├── license.astro         # Source license (Apache-2.0)
│   │   ├── terms.astro           # Terms of service
│   │   ├── security.astro        # Security policy
│   │   ├── contact.astro         # Contact / enterprise
│   │   ├── docs/                 # Documentation pages
│   │   └── blog/                 # Blog posts
│   ├── data/            # Generated data (demo-metrics.json)
│   ├── content/         # Markdown content (blog posts, docs)
│   └── styles/
│       └── global.css   # Tailwind imports + custom styles
├── public/              # Static assets (images, fonts, favicons)
├── tests/
│   └── e2e-webhook/     # Docker-based webhook integration tests
├── Dockerfile           # Multi-stage build for SSR container
├── docker-compose.test.yml  # E2E test orchestration
├── astro.config.mjs     # Astro configuration
├── eslint.config.js     # ESLint flat config
├── .prettierrc          # Prettier config
├── tsconfig.json        # TypeScript config
└── package.json         # Dependencies and scripts
```

## Design Guidelines

- **Performance target:** Lighthouse 100 across the board — the site itself proves
  the product works
- **Design philosophy:** Minimal, documentation-first, substance over flash
- **Zero JS by default:** Only use client-side JS for interactive islands (demo tool,
  calculator, pricing toggle)
- **Accessibility:** Semantic HTML, proper ARIA, keyboard navigable
- **Mobile-first:** Responsive design, touch-friendly

## Critical-CSS dark-mode gotcha (issue #337)

This site is dogfooded behind ModPageSpeed's own critical-CSS inlining. The
critical-CSS extractor analyses the page in its rendered state at extraction
time, and that render happens **without** the `.dark` class on `<html>`. So the
extractor never sees any `.dark` / `dark:` selectors and never inlines them as
critical CSS — every `dark:` Tailwind variant on a critical-render-path element
then flashes the light value (or shows a missing element) on PageSpeed-optimized
loads until the full stylesheet finishes.

**Required pattern:** for every `dark:` utility on an above-the-fold element
(page background, header/nav colors, mobile-menu drawer, card backgrounds, the
header logo `dark:hidden`/`dark:block` swap), add a matching inline override in
the `<style is:inline>` block in `src/layouts/BaseLayout.astro` that hard-sets
the dark value with `!important`, e.g.:

```css
html.dark {
  background: #0c0a09 !important;
}
html.dark img[class*='dark:...'] {
  display: ... !important;
}
```

The pattern is reactive — one inline override per missed `dark:` utility on a
critical element. **When you add a new `dark:` utility to `BaseLayout.astro` (or
any other critical-render-path template), add the matching inline override in the
same change.** This has bitten the site repeatedly (PR #336 dark logo, page
background, dropdown panel, card backgrounds). See issue #337 for the full
history.

## Routes

Routes are file-based: each `.astro` (or `.ts` endpoint) under `src/pages/` is a
route. Do NOT maintain a hand-curated site map here — it drifts. The live route set
is `git ls-files 'website/src/pages/*'`. Notable areas (not exhaustive):

- Marketing: `index`, `features`, `pricing`, `demo`, `calculator`, `about`, `contact`
- Support / license: `buy/` (placeholder), `pricing`, `hosting-partners`, `license`,
  `terms`, `privacy`, `security` (`/claim-license/` and `/community-license/` are 301s
  to `/license/`; `/docs/license-activation/` is a 301 to `/docs/license/`)
- Docs: `docs/` (2.0), `1.1/` (1.1 engine pages + `1.1/docs/`)
- Comparison/SEO: `vs/`, `alternatives/`, `core-web-vitals/`, `how-it-works/`,
  `examples/`, `blog/`
- Tools/products: `analyze`, `ai-readability/`, `wordpress`, `psol`, `download/`
- Endpoints: `api/product.json.ts`, `blog/rss.xml.ts`
- Error pages: `404`, `500`, `error/`

## Branding

- **Product name:** ModPageSpeed 2.0
- **Company:** We-Amp B.V.
- **Tagline:** "Built by a mod_pagespeed maintainer. Rebuilt from scratch for modern infrastructure."
- **Primary angle:** Self-hosted sovereignty — "Your servers, your rules."
- **Disclaimer (must appear on site):** "mod_pagespeed is an open-source project
  originally developed at Google. ModPageSpeed 2.0 is developed by We-Amp B.V.
  and is not affiliated with or endorsed by Google."

## 1.1 ↔ 2.0 Positioning (different strengths, not better/worse)

**Thesis:** mod_pagespeed 1.1 and ModPageSpeed 2.0 are two first-class,
actively-maintained products on the same per-site price ladder (one ladder covers
both engines — ADR-092; never restate a price literal, see Pricing below). They
share one optimization core (PSOL) and one cache (Cyclone) but ship **different
deployment architectures**. The reader chooses by deployment fit, never by a
quality or recency ranking.

**The axis each owns:**

- **1.1 — in-process module.** Native module across the broadest server matrix
  (Apache + nginx + IIS GA, Envoy experimental — the only product native inside
  Apache and IIS, the IISpeed successor). Drop-in adoption: same ModPagespeed
  directives, same filters, same behavior, no config rewrite. In-process
  simplicity (no separate worker, no sidecar, no reverse-proxy hop, no Docker).
  The wider classic filter set (combine_css/combine_javascript, image spriting,
  IPRO, domain mapping, DNS prefetch). Built-in `/pagespeed_admin/` UI.
- **2.0 — out-of-process worker.** Async worker behind an nginx reverse proxy
  fronting any HTTP origin; native ASP.NET Core middleware (WeAmp.PageSpeed
  .AspNetCore NuGet, P/Invoke). The newest image pipeline (SVG
  auto-vectorization, Jpegli, ML-predicted quality — AVIF is NOT on this list
  any more; 1.15 ships it too, see the AVIF fact below). Variant-aware caching via a
  32-bit capability bitmask plus zero-copy mmap serving. Container/K8s native
  (Docker + Helm), SvelteKit web console, Prometheus, the RenderPeek lineage.

**FACT — 1.15 DOES ship AVIF (this reverses the pre-1.15 instruction).**
mod_pagespeed 1.15 ships AVIF *encoding*, with the AV1 encoder statically linked.
An older version of this file said 1.1 had no AVIF; that was true of the 1.1 line
and is now stale. Do not re-derive the old claim from archived copy.

**Source of truth:** `src/data/product-facts.mjs` → `IMAGE_FORMAT_SUPPORT`
(format-first, edition-keyed, port-scoped) plus its helpers `editionsFor()`,
`formatsFor()`, `portsFor()` and the copy helper `editionClause()`. Never restate
an image-format/edition claim by hand — derive it, or match what that table says.
`test/content-accuracy.test.ts` guards the inverse claim.

What is true, in detail (this file is repo-internal instruction, so it carries
nuance the published copy deliberately omits):

- **AVIF on 1.15 ships on Apache, nginx (standard and lite), and the native IIS
  module.** 2.0 has AVIF in the nginx worker and the ASP.NET Core middleware.
- **AVIF on 1.15 is OPT-IN.** The four filters — `convert_jpeg_to_avif`,
  `convert_to_avif_lossless`, `convert_to_avif_animated`, `recompress_avif` —
  sit **outside** `rewrite_images` and **outside** CoreFilters. Never imply AVIF
  is on by default on 1.15.
- **Envoy AVIF is UNVERIFIED and must never be claimed or implied.** The 1.15
  Envoy port is not shipped and is excluded from CI. Envoy appears in no port
  list in `IMAGE_FORMAT_SUPPORT`, and a guard test asserts its absence. Do not
  add it, and do not write a sentence whose port list could be read as
  including it.
- **The IIS module is x64-only and labelled Experimental.**
- **The shipped `WeAmp.PageSpeed.Sidecar` packages do NOT have AVIF** — the
  published packages predate it. **The `WeAmp.PageSpeed.AspNetCore` NuGet
  package is a separate 2.0 product and does NOT inherit AVIF from 1.15.**
  Both of those "no AVIF" statements remain correct and may still be written.
- **Still genuinely 2.0-only:** SVG auto-vectorization, Jpegli, ML-predicted
  quality, and variant-aware caching. "2.0-only" claims scoped to *those* are
  correct — it is only AVIF that moved.
- **Google's archived 1.13.35.2 open-source build genuinely never had AVIF.** A
  claim correctly scoped to that archived build stays correct.

**RENDERING RULE — keep published copy MINIMAL and STRICTLY BARE.** Rendered
marketing copy fixes **edition scoping only**. Do NOT render port lists, the
opt-in caveat, Accept-header notes, or the Experimental label into marketing
prose — model them, do not emit them. Because WebP and AVIF now carry identical
edition sets, the clause collapses: write **"WebP and AVIF across 1.15 and 2.0"**
(what `editionClause(['WebP','AVIF'])` returns), not a per-format split. The full
nuance above belongs in docs and in this file, not in a comparison table cell.

**Equal visual weight.** The 1.1↔2.0 pair gets equal-saturation accents, equal
bullet counts, and peer position. If one card is elevated/featured, both are.
Muted/grey treatment is reserved ONLY for the legacy open-source "mod_pagespeed
1.x" column. Never build a table where 1.1 is bare "No" against 2.0's "Yes" — use
the substantive shape ("in-process module" / "via reverse proxy"). For the
image-format cell, do NOT hardcode a list here — derive it from
`IMAGE_FORMAT_SUPPORT` / `editionClause()` (see the AVIF fact above). The old
prescribed wording "WebP, JPEG, PNG, GIF" for 1.1 is **stale**: 1.15 also encodes
AVIF, so that cell now reads "WebP, AVIF, JPEG, PNG, GIF" (or, in a shared row,
"WebP and AVIF across 1.15 and 2.0").

**Banned for 1.1 relative to 2.0:** "upgrade", "the latest", "modern
architecture", "current line", "more capable". "rebuilt from scratch" is scoped to
2.0 only (never paired as the sole differentiator against a demoted 1.1 label).
"upgrade" / "drop-in upgrade" is allowed ONLY when the object is Google's archived
open-source mod_pagespeed/ngx_pagespeed.

## Brand Voice & Strategy (CRITICAL)

**Canonical source of truth (ADR-005):** the governed brand guide lives in the
corp meta-repo at `brand/BRAND-STRATEGY.md` (We-Amp/corp). Always reconcile
against it — it wins on any conflict. The local `BRAND-STRATEGY-PROPOSAL.md` is a
superseded early proposal kept only for history; do **not** treat it as authority.
Key rules below.

### Voice Attributes (priority order)

1. **Direct** — Short sentences. Active voice. No filler.
2. **Honest** — Name trade-offs and limitations.
3. **Authoritative** — Speak from deep experience.
4. **Opinionated** — Have a point of view and defend it.
5. **Dry wit** — Occasional understated humor. Never forced.

When in doubt, Direct and Honest always win.

### Tone Toward mod_pagespeed (CRITICAL)

The original mod_pagespeed authors are friends and collaborators. **Never
disparage their work.** The narrative is: mod_pagespeed was excellent engineering
for its era and constraints. The infrastructure landscape shifted (nginx
dominance, HTTP/2, containers), making a different architecture worthwhile. The
original team's low-level libraries are so well-built they form the foundation
of 2.0.

**Do:**

- Credit the original team's work explicitly
- Frame differences as "different approach for a different era"
- Say "no longer actively developed" (neutral, factual)
- Say "Apache-native design" (descriptive, not pejorative)
- Say "the optimization libraries underneath are the foundation of 2.0"

**Never:**

- Call the original "monolithic," "bloated," "fragile," or "unmaintained"
- Say code was "thrown away" or "discarded" — say "replaced"
- Use "fighting the architecture" or similar combative metaphors
- Imply the original team made bad decisions
- Use "nobody wanted to touch" or similar dismissive language

### Forbidden Sentence Patterns

- "With our..." openers
- "Whether you're... or..." constructions
- "Introducing..." announcements
- "We're excited to..." openers
- "Unlock the power of..." anything
- Rhetorical questions as headlines

### Word List

| Use            | Avoid                    |
| -------------- | ------------------------ |
| optimize       | supercharge, turbocharge |
| self-hosted    | on-premise, on-prem      |
| variant        | version, copy            |
| original       | unoptimized, raw         |
| worker         | daemon, service, engine  |
| interceptor    | module, plugin           |
| sovereignty    | control, ownership       |
| serves         | delivers, provides       |
| writes / reads | processes, handles       |

### Conference Test

Every piece of copy must pass: _Could you say this out loud at a tech conference
— in front of the original mod_pagespeed team — without feeling embarrassed?_

## Content Conventions

- No emojis in page content unless explicitly requested
- Use plain, direct language (the audience is technical)
- **Prices are data-driven — never hard-code a price literal in copy or docs.** The
  two offerings are defined once in `src/data/product-facts.mjs` (`SUPPORT_TIERS`: support
  subscription; hardened attested builds — all prices `null` until published — no surface may
  render a support or artifact price, and no tier names).
  The retired per-server and per-site prices must not appear anywhere. See the
  Pricing section below.
- **No license keys, anywhere on the site.** The site does not describe a key model,
  an activation step, or reassurance about their absence ("no license key needed", a
  key being ignored, and so on). The software is free to install and run, in
  development and in production; what is sold is support and hardened builds.
- **Publication claims derive from data.** "Licensed under the Apache License 2.0" is
  true today; "open source" is true only once the public repositories exist. Every
  such sentence goes through `LICENSE_CLAUSE` / `LICENSE_CLAUSE_CAP` /
  `FREE_TO_RUN_LINE` (derived from `SOURCE_PUBLICATION.status` in `product-facts.mjs`),
  never a literal.
- **No free trial.** Never write "free trial", "start a trial", or "no card" — there is
  nothing to trial; the software is free.
- Blog posts: depth over volume, AI for editing/polish only

## Pricing (data-driven)

What is for sale is support and the hardened build channel; the standard signed
packages are free. Tiers are defined ONCE and rendered from data. **Never hard-code a
price literal in a page, component, doc, or the `llms*.txt` files.**

- **Offerings (source of truth):** `src/data/product-facts.mjs` — `SUPPORT_TIERS` (exactly
  two rows: the support subscription and hardened attested builds; no tier names; prices
  `null` until published) plus the derived `SUPPORT_LADDER_MD` / `SUPPORT_LADDER_LINE`, and
  `ARTIFACT_ACCESS` (standard packages free, hardened builds paid, pricing to be announced).
- **Regional/localized prices:** `src/data/fastspring-pricing.json` is still fetched by
  `scripts/fetch-pricing.js` at prebuild — run
  `git checkout -- src/data/fastspring-pricing.json` before committing a website PR
  so the working-tree FX churn is not committed.
- **Render:** `src/pages/pricing.astro` maps over `SUPPORT_TIERS` — no price literals
  in the page; both cards read "Pricing to be announced". The page URL, `<title>` and
  description are an SEO red line: change body content only.
- **Downstream consumers** (`api/product.json.ts`, `offer-jsonld.ts`, and the
  generated `public/llms.txt` / `llms-full.txt` / `.well-known/ai-plugin.json`) all
  import from `product-facts.mjs`; a drift test enforces byte-equivalence. Change the
  fact in `product-facts.mjs`, then regenerate (`npm run gen:llms`), never the reverse.

The retired per-server and per-site price points are gone and must not resurface.

## Interactive Islands

These components need `client:load` or `client:visible` directives:

- **Pricing toggle:** Monthly/annual switch with price animation
- **Savings calculator:** Client-side bandwidth/cost calculation
- **Demo before/after:** Image/page comparison slider

All other content is static HTML — no client-side JavaScript.

## Commerce service

FastSpring webhook handling runs as a standalone service at `api.modpagespeed.com`.
See `tools/license-service/` for the implementation and `deploy/production/` for
deployment configuration.

The website has **no secrets** and no commerce API routes.

## Demo Metrics

The demo page (`src/pages/demo.astro`) imports optimization metrics from
`src/data/demo-metrics.json`. This file is generated by `tools/measure-demo/run.sh`
which runs the actual PageSpeed stack against the demo sites in
`public/demos/{ecommerce,blog,news,portfolio}/`.

Each demo site has external CSS (`style.css`), JS (`app.js`), and local JPEG images
(`images/`) — all served through the PageSpeed pipeline for real measurements.

To regenerate metrics: `cd .. && ./tools/measure-demo/run.sh`
