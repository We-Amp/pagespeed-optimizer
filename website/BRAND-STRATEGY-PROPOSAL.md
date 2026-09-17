> ⚠️ **SUPERSEDED — not the source of truth.** The canonical, governed brand
> guide is `brand/BRAND-STRATEGY.md` in the We-Amp/corp meta-repo (ADR-005).
> Reconcile all brand decisions against that file; it wins on any conflict. This
> document is an early (Feb 2025) proposal retained for history only.

# ModPageSpeed 2.0 — Visual & Copywriting Strategy Proposal

**Date:** February 2025
**Status:** Reviewed by expert panel (copywriting, design, developer marketing)
**Inspired by:** Ohmforce (ohmforce.com) — naming consistency, copywriting
confidence, visual polish

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Brand Voice & Copywriting Guide](#2-brand-voice--copywriting-guide)
3. [Naming Strategy: No Metaphor](#3-naming-strategy-no-metaphor)
4. [Visual Design Principles](#4-visual-design-principles)
5. [Content Strategy](#5-content-strategy)
6. [Developer Trust & Go-to-Market](#6-developer-trust--go-to-market)
7. [Application Guidelines](#7-application-guidelines)
8. [Implementation Roadmap](#8-implementation-roadmap)

---

## 1. Executive Summary

### The Opportunity

ModPageSpeed 2.0 has strong technical substance but lacks a cohesive brand
personality. The current website is professional and clean, but reads like a
generic SaaS product page. Compared to Ohmforce — which turned audio plugins
into a cult brand through consistent naming, confident copy, and distinctive
visual identity — there's room to develop a voice that's as distinctive as the
product itself.

### The Approach

**Inspired, not imitative.** We keep the light-mode, blue-accented professional
foundation but inject:

- **Clear, descriptive naming** that respects the audience's intelligence
- **A confident, opinionated voice** that reflects the founder's deep expertise
- **Visual polish** that proves the product's performance claims by example
- **Content patterns** that work across website, blog, newsletter, and docs
- **Developer trust signals** that let the product prove itself (benchmarks,
  source access, test rigor)

### Core Brand Tension (Our Sweet Spot)

ModPageSpeed lives at the intersection of two things that rarely coexist:

> **Deep technical authority** + **Accessible, human communication**

We're not a faceless CDN. We're not a VC-funded dashboard. We're a tool built by
someone who maintained the original, rebuilt it from scratch, and has opinions
about why most web performance tools get it wrong. That's the voice.

### Three Above-the-Fold Proof Points

Every first impression (landing page, conference slide, tweet) should lead with
these three claims:

1. **Sovereignty** — "Your servers, your rules. No proxy, no CDN, no third
   party."
2. **Up to 194 cache variants** — "One image in, up to 36 optimized image
   variants out. Format, viewport, density, and save-data matched to every
   visitor. Up to 194 possible cache keys across all content types."
3. **Zero-copy serving** — "Cache hits served via mmap. No copies, no
   allocations, no middleware."

The variant-aware caching architecture is the product's strongest technical
differentiator. It should be elevated to equal footing with the sovereignty
message, not buried as one feature among many.

---

## 2. Brand Voice & Copywriting Guide

### 2.1 Voice Attributes (Priority-Ordered)

When attributes conflict, higher-priority wins. Clarity before personality.

| Priority | Attribute         | What it means                                       | What it doesn't mean                 |
| -------- | ----------------- | --------------------------------------------------- | ------------------------------------ |
| 1        | **Direct**        | Short sentences. Active voice. No filler.           | Not aggressive or dismissive         |
| 2        | **Honest**        | We name trade-offs. We explain limitations.         | Not self-deprecating or apologetic   |
| 3        | **Authoritative** | We speak from deep experience. We've read the RFCs. | Not academic or condescending        |
| 4        | **Opinionated**   | We have a point of view and defend it.              | Not dogmatic or closed-minded        |
| 5        | **Dry wit**       | Occasional understated humor. Never forced.         | Not jokey, memey, or trying too hard |

**When in doubt:** Direct and Honest always win. If adding personality
compromises clarity, cut the personality.

### 2.2 Voice Examples

**Instead of this (generic SaaS):**

> "ModPageSpeed 2.0 is a next-generation web performance optimization solution
> that leverages cutting-edge technology to deliver blazing-fast page loads."

**Write this:**

> "ModPageSpeed 2.0 optimizes your pages on your servers. No proxy. No CDN.
> No external dependency that goes down at 3 AM and takes your site with it."

**Instead of this (over-technical):**

> "The capability bitmask encodes client characteristics into a 32-bit integer
> using bitwise operations across 7 dimensions for cache key composition."

**Write this:**

> "Every visitor gets a 32-bit fingerprint: their browser's format support,
> screen size, connection speed, and data-saving preferences. One number,
> 194 possible variants. The cache does the rest."

**Instead of this (marketing hype):**

> "Experience revolutionary performance gains with our AI-powered optimization
> engine!"

**Write this:**

> "Built on the libraries mod_pagespeed proved at scale. New architecture
> designed for nginx from the ground up."

**Additional examples across formats:**

| Format             | Bad                                            | Good                                                                                                                  |
| ------------------ | ---------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| **Headline**       | "Supercharge Your Web Performance"             | "Web optimization that stays on your servers"                                                                         |
| **Feature bullet** | "Powerful image optimization capabilities"     | "One decode pass, 36 variants: WebP, AVIF, and optimized originals"                                                   |
| **Error message**  | "Oops! Something went wrong!"                  | "Cache file not writable (mode 644). Run: chmod 666 /data/cache.vol"                                                  |
| **Tweet**          | "Excited to announce our latest release!"      | "2.0.3 ships viewport-aware resizing. Mobile visitors get 480px images, tablets 768px. Same JPEG, 3x less bandwidth." |
| **Email subject**  | "You Won't Believe These Optimization Results" | "Chrome lies about AVIF support 3% of the time"                                                                       |
| **Tooltip**        | "Click here to learn more about this feature"  | "Warmup threshold: re-notify after N fallback hits (default: 5)"                                                      |

**"Close but wrong" example:**

> "ModPageSpeed 2.0 is a battle-tested, production-ready web optimization
> platform that self-hosts on your infrastructure."

This _sounds_ right but fails: "battle-tested" and "production-ready" are
earned-adjective violations (Rule 6). "Platform" is vague. Better:

> "ModPageSpeed 2.0 runs on your server. 165 E2E tests, sanitizer coverage,
> and the same optimization algorithms that served billions of pages in
> mod_pagespeed."

### 2.3 Forbidden Sentence Patterns

These patterns signal generic SaaS copy. Never use them:

- "With our..." openers ("With our advanced optimization engine...")
- "Whether you're... or..." constructions
- "Introducing..." announcements
- "We're excited to..." openers
- "Unlock the power of..." anything
- Rhetorical questions as headlines ("Ready to speed up your site?")

### 2.4 Copywriting Rules

1. **Lead with the problem, not the feature.** "Your JPEG is 4x larger than
   it needs to be" beats "Advanced image transcoding support."

2. **Use concrete numbers.** "36 variants from one decode pass" is more
   compelling than "comprehensive format coverage."

3. **Name the alternative.** "Unlike CDN-based tools, your content never
   leaves your infrastructure" positions without attacking.

4. **One idea per sentence.** If a sentence has "and" or "which," consider
   splitting it. Target 12-15 words per sentence on landing pages, allow
   up to 25 in documentation.

5. **Active voice, present tense.** "The worker reads, optimizes, and writes"
   not "images are read, optimized, and written by the worker."

6. **Earn every adjective.** Don't say "powerful" — show what it does. Don't
   say "fast" — show the benchmark. If you can delete an adjective and the
   sentence still works, delete it.

7. **Address objections head-on.** The FAQ should feel like a conversation with
   a skeptical engineer, not a marketing FAQ. "Why should I pay when
   mod_pagespeed is free?" is a better question than "What are the benefits?"

8. **Technical precision is a feature.** Say "zero-copy mmap" not "efficient
   memory usage." The audience respects specificity.

### 2.5 Tone Spectrum by Context

| Context               | Tone                                  | Example                                                                         |
| --------------------- | ------------------------------------- | ------------------------------------------------------------------------------- |
| **Landing page**      | Confident, concise, benefit-driven    | "Your servers, your rules."                                                     |
| **Features page**     | Technical but scannable               | "32-bit capability mask → 194 cache variants"                                   |
| **Blog posts**        | Opinionated, narrative, educational   | "Why I rebuilt mod_pagespeed from scratch"                                      |
| **Documentation**     | Clear, precise, no personality needed | "Set `enable_mmap_directory = true`"                                            |
| **Newsletter**        | Conversational, insider-knowledge     | "This month I fixed a bug that's been in PSOL since 2014."                      |
| **Error messages**    | Helpful, functional                   | "Cache file not writable. Both nginx and the worker need write access."         |
| **Pricing page**      | Transparent, addresses doubt          | "One price. No per-request fees. No bandwidth metering."                        |
| **Social/community**  | Casual, responsive                    | "Good catch — filed as #247."                                                   |
| **Onboarding emails** | Warm, instructional                   | "Your license key is below. Here's the 3-minute setup."                         |
| **Changelog**         | Factual, brief, specific              | "Fixed: AVIF color space handling for wide-gamut displays (#312)"               |
| **404 page**          | Helpful, not cute                     | "Page not found. Try the docs or search."                                       |
| **Renewal emails**    | Transparent, value-recapping          | "Your trial ends Tuesday. Here's what PageSpeed optimized: [metrics]"           |
| **Welcome email**     | Warm, anticipatory, instructional     | "You're in. Here's the 3-minute setup — and what to expect in your first week." |
| **Log output**        | Terse, machine-parseable              | "[INFO] variant written: /img/hero.jpg mask=0xC9 format=webp 42KB"              |

### 2.6 Words We Use / Words We Avoid

| Use            | Avoid                    | Why                                  |
| -------------- | ------------------------ | ------------------------------------ |
| optimize       | supercharge, turbocharge | Precision over hype                  |
| self-hosted    | on-premise, on-prem      | Modern, clear                        |
| variant        | version, copy            | Technical term for cache entries     |
| original       | unoptimized, raw         | Neutral, not judgmental of input     |
| worker         | daemon, service, engine  | Consistent internal name             |
| interceptor    | module, plugin           | Distinguishes from old mod_pagespeed |
| sovereignty    | control, ownership       | Our differentiator term              |
| serves         | delivers, provides       | Active, specific                     |
| writes / reads | processes, handles       | Concrete actions                     |
| notification   | request, message, signal | IPC-specific term                    |

### 2.7 The "Conference Test"

Every piece of copy should pass this test: _Could you say this out loud at a
tech conference without feeling embarrassed?_

- "Blazing fast" → embarrassing
- "36 variants from one decode pass" → interesting
- "Zero-copy serving via mmap" → impressive to the right audience
- "Next-generation solution" → cringe

---

## 3. Naming Strategy: No Metaphor

### 3.1 The Decision

After exploring multiple metaphorical systems (forge/foundry, mint, distillery,
quench, and others), the decision is: **no metaphor.** Use plain, descriptive
names for everything. Let the voice, the origin story, and the numbers carry
the brand — not a naming gimmick.

### 3.2 Why No Metaphor

Ohmforce's "Ohm-" naming works because their audience is creative musicians
who value personality and identity in their tools. Our audience is DevOps
engineers and SREs who value clarity, precision, and substance. A product that
claims "no unnecessary bytes" shouldn't have unnecessary branding.

- Metaphors require maintenance. Every new feature needs a name that "fits."
- Metaphors create insider/outsider dynamics that work against onboarding.
- Metaphors that don't land perfectly are worse than no metaphor at all.
- The product's technical specifics ("36 variants," "zero-copy mmap") are
  already more memorable than any branded name.

### 3.3 The Naming System

| What              | Name                             | Notes                                                                                                                                                                         |
| ----------------- | -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Product           | **ModPageSpeed 2.0**             | Established. Heritage is in the name itself.                                                                                                                                  |
| Worker daemon     | **the worker**                   | Already used everywhere in docs and code.                                                                                                                                     |
| Nginx interceptor | **the interceptor**              | Already in the codebase. Descriptive.                                                                                                                                         |
| Cache             | **the cache** / **Cyclone**      | Cyclone is the library name — already distinctive.                                                                                                                            |
| Warmup system     | **warmup**                       | Plain.                                                                                                                                                                        |
| Image optimizer   | **the image optimizer**          | What it is.                                                                                                                                                                   |
| CSS minifier      | **the CSS minifier**             | What it is.                                                                                                                                                                   |
| HTML processor    | **critical CSS injection**       | What it does.                                                                                                                                                                 |
| CLI               | **mps** _(planned)_              | Three letters. `mps status`, `mps purge`, `mps stats`. Short to type, clear to read. Until built, quickstart uses `docker compose` and `socat` against the management socket. |
| Pro tier          | **PageSpeed Pro**                | Straightforward.                                                                                                                                                              |
| Newsletter        | No series name                   | Subject line earns the open, not a brand prefix.                                                                                                                              |
| Blog              | **Blog** or **Engineering Blog** | Tags for categorization: `#deep-dive`, `#benchmark`, `#release`, `#comparison`                                                                                                |
| Release notes     | **Changelog**                    | Industry standard.                                                                                                                                                            |

### 3.4 What Carries the Personality Instead

Without metaphorical naming, three things do the heavy lifting:

1. **The voice.** Direct, honest, opinionated copy is the brand identity. The
   naming system steps aside so the writing can be distinctive. (See Section 2.)

2. **The origin story.** "Built by a mod_pagespeed maintainer. Rebuilt from
   scratch." This is more memorable than any metaphor because it's true and
   nobody else can claim it.

3. **The numbers.** "36 variants, one decode pass." "194 possible cache keys."
   "Zero-copy mmap." These are the product's real names — technical specifics
   that become shorthand in the community.

### 3.5 Naming Rules

1. **Use terms consistently.** Always "the worker," never "the daemon" or "the
   service" or "the engine." Always "variant," never "version" or "copy."
   Always "interceptor," never "module" or "plugin." (See Section 2.6 for
   the full word list.)

2. **Nothing gets a branded name unless it earns one.** If the community
   starts calling the worker something organically, that's a name. Assigning
   names upfront is premature branding.

3. **The CLI is `mps`.** Three letters, Unix convention. Memorable enough
   to type daily, short enough to not resent.

4. **Trademark note:** All naming decisions remain contingent on the pending
   IP attorney review of the "PageSpeed" trademark.

---

## 4. Visual Design Principles

_Note: This section provides design direction and constraints. A detailed
design system (component specs, Figma library, Storybook) should be developed
as a separate artifact during implementation._

### 4.1 What We Learn from Ohmforce (Without Copying)

| Ohmforce principle                               | Our adaptation                                                         |
| ------------------------------------------------ | ---------------------------------------------------------------------- |
| Dark-only, monochromatic                         | Keep light mode. Add **strong contrast** and more confident whitespace |
| Single font family, weight-based hierarchy       | Adopt. One font, hierarchy through weight + size + opacity             |
| Pill-shaped CTAs as primary interactive language | Refine our button system with consistent radius and sizing             |
| Zero inter-section spacing, color-shift rhythm   | Create clearer section rhythm with alternating backgrounds             |
| Product screenshots float on clean background    | Hero images with clean, generous framing                               |
| Restrained animation (fade-in on scroll only)    | Keep minimal. Performance product = fast-loading site                  |

### 4.2 Color System (Evolution, Not Revolution)

**Current:** Blue (#0066cc / blue-600) + cool gray scale.
**Proposed:** Keep blue-600/700 range + warm neutrals + constrained amber accent.

```
Interactive Blue:  #1d4ed8 / blue-700  (buttons, links, active states)
Hover Blue:        #1e40af / blue-800  (hover states, full-bleed CTA sections)
Light Blue:        #dbeafe / blue-100  (backgrounds, badges)
Link Blue:         #2563eb / blue-600  (in-text links — must be distinguishable)

Warm Stone-50:     #fafaf9  (alternating section backgrounds)
Warm Stone-200:    #e7e5e4  (borders, dividers)
Warm Stone-900:    #1c1917  (body text — warmer than pure black)

Accent Amber:      #f59e0b  (badges and callouts ONLY — "new", "hot")
Success Green:     #059669  (confirmations, improvement metrics)

Code blocks:       #1e1e2e  (dark, high-contrast)
```

**Why warmer tones?** Pure gray (`gray-*`) feels sterile. Warm stone neutrals
feel crafted — which aligns with the "built by a maintainer" narrative. Linear,
Supabase, and Raycast all use warm-shifted neutrals in their light modes.

**Constraint: Amber is for badges and callouts only.** Don't use it in charts,
icons, or section backgrounds. A three-color system (blue + amber + green) gets
noisy on data-heavy pages.

**Dark mode readiness:** Define colors using CSS custom properties / Tailwind's
semantic tokens from the start (e.g., `--color-bg-primary`, `--color-text-body`).
Dark mode shipped in Phase 1 alongside light mode: BaseLayout exposes a
theme toggle that flips between `-dark` and light variants of every brand
asset, and component classes carry `dark:` Tailwind variants throughout.
Both modes are first-class; new components must declare `dark:` variants
from the start.
Every peer in the developer tool space
(Linear, Vercel, Supabase, Raycast) supports dark mode.

**Tailwind 4 implementation:** Custom values go in `global.css` using `@theme`
blocks (no `tailwind.config.js` in Tailwind 4).

**Accessibility:** Run all text/background combinations through WCAG AA checker.
Particular risk: `stone-500` text on `stone-50` at 14px may fail contrast.

### 4.3 Typography

**Recommendation:** Switch from system fonts to **Inter**, self-hosted.

**Why Inter (not DM Sans):**

- Designed for screens. Excellent OpenType features (tabular numbers for
  metrics, case-sensitive punctuation).
- Most readable sans-serif at body sizes for long technical content.
- 9 weights for nuanced hierarchy.
- Industry standard for developer tools (Linear, Supabase, Vercel/Geist).
- DM Sans (Ohmforce's choice) is geometric and optimized for display sizes,
  not for body text in technical documentation.

**Self-host in `public/fonts/` with WOFF2 format and `font-display: swap`.**
A site selling sovereignty should not phone home to Google for fonts.

Register in `global.css`:

```css
@theme {
  --font-sans: 'Inter', system-ui, sans-serif;
}
```

| Element         | Size (desktop)  | Size (mobile)  | Weight     | Tracking | Line-height |
| --------------- | --------------- | -------------- | ---------- | -------- | ----------- |
| H1 (hero)       | 56px / 3.5rem   | 40px / 2.5rem  | 700        | -0.025em | 1.1         |
| H2 (section)    | 32px / 2rem     | 28px / 1.75rem | 700        | -0.025em | 1.25        |
| H3 (subsection) | 24px / 1.5rem   | 20px / 1.25rem | 600        | 0        | 1.375       |
| Body            | 16px / 1rem     | 16px / 1rem    | 400        | 0        | 1.625       |
| Small / caption | 14px / 0.875rem | 14px           | 400        | 0.01em   | 1.5         |
| Code            | 14px / 0.875rem | 14px           | 400 (mono) | 0        | 1.625       |
| Nav links       | 14px            | 14px           | 500        | 0.02em   | 1.5         |
| Button labels   | 14px-16px       | 14px           | 600        | 0.02em   | 1           |

**Key principle from Ohmforce:** Create hierarchy through weight contrast
(400 body vs 700 headings) rather than font variety. One family, many weights.

### 4.4 Layout Principles

1. **Max width:** 1152px (keep current). Content doesn't need to be wider.

2. **Section rhythm:** Alternate between white and `stone-50` backgrounds.
   Every section should be visually distinct from its neighbors.

3. **Section spacing:** Standardize at `py-20` (80px) for top-level sections,
   `py-12` (48px) for nested sub-sections. Be consistent.

4. **Card design:** Subtle border (`border-stone-200`) + slight shadow on
   hover. No heavy drop shadows. `rounded-xl` for cards.

5. **Image treatment:** Product screenshots on clean, slightly off-white
   backgrounds. No device mockups unless showing actual browser rendering.

6. **Grid:** 3-column grids for feature descriptions on desktop (technical
   content needs room). 4-column grids acceptable for compact cards with
   short content (contact cards, stat cards). Stack to 1-column on mobile.

7. **Sticky header:** Keep it. Add `scroll-margin-top: 5rem` to heading
   elements for correct anchor link positioning.

8. **Hero text treatment:** Consider a CSS gradient on the hero accent phrase
   (`bg-gradient-to-r from-blue-600 to-blue-400 bg-clip-text
text-transparent`). This adds distinctiveness without JavaScript. Linear
   and Vercel both use this technique.

### 4.5 Component Design Language

**Buttons:**

```
Primary:    bg-blue-700 text-white rounded-md px-6 py-3 font-semibold
            hover:bg-blue-800 transition-colors
Secondary:  border border-blue-700 text-blue-700 rounded-md px-6 py-3
            hover:bg-blue-50 transition-colors
Ghost:      text-blue-700 hover:text-blue-800 underline-offset-4
            hover:underline
Disabled:   bg-stone-200 text-stone-400 rounded-md px-6 py-3 cursor-not-allowed
Icon-only:  p-2 rounded-md hover:bg-stone-100 transition-colors
```

Note: `rounded-md` (6px) rather than `rounded-lg` — keeps buttons tight and
professional. Matches Linear's approach.

**Cards:**

```
Feature:  bg-white border border-stone-200 rounded-xl p-6
          hover:shadow-md transition-shadow
Metric:   bg-stone-50 border border-stone-200 rounded-xl p-4
Code:     bg-gray-900 text-gray-100 rounded-lg p-4 font-mono text-sm
          overflow-x-auto [+ line numbers, copy button, language indicator]
          (Exception: code blocks use gray-*, not stone-*, because warm
          neutrals look odd against syntax-highlighted code.)
```

**Badges:**

```
Feature:    bg-blue-100 text-blue-800 text-sm font-medium px-3 py-1 rounded
Status:     bg-green-100 text-green-800 (same structure)
New:        bg-amber-100 text-amber-800 (same structure)
```

**Form inputs:**

```
border border-stone-300 rounded-md px-3 py-2 text-stone-900
focus:ring-2 focus:ring-blue-600 focus:border-blue-600
placeholder:text-stone-400
```

### 4.6 Visual Distinctiveness (Without a Metaphor)

Without metaphorical naming, the visual identity needs its own distinctive
elements. Three candidates:

1. **The bitmask motif.** The 32-bit capability mask is genuinely novel. A
   compact, color-coded bit strip (format | viewport | density | save-data |
   connection) can appear as a recurring design element — in the hero, as
   section dividers, and potentially in the favicon. It is unique to this
   product and instantly communicates technical depth. The features page
   already has a partial implementation (the bit breakdown table).

2. **Monospaced display numbers.** The product's key metrics ("36," "194,"
   "0xC8") gain visual punch when rendered in the monospace font at display
   scale. Use `font-mono` at H1/H2 sizes for hero statistics and proof
   points. This creates a typographic identity tied to the product's actual
   substance — not a decorative choice, but a signal that the numbers matter.

3. **The architecture icon.** The three-node flow (nginx → cache → worker)
   should become an instantly recognizable shape, like Docker's whale or
   Kubernetes' helm. Simple enough to work at favicon size, detailed enough
   to be informative on the features page.

At least one of these should be implemented in Phase 1. The bitmask motif is
the strongest candidate — it's unique, technical, and impossible for
competitors to copy.
→ Superseded by §4.10: the favicon ships as a gradient italic "S", not the
bitmask motif. The architecture-icon slot was not implemented in Phase 1 and
is now occupied by the terminal-prompt motif.

### 4.7 Illustrations & Diagrams

**Style:** Technical line-art diagrams, monochrome with blue accent. Similar
to Cloudflare's blog post diagrams. No illustrations of people, abstract art,
or isometric 3D.

**Architecture diagram (priority):** A single SVG showing the nginx → Cyclone
Cache → Factory Worker flow. This should be on the features page and shareable
as a standalone image. Engineers share diagrams on Twitter. This is the most
viral visual asset.

**Icons:** Switch from Heroicons to **Lucide** (`astro-icon` integration).
Lucide has a cleaner, more consistent stroke weight and is the de facto icon
set for modern developer tools (used by Linear, Shadcn).

### 4.8 Animation & Motion

**Rule: Less than Ohmforce, but not zero.**

- Page load: No animation. Content appears instantly. (We're a performance
  product.)
- Scroll: Subtle fade-in for below-fold sections (`opacity 0→1, translateY
8px→0, 300ms ease-out`). Optional, not critical.
- Hover: Color transitions only (`150ms ease`). No transforms.
- Modals/dropdowns: `200ms ease-out` slide/fade.
- **Never:** Parallax, particle effects, loading spinners on static pages,
  auto-playing videos, `transition-all` (causes layout shift risks).
- **Accessibility:** All optional animations must respect
  `prefers-reduced-motion`. Wrap in `@media (prefers-reduced-motion: reduce)`.

### 4.9 Responsive Principles

- **Mobile-first** — design the mobile layout first, then expand.
- **Breakpoints:** sm (640px), md (768px), lg (1024px). No xl breakpoint
  needed — max-width handles large screens.
- **Navigation:** Hamburger on mobile, horizontal links on desktop.
- **Touch targets:** Minimum 44x44px for all interactive elements.

### 4.10 Logo Guidelines

The mark is a deliberate **terminal-prompt composition**, not a typographic
wordmark. The brand's audience is sysadmins and platform engineers; the logo
signals what the product is (a CLI-shaped piece of infrastructure) rather than
how it's marketed. The gradient identity that would normally live on a
wordmark is carried by the favicon instead — see "Favicon" below.

**Composition.** The mark is a single SVG with four elements arranged on two
rows in a `viewBox="0 0 370 58"` canvas (intentionally wider/shorter than a
typical wordmark to accommodate the status line on the second row):

| Element        | Glyph / text                          | Font                                     | Notes                          |
| -------------- | ------------------------------------- | ---------------------------------------- | ------------------------------ |
| Prompt chevron | `❯` (U+276F)                          | JetBrains Mono, 700, 26px                | Sets the terminal-prompt motif |
| Wordmark       | `mod_pagespeed`                       | Inter, 700, 32px, `letter-spacing: -1.2` | Underscore is part of the mark |
| Cursor block   | 2.5 × 28 px `<rect>`, rx 1            | —                                        | Blinks via inline `<animate>`  |
| Status line    | `● v{version} · running · we-amp.com` | JetBrains Mono, 700/600, 14px            | Green dot + version + suffix   |

The 2.0 → 1.1 diff (within the same theme) is only the version string in the
status line. The light → dark diff (within the same product) is only the
colour palette — chevron, wordmark, cursor, version, and suffix all flip;
geometry, fonts, sizes and positions are unchanged.

**Variants:**

| File                | Product | Background | Chevron / cursor / version | Wordmark              | Suffix                |
| ------------------- | ------- | ---------- | -------------------------- | --------------------- | --------------------- |
| `logo.svg`          | 2.0     | Light      | `#1d4ed8` (blue-700)       | `#1c1917` (stone-900) | `#78716c` (stone-500) |
| `logo-dark.svg`     | 2.0     | Dark       | `#60a5fa` (blue-400)       | `#fafaf9` (stone-50)  | `#a8a29e` (stone-400) |
| `logo-1.1.svg`      | 1.1     | Light      | `#1d4ed8` (blue-700)       | `#1c1917` (stone-900) | `#78716c` (stone-500) |
| `logo-1.1-dark.svg` | 1.1     | Dark       | `#60a5fa` (blue-400)       | `#fafaf9` (stone-50)  | `#a8a29e` (stone-400) |

The green status dot is `#059669` (green-600) in **all four files** — it
reads correctly against both light and dark backgrounds and is the one
colour that does not flip with the theme.

**Naming convention.** The `-dark` suffix means **"for dark backgrounds"**
(i.e. the variant that contains light-coloured ink). This is the inverse of
the convention proposed in earlier drafts of this document.
`BaseLayout.astro` swaps the two with a `prefers-color-scheme` / theme
toggle:

```ts
const logoLight = is11Section ? '/logo-1.1.svg'      : '/logo.svg';      // used in light mode
const logoDark  = is11Section ? '/logo-1.1-dark.svg' : '/logo-dark.svg'; // used in dark mode
```

Do not rename. The convention is established across the codebase.
(`logo-light.svg` referenced in prior drafts of this section never existed on
disk; do not resurrect that filename.)

**Animation.** The cursor block uses a CSS `@keyframes` animation with a
1.2s opacity loop (`0.9 → 0 → 0.9`, `infinite`) defined in an inline
`<style>` block inside the SVG's `<defs>`. Under
`@media (prefers-reduced-motion: reduce)` the animation is set to `none`
and the cursor stays at a static `opacity: 0.9` — a visible, non-flickering
caret for motion-sensitive users. The single motion-control axis
(`prefers-reduced-motion`) governs both site animations and the inline-SVG
cursor; treat the reduced-motion fallback as a first-class feature of the
mark, not a known limitation. (The earlier `<animate>` SMIL implementation
was replaced with this CSS pattern; the `<style>` block is byte-identical
across all four logo variants — keep it in sync if it ever needs editing.)

**Sizing:**

| Context         | Height | Rendered width | Class |
| --------------- | ------ | -------------- | ----- |
| Site header     | 32px   | ~204px         | `h-8` |
| Site footer     | 28px   | ~179px         | `h-7` |
| Console topbar  | 20px   | ~128px         | —     |
| Minimum legible | 20px   | ~128px         | —     |

The 370×58 viewBox makes the rendered width roughly **6.4×** the height.
Plan header layouts around ~200px of mark width at 32px tall — the status
line is unreadable below 20px and should be cropped or hidden in tighter
slots.

**Clear space.** Reserve at least the height of the cursor block (~28 SVG
units, ≈ the cap-height of the wordmark) of empty space on all four sides.
The previous "width of 2.0" guideline no longer applies — there is no `2.0`
glyph in the mark.

**Favicon (`favicon.svg`).** The favicon is a separate visual: a heavy
italic "S" set on a rounded square. It is the only place in the brand
system that uses the blue gradient `#1d4ed8 → #60a5fa` (linear, top-left
to bottom-right). The background is white on light, stone-900 (`#1c1917`)
on dark, switched via an inline `prefers-color-scheme` CSS block.

**The favicon, not the wordmark, carries the brand gradient.** Whenever
abbreviated branding is needed (social avatars, app icons, sticker sheets,
single-glyph contexts), derive from the favicon — not from a "speed" glyph
or a recoloured chevron.

**When to use logo vs text-only branding:**

- Logo: site header, footer, OG images, presentation slides, external decks,
  README banners.
- Text-only `mod_pagespeed` (or `mod_pagespeed 2.0` / `mod_pagespeed 1.1`
  when version disambiguation matters): inline references in body copy,
  documentation prose, code comments, CLI output, error messages.
- Favicon glyph: avatars, app icons, anywhere a single-glyph mark is needed.

---

## 5. Content Strategy

### 5.1 Blog Strategy

**Cadence:** 2 posts per month (sustainable for solo/small team)

**Content pillars (revised based on what actually drives infrastructure
adoption):**

| Pillar                        | % of posts | Example titles                                       |
| ----------------------------- | ---------- | ---------------------------------------------------- |
| **Technical deep-dives**      | 35%        | "How the 32-bit capability mask works"               |
| **Benchmarks & measurements** | 25%        | "We measured image optimization across 10,000 pages" |
| **Release notes**             | 15%        | "What's new in 2.0.3: viewport-aware resizing"       |
| **Comparisons & migrations**  | 15%        | "When a CDN is enough (and when it isn't)"           |
| **Opinion/perspective**       | 10%        | "CDNs are a tax on your sovereignty"                 |

Note: Opinion/perspective cut from 25% to 10%. Data-driven posts get shared
more than opinion posts in the infrastructure space. Comparison/migration
content targets high-intent search queries ("ModPageSpeed vs imgproxy",
"migrate from mod_pagespeed").

**Blog voice rules:**

- Every post should have a thesis you can state in one sentence
- Open with a hook (problem, surprising fact, or contrarian take)
- Include at least one concrete example (code, benchmark, architecture diagram)
- End with a clear takeaway, not a sales pitch
- 800-2000 words. Shorter is fine if the point is made. Over 2000 words,
  split into a series or cut — long posts get bookmarked, not read.
- **Target specific search queries.** "Self-hosted image optimization for
  nginx" has real search volume. "How the 32-bit capability mask works" is
  interesting but niche. Balance thought leadership with discoverability.

**Blog organization:**

- No series branding. Post titles and tags do the work.
- Tags: `#deep-dive`, `#benchmark`, `#release`, `#comparison`, `#opinion`

### 5.2 Newsletter Strategy

**Cadence:** Monthly

**Format (consistent every issue):**

```
Subject: {one compelling line — no prefix branding}

1. THE UPDATE (2-3 sentences)
   What shipped this month. Link to release notes.

2. THE DEEP CUT (1 paragraph + link)
   One interesting technical detail from this month's work.
   "This month I learned that Chrome's Accept header lies about AVIF
   support in 3% of requests. Here's how we handle it."

3. THE READ (1 link)
   One article, talk, or tool worth your time.
   Not always ours — curating good content builds trust.

4. THE NUMBER (1 metric)
   One interesting number from production.
   "2.3 million variants generated this month across all installations."
```

**Newsletter voice:** Casual, insider. Like a monthly note from a colleague
who works on interesting infrastructure. Never corporate.

**Newsletter capture:** Signup should be on every page (footer at minimum,
inline on blog posts). Someone who reads a post but isn't ready to trial should
still be capturable.

### 5.3 Content Repurposing Pipeline

A solo founder cannot produce blog posts, newsletter issues, docs, social
posts, and community responses as independent workstreams. Content must flow
between channels:

```
Blog post
  → Becomes "The Deep Cut" in next month's newsletter
  → Breaks into 3-4 tweets/social posts
  → Answers a recurring GitHub Discussion question

Release note
  → Becomes blog post with technical context
  → Becomes newsletter "The Update" section

Community question (asked 3+ times)
  → Becomes a documentation page
  → Becomes a blog post with depth

Benchmark / measurement
  → Blog post
  → Landing page proof point
  → Social sharing image
```

This pipeline is critical for sustainability. Without it, the cadences break
within 3 months.

### 5.4 Guest Posting & Syndication

For a new product with no audience, publishing exclusively on your own blog is
speaking into a void. Plan contributions to:

- **Smashing Magazine** / **web.dev** — performance-focused audiences
- **r/nginx**, **r/webdev**, **r/sysadmin** — Reddit communities
- **Hacker News** — the "Why I rebuilt mod_pagespeed" post is HN front-page
  material
- **DevOpsDays / PerformanceNow** — conference talks (see Section 6)

### 5.5 Documentation Strategy

**Docs should be:**

- Scannable (use headers, code blocks, tables)
- Task-oriented ("How to configure viewport widths" not "Viewport Width API
  Reference")
- Layered (quick start → configuration → advanced → troubleshooting)
- Updated with every release (doc changes ship with code changes)

**Docs should NOT be:**

- Branded or personality-heavy (clarity > voice)
- Walls of text (max 3 paragraphs before a code example)
- Missing from the main navigation

**Priority doc:** `/docs/quickstart/` — "Run ModPageSpeed 2.0 in 60 seconds
with Docker Compose." This should be linked from the landing page, features
page, and pricing page. The Docker Compose infrastructure already exists.

### 5.6 Social & Community Communication

**Channels:** GitHub Issues/Discussions, Discord, Twitter/X

**Voice per channel:**

| Channel       | Tone                 | Example                                                                                            |
| ------------- | -------------------- | -------------------------------------------------------------------------------------------------- |
| GitHub Issues | Helpful, precise     | "Fixed in 2.0.4 — the issue was in the AVIF encoder's color space handling. Details in #312."      |
| Discord       | Casual, responsive   | "Good question — the warmup threshold default is 5 but you can tune it down for smaller sites."    |
| Twitter/X     | Opinionated, concise | "Every byte your CDN serves is a byte you don't control. Ship a 14KB critical CSS inline instead." |

---

## 6. Developer Trust & Go-to-Market

### 6.1 Trust Signals (Priority Order)

| Signal                     | What                                                                                                                                  | Why it matters                                                                                                                 |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| **Benchmark page**         | `/benchmarks/` — reproducible, open-methodology. Cache hit latency, variant throughput, memory footprint. Include the `wrk` commands. | Engineers buy on data, not vibes. Every infrastructure company that published open benchmarks saw it become a top-5 page.      |
| **Source access**          | Prominent "Read the source" link on the landing page. The code is BSL — anyone can read it.                                           | Tailscale's biggest trust builder was publishing their code. We already have this — surface it.                                |
| **Architecture diagram**   | SVG showing nginx → Cyclone Cache → Factory Worker flow. On features page and shareable standalone.                                   | Engineers share diagrams. They don't share prose.                                                                              |
| **"How we test" page**     | 165 Playwright E2E tests, 149 HTTP compliance tests, 64 licensing tests, ASan/UBSan/TSan suites.                                      | This rigor is unusual for a product this size. Show it. Gets posted on HN with "this is how you ship infrastructure software." |
| **Changelog**              | Living `/changelog/` page. Every release, what changed.                                                                               | Fly.io's changelog is one of their most-visited pages.                                                                         |
| **Case study: dogfooding** | "modpagespeed.com runs on ModPageSpeed 2.0. Here are the numbers."                                                                    | The demo measurement pipeline already generates this data. Turn it into a case study.                                          |

### 6.2 Competitive Positioning

The proposal does not exist in a vacuum. Engineers evaluate alternatives.

**Positioning matrix:**

|                                           | Self-hosted          | SaaS/CDN                     |
| ----------------------------------------- | -------------------- | ---------------------------- |
| **Full-stack** (images + CSS + JS + HTML) | **ModPageSpeed 2.0** | Cloudflare (free tier)       |
| **Images only**                           | imgproxy, Thumbor    | Cloudinary, imgix, Fastly IO |
| **Legacy**                                | mod_pagespeed 1.x    | —                            |

**Our position:** Self-hosted full-stack. Only product in that quadrant.

**Sovereignty angle, segmented by buyer motivation:**

| Buyer                 | Why they care about sovereignty      | Key message                                                         |
| --------------------- | ------------------------------------ | ------------------------------------------------------------------- |
| GDPR/compliance teams | Must control data processing         | "No third-party data processor. No DPA needed."                     |
| Latency-obsessed SREs | CDN adds a network hop               | "Cache hits in microseconds. No round-trip to someone else's edge." |
| Cost-sensitive ops    | CDN per-request pricing scales badly | "$49/month flat. No bandwidth metering."                            |
| Control-plane purists | Philosophical preference             | "Works offline. No vendor lock-in. Cancel and keep running."        |

**Comparison pages (high-intent SEO):**

- `/vs/cloudflare/` — sovereignty, variant depth, no free-tier lock-in
- `/vs/imgproxy/` — images-only vs full-stack optimization
- `/vs/diy-sharp/` — maintenance burden, variant management
- `/vs/mod-pagespeed/` — migration guide, what's new

### 6.3 Conversion Funnel

```
AWARENESS
  Blog post / HN / Reddit / Conference talk
    ↓
INTEREST
  Landing page → Features page → Benchmark page
    ↓
EVALUATION
  Quickstart docs (run locally in 60 seconds, free under BSL)
  Comparison pages ← high-intent search traffic enters here
    ↓
TRIAL
  Self-serve: enter email → receive key → docker compose up → optimized in 5 min
  (Frictionless. No "Request trial" → contact form. Paddle integration exists.)
    ↓
CONVERSION
  Renewal email with concrete metrics from the trial period
```

**Critical gap to fix:** The current "Request Your Free Trial" → `/contact/`
flow is a conversion killer. Engineers expect instant key generation.

#### 6.3.1 Trial Provisioning Specification

The trial flow must be self-serve with zero human gates:

```
1. Deploy ModPageSpeed via Docker Compose or module install
2. Open the web console at /console/
3. Purchase and activate license from the console (FastSpring checkout)
4. License key is set automatically — optimizations start immediately
5. 14-day free trial. Cancel anytime.
```

**Target:** Under 5 minutes from first `docker compose up` to optimized
responses. Purchase happens in the console, not on the marketing website.

### 6.4 Community Strategy

| Initiative              | Description                                                                                                                     | Priority |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------- | -------- |
| **Showcase page**       | `/showcase/` with early adopters. Even 3-5 logos create social proof.                                                           | P1       |
| **Discord channels**    | `#general`, `#help`, `#show-your-metrics` (user-generated optimization results)                                                 | P1       |
| **Contributor pathway** | Accept patches to `lib/` (Apache 2.0 code). Signal: "we accept contributions."                                                  | P2       |
| **Conference talks**    | "What I learned maintaining mod_pagespeed, and how it shaped 2.0." Submit to Nginx Summit, PerformanceNow, DevOpsDays. | P2       |
| **"Works with" guides** | "Speed up your Next.js / Rails / Django / WordPress site" → post to framework communities.                                      | P3       |

**Day-one social proof (no customer logos yet):**
At launch, traditional social proof (customer logos, testimonials) doesn't exist.
Substitute with signals that are equally persuasive to engineers:

- Dogfooding metrics: "modpagespeed.com is optimized by ModPageSpeed 2.0" with
  real Lighthouse scores and byte savings
- Test suite rigor: "165 E2E tests, 149 HTTP compliance tests, full sanitizer
  coverage" — this is unusually rigorous for a product this size
- Origin story: maintainer credibility is social proof in the infrastructure space
- BSL source access: "Read every line before you deploy it" — transparency as trust

### 6.5 Brand Narrative (Origin Story)

The "built by a mod_pagespeed maintainer" line is the most valuable brand asset.
It needs a canonical ~200-word narrative that can be referenced (not repeated
verbatim) across the landing page, about section, first blog post, and newsletter
welcome email:

> Otto van der Schaaf maintained mod_pagespeed — the open-source web optimization module
> that served billions of pages through Apache and Nginx. After years of
> working inside the codebase, he understood every layer of the system — and
> saw how a different architecture could serve the nginx-first, container-native
> world the web had moved to.
>
> ModPageSpeed 2.0 keeps the optimization libraries — the ones that served
> billions of pages — but replaces the architecture around them. Instead of
> RewriteDriver orchestrating 60+ filters in the request path, a lightweight
> worker reads originals from a shared cache, generates up to 36 variants per
> asset, and writes them back. Nginx serves cache hits via zero-copy mmap. No
> proxy, no CDN, no external dependency.
>
> The result is a tool that does what mod_pagespeed did — compress images,
> minify CSS and JS, inject critical CSS — but on modern infrastructure, with
> modern formats (WebP, AVIF), and designed for nginx from the ground up.

This narrative should evolve as the product matures and customer stories emerge.

---

## 7. Application Guidelines

### 7.1 Landing Page Copy Framework

```
HERO
  Headline: {What it does, in 8 words or fewer}
  Subhead:  {Why it matters, addressing the core pain}
  CTA:      {Action verb} + {what they get}

  Example:
  Headline: "Web optimization that stays on your servers."
  Subhead:  "ModPageSpeed 2.0 compresses images, minifies CSS and JS,
            injects critical CSS — all without sending your content
            through someone else's infrastructure."
  CTA:      "Start free trial" / "Read the docs"

PROOF POINTS (above the fold)
  Three claims that work together:
  "Self-hosted · 36 variants per image · Zero-copy cache hits"

HOW IT WORKS
  {3-step visual explanation with architecture diagram}
  1. Nginx intercepts the request and checks the cache
  2. On miss: serve the original, notify the worker
  3. Worker optimizes and writes 36 variants for future requests

FEATURES
  {6-8 features, each with icon + headline + one sentence}
  Lead with benefit, not capability name.
  Consider bento-grid layout: 2-3 hero features get larger cards.

SOCIAL PROOF
  {Testimonials, user count, or notable deployments}
  Dogfooding case study: "modpagespeed.com runs on ModPageSpeed 2.0"

OBJECTION HANDLING
  {FAQ that addresses real doubts}
  "Is this just mod_pagespeed repackaged?"
  "Why should I pay when the original is free?"
  "What happens if I cancel?"

CTA (REPEAT)
  {Same CTA as hero, different framing}
  "$49/month. No bandwidth fees. Cancel anytime."
```

### 7.2 Feature Description Pattern

Every feature should follow this structure:

```
[Icon]
HEADLINE: {Benefit statement, not feature name}
         "Images sized for every screen, encoded in every format"

BODY:     {What it does + how, in 2 sentences}
         "The worker decodes your image once and generates up to 36
         variants — WebP, AVIF, and optimized originals across three
         viewport sizes, two pixel densities, and save-data modes."

PROOF:    {One concrete metric or comparison}
         "A 10MP JPEG decodes to ~40MB. One decode, 36 outputs."
```

### 7.3 Blog Post Template

```markdown
# {Compelling headline — thesis in title form}

{Opening hook: 1-2 sentences. Problem, surprising fact, or contrarian take.}

{Context: Why this matters. 1 paragraph.}

## {Section 1: The problem/background}

{2-3 paragraphs with code examples or diagrams}

## {Section 2: The solution/approach}

{Technical details. Show, don't tell.}

## {Section 3: Results/implications}

{Benchmarks, before/after, or architectural insight}

---

{Closing: 1-2 sentences. Takeaway, not sales pitch.}
```

### 7.4 Error Message Style

```
PATTERN: {What happened} + {Why} + {What to do}

GOOD:
  "Cache file not writable (mode 644). Both nginx (nobody) and the
   worker (root) need write access. Run: chmod 666 /data/cache.vol"

BAD:
  "Error: Permission denied"
  "Oops! Something went wrong with the cache!"
  "The forge is cooling down" (never use metaphors in error states)
```

---

## 8. Implementation Roadmap

### Phase 1: Voice & Foundation (Week 1-2)

- [ ] Write canonical brand narrative (origin story)
- [ ] Self-host Inter font in `public/fonts/`
- [ ] Configure Tailwind 4 `@theme` with warm stone palette + semantic tokens
- [ ] Create reusable Astro components: `SectionHero`, `FeatureCard`,
      `CTASection`, `FAQItem`, `CodeBlock`, `Badge`
- [ ] Switch to Lucide icons
- [ ] Add newsletter signup to footer (all pages) and blog post layout
- [ ] Capture baseline metrics (current Lighthouse scores, trial signup rate,
      bounce rate) before redesign begins

### Phase 2: Landing & Core Pages (Week 3-4)

- [ ] Rewrite landing page using hero framework + three proof points
      (Note: current landing page violates several proposal rules — rhetorical
      question headline, missing proof points, generic SaaS voice. This is a
      full rewrite, not a tweak.)
- [ ] Create architecture diagram (SVG)
- [ ] Rewrite features page with bento-grid layout for top features
- [ ] Rewrite pricing page with segmented sovereignty messaging + FAQ
- [ ] Implement self-serve trial flow: Paddle Checkout overlay on pricing
      page, replacing the current `/contact/` link (see Section 6.3.1)
- [ ] Apply component library and warm stone palette across all pages
- [ ] Add OG image / social card template (1200x630px, blue-700 background,
      Inter Bold headline, architecture icon, `og-default.png` in `public/`)

### Phase 3: Trust & Content (Week 5-6)

- [ ] Create `/benchmarks/` page with reproducible methodology
      (Include full environment spec: hardware, nginx config, cache state,
      kernel version, `wrk` parameters, and number of runs averaged)
- [ ] Create `/changelog/` living page
- [ ] Create quickstart doc ("60 seconds with Docker Compose")
- [ ] Write launch blog post: "Why I rebuilt mod_pagespeed from scratch"
- [ ] Set up newsletter infrastructure with fixed 4-section format
- [ ] Keyword research for comparison and migration content (target queries:
      "self-hosted image optimization nginx", "mod_pagespeed alternative",
      "imgproxy vs", "nginx image optimization")
- [ ] Create dogfooding case study: "modpagespeed.com runs on ModPageSpeed 2.0"
      (Connect to demo measurement pipeline: `tools/measure-demo/run.sh`
      already generates the data)

### Phase 4: Go-to-Market (Week 7-8)

- [ ] Build first comparison page (`/vs/cloudflare/` or `/vs/mod-pagespeed/`)
- [ ] Set up Discord with channel structure
- [ ] Submit conference talk abstract (2-3 conferences)
- [ ] Typography, button, and responsive audit
- [ ] Performance audit (Lighthouse 100 target)
- [ ] Accessibility audit (contrast, keyboard, screen readers,
      `prefers-reduced-motion`)

### Success Metrics

Capture baselines for all metrics before Phase 2 begins.

| Metric                             | Target                      | Baseline             | Measurement    |
| ---------------------------------- | --------------------------- | -------------------- | -------------- |
| Lighthouse performance             | >= 98 all categories        | Capture pre-redesign | Every deploy   |
| Landing page → trial click-through | > 2%                        | Capture pre-redesign | Analytics      |
| Blog post organic impressions      | Growth MoM after 90 days    | 0 (new)              | Search Console |
| Newsletter subscribers             | 100 in first 90 days        | 0 (new)              | Email provider |
| Newsletter open rate               | > 40% (infrastructure norm) | N/A until first send | Email provider |
| Benchmark page monthly traffic     | Top-5 page by 90 days       | 0 (new)              | Analytics      |
| Trial → paid conversion            | > 5%                        | Capture after launch | Paddle         |

---

## Appendix A: Ohmforce Lessons Applied

| What Ohmforce does             | What we take                                   | What we leave                                                    |
| ------------------------------ | ---------------------------------------------- | ---------------------------------------------------------------- |
| "Ohm-" prefix everywhere       | Consistent terminology (Section 2.6 word list) | No metaphorical naming system — plain descriptions               |
| Celebrity testimonials         | Social proof (benchmarks, user stories)        | We use technical proof, not fame                                 |
| "The king is back" confidence  | Confident, opinionated voice                   | We stay grounded, not grandiose                                  |
| Dark monochromatic design      | Strong contrast, generous whitespace           | We keep light mode                                               |
| Single font family             | Inter, weight-based hierarchy                  | Same approach                                                    |
| Pill-shaped buttons            | Consistent button system                       | We use rounded-md, not full pill                                 |
| Food metaphors (Frohmager)     | Nothing — let the product speak for itself     | Metaphors add maintenance burden without value for this audience |
| Free legacy products as funnel | 14-day trial + transparent pricing             | We don't give away old versions                                  |
| Embedded audio demos           | Embedded visual demos (before/after)           | We show metrics, not audio                                       |

## Appendix B: Quick Reference Card

### Voice Checklist (Before Publishing Anything)

- [ ] Could I say this at a tech conference without cringing?
- [ ] Does every sentence earn its place? (Delete filler)
- [ ] Am I using concrete numbers instead of vague adjectives?
- [ ] Am I addressing the reader's real concern, not my feature list?
- [ ] Is the tone consistent with the spectrum for this context?
- [ ] Have I named the alternative (what they'd do without us)?
- [ ] Does it pass the "one idea per sentence" test?
- [ ] No forbidden sentence patterns? (Section 2.3)

### Naming Checklist (Before Naming Anything)

- [ ] Is the term from the approved word list (Section 2.6)?
- [ ] Is it used consistently everywhere it appears?
- [ ] Is it plain and descriptive? (No metaphors, no branded feature names)
- [ ] Does it match what the codebase already calls it?

### Design Checklist (Before Shipping Any Page)

- [ ] Correct color palette? (No ad-hoc colors)
- [ ] Typography hierarchy correct? (H1 > H2 > H3 > body)
- [ ] Buttons consistent? (Primary / Secondary / Ghost / Disabled)
- [ ] Alternating section backgrounds?
- [ ] Responsive? (Test at 375px, 768px, 1024px)
- [ ] Lighthouse >= 98 all categories?
- [ ] No animations that delay content visibility?
- [ ] `prefers-reduced-motion` respected?
- [ ] `scroll-margin-top` set for anchor links?

---

_This document is a living guide. Update it as the brand evolves. The goal is
consistency without rigidity — follow the principles, not just the rules._

**Governance:** Review quarterly. Changes via PR with rationale. Date-stamp
updates in the commit message (e.g., "brand-strategy: add social card specs,
Q1 2025"). If a rule is consistently ignored, either enforce it or remove it.
