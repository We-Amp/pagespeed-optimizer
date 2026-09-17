// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// content-accuracy.test.ts — ANTI-DRIFT guard for drift-prone product facts in
// human-facing site copy. Prevents the recurrence of the product-fact drift
// that mps2 PR #705 (2026-06-15 sitewide accuracy sweep) removed.
//
// WHAT DRIFTED (and what this guard forbids coming back)
//   PR #705 corrected a class of confidently-wrong product statements that had
//   spread across .md/.mdx content and .astro pages:
//     - a phantom 2.0 "v1.0.0" version label (2.0 GA is v2.0.0 / current
//       website manifest semver 2.0.21 — there is NO authoritative v1.0.0; the
//       roadmap is "2.x" / "a future 2.x release");
//     - ModPageSpeed 2.0 described as a loadable / standalone nginx module
//       installable from apt/yum (it is NOT — 2.0 BUNDLES nginx 1.30.2 in its
//       Docker reverse proxy and ships via Docker + Helm + NuGet;
//       the standalone 2.0 nginx module is DEFERRED);
//     - the 1.15 native nginx module's supported nginx range stated as
//       "nginx 1.26+" (it is per-distro, pinned to each distro's STOCK nginx:
//       Deb11/Ubu22.04=1.18.0 … Deb13=1.26.3 — a closed pinned matrix, not an
//       open ">=1.26" range);
//     - an Apache el9 arm64 overclaim ("AlmaLinux/RHEL 9 (amd64 + arm64)") —
//       the yum repo is x86_64 ONLY on el9; arm64 is direct-download;
//     - (guarded since the 2026-06-15 hardening) ModPageSpeed 2.0 stated to
//       require ".NET 9" — the middleware targets net8.0 + net10.0 (ASPNETCORE_TFMS).
//
// HOW THIS GUARD WORKS
//   (1) DENYLIST — a small set of TIGHTLY-SCOPED regexes, each targeting one
//       specific wrong CLAIM while deliberately NOT matching the legitimate
//       neighbouring text that lives in the clean tree today (e.g. "1.18 to
//       1.26", "el9 (x86_64; arm64 via direct download)", "before 2.0.14",
//       libpng/Envoy dependency versions like "1.0.0"). The scan surface is the
//       SIX BUCKETS defined at SCAN_BUCKETS below (content .md/.mdx, pages
//       .astro/.ts, data .ts/.mjs, layouts, llms .tmpl templates, and the
//       generated public/ agent surface); each file is scanned line-by-line and
//       a hit FAILS with file:line:matchedText.
//   (2) PER-RULE, CLAUSE-SCOPED EXEMPTIONS — exemption is OPT-IN per rule
//       (`negationExempt` for corrective-prose tolerance, `exemptIf` for a
//       rule-specific licensing term), and BOTH are tested against the CLAUSE
//       CONTAINING THE MATCH, never the whole line. Commit 39221716 inverted the
//       old global line-level negation default: a true statement in one clause
//       must not disarm a false claim in another ("Install 2.0 from apt. 1.15
//       packages are signed too." still flags). The denylist targets AFFIRMATIVE
//       restatements of the wrong fact, NOT the corrective copy that PR #705
//       itself wrote. This is precision-over-recall by design: the guard is a
//       REGRESSION NET (§Consequences), not an exhaustive semantic
//       checker. `exemptionMustNotDisarm` probes machine-enforce the scoping.
//   (3) CANONICAL-SOURCE consistency — assert the single-sources-of-truth
//       (product-facts.mjs + the release manifests) still hold the
//       ground-truth values, so the denylist's baseline never silently rots.
//   (4) CORPUS TESTS — a fixed corpus of (a) real corrective phrasings that MUST
//       NOT be flagged (the false positives an adversarial review found), and
//       (b) engine-tag refs ("v1.1.0+r11") that MUST NOT be flagged.
//   (5) MATCHER UNIT TESTS — each denylist regex is asserted to FLAG a known
//       wrong sample (and variants) AND to NOT flag a known-good sample, proving
//       the matcher discriminates without having to mutate real content.
//
// The baseline 2.0 version is DERIVED from the manifest at runtime (NOT
// hardcoded) — the manifest legitimately changes (it is 2.0.21 today).
//
// This test asserts copy/contract only; it never modifies any file.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { parse as parseYaml } from 'yaml';
import { describe, it, expect } from 'vitest';
import * as facts from './src/data/product-facts.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('.', import.meta.url));
const CONTENT_DIR = path.join(WEBSITE_ROOT, 'src/content');
const REL_2_0 = path.join(CONTENT_DIR, 'releases-2.0/release.yaml');
const REL_1_1 = path.join(CONTENT_DIR, 'releases-1.1/release.yaml');

// ---------------------------------------------------------------------------
// Canonical manifests (single sources of truth). Parsed once.
// ---------------------------------------------------------------------------
type ReleaseManifest = {
  release: { semver: string; revision: number | null; tag: string };
};
const manifest2_0 = parseYaml(readFileSync(REL_2_0, 'utf8')) as ReleaseManifest;
const manifest1_1 = parseYaml(readFileSync(REL_1_1, 'utf8')) as ReleaseManifest;

// Baseline 2.0 semver DERIVED at runtime — never hardcode (it changes).
const V2_SEMVER = manifest2_0.release.semver; // e.g. "2.0.21"

// ---------------------------------------------------------------------------
// File collection.
//
// SCAN SURFACE. The original guard scanned only src/content/**.{md,mdx} and
// src/pages/**.astro. That left published surfaces unguarded: the two API/feed
// routes are .ts, every product CLAIM originates in src/data/**, the shared
// layout emits meta/JSON-LD prose, and the llms.txt / ai-plugin.json family is
// generated from .tmpl sources and served verbatim to agents. All of those are
// PUBLISHED, so all of them are in scope.
//
// NO PRE-PROCESSING. Lines are scanned RAW, comments included. In src/data the
// comments are load-bearing fact records ("1.15 ships AVIF via four opt-in
// filters") — a claim that drifts in a comment drifts in the reader's head and
// then in the copy. Scanning them is intended, not an accident. If a real false
// positive ever appears, REWORD THE COMMENT — do not add a comment opt-out. One
// was tried (`notInComment`) and reverted: it cost the rule that set it 41.7% of
// product-facts.mjs (the canonical fact record) and 15.6% of src/data/**, to
// accommodate exactly ONE line. Rewording that one line costs nothing and keeps
// every rule's eyes open across the whole 57k-line scan surface.
// ---------------------------------------------------------------------------

// EXCLUSIONS — each with the reason it is NOT scanned. Matched against the
// path RELATIVE to WEBSITE_ROOT (posix separators).
const SCAN_EXCLUSIONS: Array<{ test: (rel: string) => boolean; why: string }> = [
  {
    // Archived Google-era documentation mirror. It is a HISTORICAL RECORD of
    // what the upstream project said, not a claim we are making today; the
    // guard's whole premise (this text asserts our current product facts) does
    // not hold for it, and "correcting" it would falsify the archive.
    test: (rel) => rel.startsWith('public/1.0/'),
    why: 'public/1.0/** — archived Google-era doc mirror; historical record, not our claim',
  },
  {
    // Large machine payloads (pricing snapshots, demo metrics, example
    // fixtures). No prose to drift, and megabytes of JSON per rule per run.
    test: (rel) => /^src\/data\/[^/]+\.json$/.test(rel),
    why: 'src/data/*.json — large machine payloads, no prose',
  },
  {
    // The guard's own fixtures are DELIBERATELY wrong (every rule's `bad`
    // sample lives here). Scanning them would make the guard flag itself.
    test: (rel) => rel.startsWith('test/'),
    why: "test/** — the guard's own known-bad fixtures would self-flag",
  },
  {
    // Presentational only. Components render claims that arrive as PROPS from
    // the pages and data modules already in scope, so the claim is caught at
    // its source rather than at each render site.
    test: (rel) => rel.startsWith('src/components/'),
    why: 'src/components/** — presentational; claims arrive as props from scanned pages',
  },
];

function isExcluded(abs: string): boolean {
  const rel = path.relative(WEBSITE_ROOT, abs).split(path.sep).join('/');
  return SCAN_EXCLUSIONS.some((x) => x.test(rel));
}

// `honorExclusions` is false ONLY for the canary fixtures, which live under
// test/** (excluded by design) but must still be walked by this exact function
// so the canaries actually exercise the real walker.
function walk(dir: string, exts: string[], honorExclusions = true): string[] {
  let out: string[] = [];
  let entries: string[];
  try {
    entries = readdirSync(dir);
  } catch {
    return out; // dir absent — nothing to scan
  }
  for (const name of entries) {
    const full = path.join(dir, name);
    if (honorExclusions && isExcluded(full)) continue;
    const st = statSync(full);
    if (st.isDirectory()) {
      out = out.concat(walk(full, exts, honorExclusions));
    } else if (exts.some((e) => name.endsWith(e))) {
      out.push(full);
    }
  }
  return out;
}

// Explicit files (not a directory walk): the generated agent-facing surface.
function existingFiles(rels: string[]): string[] {
  return rels
    .map((r) => path.join(WEBSITE_ROOT, r))
    .filter((f) => {
      try {
        return statSync(f).isFile();
      } catch {
        return false;
      }
    });
}

// SCAN BUCKETS + FLOORS. `walk()` returns [] on a missing directory, so a path
// typo or a moved tree would silently degrade this guard to green while
// scanning nothing. A PER-BUCKET floor makes that failure loud: one global
// "> 0" would stay satisfied by src/content alone even if every other bucket
// vanished. Floors are set below today's real counts — they catch a bucket
// COLLAPSING, they are not a ratchet on content volume.
//
// PER-EXTENSION floors alongside the bucket total. A bucket total is blind to
// losing one whole EXTENSION inside a mixed bucket: fault injection showed
// content dropping every .mdx (110 files, floor 100), pages dropping every .ts
// (62, floor 50) and data dropping every .mjs (11, floor 10) — all three stayed
// GREEN. That last one means losing product-facts.mjs, the single most
// load-bearing file in the scan set, was invisible. So each mixed bucket also
// asserts a minimum per extension.
type ScanBucket = {
  name: string;
  files: string[];
  floor: number;
  extFloors?: Record<string, number>;
};

const SCAN_BUCKETS: ScanBucket[] = [
  {
    name: 'content',
    files: walk(CONTENT_DIR, ['.md', '.mdx']),
    floor: 100,
    extFloors: { '.mdx': 10 },
  },
  {
    // .astro pages plus the two PUBLISHED .ts routes (api/product.json.ts,
    // blog/rss.xml.ts) — both emit agent- and reader-facing product prose.
    name: 'pages',
    files: walk(path.join(WEBSITE_ROOT, 'src/pages'), ['.astro', '.ts']),
    floor: 50,
    extFloors: { '.ts': 2 },
  },
  {
    name: 'data',
    files: walk(path.join(WEBSITE_ROOT, 'src/data'), ['.ts', '.mjs']),
    floor: 10,
    // >= 1 .mjs === product-facts.mjs is still being scanned.
    extFloors: { '.mjs': 1 },
  },
  { name: 'layouts', files: walk(path.join(WEBSITE_ROOT, 'src/layouts'), ['.astro']), floor: 1 },
  {
    name: 'templates',
    files: walk(path.join(WEBSITE_ROOT, 'scripts/llms-templates'), ['.tmpl']),
    floor: 3,
  },
  {
    name: 'public',
    files: existingFiles([
      'public/llms.txt',
      'public/llms-full.txt',
      'public/.well-known/ai-plugin.json',
    ]),
    floor: 3,
  },
];

const CONTENT_FILES = SCAN_BUCKETS.find((b) => b.name === 'content')!.files;
const SCAN_FILES = SCAN_BUCKETS.flatMap((b) => b.files);

// ---------------------------------------------------------------------------
// (2) NEGATION TOLERANCE IS OPT-IN, PER RULE.
//
//   This used to be the inverse: a global ~25-token NEGATION_CONTRAST list
//   disarmed ANY rule whenever ANY negation token appeared anywhere on the
//   line. That default was backwards, and the evidence was in the rules
//   themselves — every rule authored against REAL observed drift (f1, f2, f3)
//   had to set `hardFalse: true` to escape it, because the wrong claims those
//   rules target are themselves negation-shaped ("1.15 does not ship AVIF",
//   "AVIF is 2.0-only"). A blanket token list also disarms on an INCIDENTAL
//   negation elsewhere in the sentence, which is a silent hole by construction.
//
//   Now: negation exempts NOTHING by default. A rule that genuinely needs
//   corrective-prose tolerance sets `negationExempt`, and that regex carries the
//   SPECIFIC corrective phrasing for THAT claim — not a generic token list. Only
//   three rules need it (a, b2, b3); f1/f2/f3 need nothing, and c/d/e are
//   already cleared by their own regex directionality and `exemptIf`.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// (1) DENYLIST. Each rule is one tightly-scoped regex + human-readable why,
// plus a known-bad sample (must flag), a known-good sample (must not flag), and
// optional extra variants exercised by the matcher tests.
//
// SCOPING NOTES (false positives deliberately avoided — verified against the
// clean PR-#705 tree AND an adversarial false-positive corpus, see FP_CORPUS):
//   - Rule a does NOT bare-match "1.0.0": libpng 1.6.x, Envoy and other
//     dependency versions legitimately use x.y.z including "1.0.0". It only
//     matches phantom 2.0 version-label phrasings.
//   - Rule b3 is FORWARD-only ("ModPageSpeed 2.0" is the SUBJECT of the apt/yum
//     claim): the reverse direction ("1.15 ships apt and yum … 2.0 ships Docker")
//     is the common legitimate contrast and must not fire.
//   - Rule c keys on the OPEN-range expression ("1.26+", "1.26 or newer", ">=1.26"),
//     NOT "1.18 to 1.26" / "stock nginx 1.26.3".
//   - Rule d keys on the explicit "(amd64 + arm64)" overclaim attached to el9 /
//     RHEL 9 / AlmaLinux 9, OR an affirmative verb tying arm64 INTO the el9 yum
//     repo — NOT a correct arch-split description ("yum is x86_64 only; arm64 via
//     apt/direct download"), which the token order + the negation exemption clear.
// ---------------------------------------------------------------------------
type DenyRule = {
  id: string;
  why: string;
  re: RegExp;
  bad: string; // sample that MUST be flagged (re matches AND not corrective)
  good: string; // sample that MUST NOT be flagged
  // Per-rule extra exemption: a rule-specific term that LICENSES the claim.
  // Tested against the CLAUSE CONTAINING THE MATCH, not the whole line.
  exemptIf?: RegExp;
  // Opt IN to corrective-prose (negation/contrast) tolerance — §2. Set ONLY
  // when this rule's own targeted claim has a legitimate DENIAL form in real
  // copy, and make the regex carry that SPECIFIC phrasing. Never a generic
  // negation-token list: that is the backwards default this replaced. Also
  // CLAUSE-SCOPED — a denial in the neighbouring clause is about a different
  // claim and must not disarm this one.
  negationExempt?: RegExp;
  variantsBad?: string[]; // extra affirmative reintroductions that MUST be flagged
  variantsGood?: string[]; // extra legitimate phrasings that MUST NOT be flagged
  // ADVERSARIAL PROBES for the exemptions. Sentences that carry an exemption
  // term NATURALLY but are FALSE, so the exemption must not disarm the rule.
  // Each entry is asserted to be flagged. This is what turns "the exemption is
  // too broad" from a silent hole into a red test.
  exemptionMustNotDisarm?: string[];
};

const DENYLIST: DenyRule[] = [
  {
    id: 'a-phantom-2.0-v1.0.0-label',
    why: '2.0 GA is v2.0.0 (manifest semver derived at runtime); there is no authoritative "v1.0.0". Roadmap is "2.x" / "a future 2.x release".',
    // Requires the "v" in "post-v1.0": bare "post-1.0" is legitimate
    // stabilization-after-1.x copy ("the post-1.0 cleanup of the Envoy shim").
    re: /\bpost-v1\.0\b|\b2\.0 v1\.0\b|\bNot in v1\.0\.0\b|\bv1\.0\.0 (?:roadmap|release|of)\b|\bModPageSpeed 2\.0 1\.0(?:\.0)?\b/i,
    // Corrective form: copy that DENIES the phantom label exists. Anchored on
    // the denial sitting directly on "v1.0.0" ("there is no v1.0.0 release",
    // "not a v1.0.0"), never on a stray "no"/"not" elsewhere in the sentence.
    negationExempt: /\b(?:no|not an?)\s+v1\.0\.0\b/i,
    exemptionMustNotDisarm: [
      // The denial governs the SECOND clause (and a different product);
      // the phantom label in clause 1 is still false.
      'Planned for the post-v1.0 milestone. There is no v1.0.0 of Cyclone yet.',
    ],
    bad: 'Planned for the post-v1.0 milestone of ModPageSpeed 2.0.',
    good: 'Bundled libpng 1.6.58 and an Envoy 1.0.0 shim; before 2.0.14 the default differed.',
    variantsBad: ['Shipped in v1.0.0 of the 2.0 line.', 'Deferred to ModPageSpeed 2.0 1.0.0.'],
    variantsGood: [
      'Upgrade to 2.0.16+ and confirm; the Envoy 1.0.0 shim is internal.',
      'There is no v1.0.0 release of ModPageSpeed 2.0; GA was v2.0.0.', // corrective
      'The post-1.0 cleanup of the Envoy shim landed in 2.0.16.', // legit stabilization copy (no "v")
    ],
  },
  {
    id: 'b1-2.0-drop-in-or-loadable-nginx-module',
    why: '2.0 is NOT a loadable nginx module — it bundles nginx 1.30.2 in a Docker reverse proxy. The standalone 2.0 nginx module is deferred. (mod_pagespeed 1.15 IS a drop-in/loadable nginx module — so this rule requires a 2.0 subject; the bare phrase is correct for 1.15.)',
    re: /ModPageSpeed 2\.0[\s\S]{0,80}?(?:drop-in nginx (?:image)?optimization module|loadable nginx module|drop-in module for nginx|nginx module you (?:load|compile|install))|(?:drop-in nginx (?:image)?optimization module|loadable nginx module|drop-in module for nginx)[\s\S]{0,40}?ModPageSpeed 2\.0/i,
    bad: 'ModPageSpeed 2.0 is a drop-in nginx image optimization module you load_module.',
    good: 'ModPageSpeed 2.0 ships as a Docker reverse proxy that bundles nginx 1.30.2.',
    variantsBad: [
      'ModPageSpeed 2.0 is a loadable nginx module.',
      'ModPageSpeed 2.0 is a drop-in module for nginx.',
    ],
    variantsGood: [
      // 1.15 IS a drop-in nginx module — the bare phrase must not fire (no 2.0 subject):
      'A drop-in nginx image optimization module — install mod_pagespeed 1.15 from signed apt/yum.',
      'The drop-in nginx optimization module for the open-source crowd: mod_pagespeed 1.15.',
    ],
  },
  {
    id: 'b2-2.0-near-load_module-or-dynamic-module',
    why: '2.0 is distributed via Docker + Helm + NuGet, not loaded into an existing nginx via load_module.',
    re: /ModPageSpeed 2\.0[\s\S]{0,80}?(?:load_module|dynamic module on an? existing nginx|(?:dynamic|loadable)(?: nginx)? module)|(?:load_module|dynamic module on an? existing nginx)[\s\S]{0,80}?ModPageSpeed 2\.0/i,
    // Corrective forms for THIS claim, all anchored on the denied object
    // (load_module / dynamic module / nginx config) rather than a bare token:
    //   "2.0 does not use a load_module directive"
    //   "there is no load_module line for 2.0" / "has no load_module directive"
    //   "2.0 needs no nginx config"
    //   "2.0 is not installed as a dynamic module"
    // plus the Unlike/Whereas contrast where 1.15 owns the dynamic-module clause.
    negationExempt: new RegExp(
      [
        /\b(?:does not use|do(?:es)? not need|has no|have no|is no|there is no|needs? no|is not installed as|is not loaded as|not use)\b[^\n]{0,40}?(?:load_module|dynamic module|loadable module|nginx config)/
          .source,
        /\b(?:Unlike|Whereas)\b[^\n]{0,80}?\b1\.15\b/.source,
        /\b(?:Unlike|Whereas)\b[^\n]{0,80}?\bload_module\b/.source,
      ].join('|'),
      'i',
   ),
    exemptionMustNotDisarm: [
      // The corrective statement is about the Docker image, in clause 2; the
      // load_module claim in clause 1 is still false.
      'Add ModPageSpeed 2.0 with a load_module directive. The Docker image needs no nginx config.',
      // The Unlike/1.15 contrast governs clause 2 only.
      'Install ModPageSpeed 2.0 as a dynamic module on an existing nginx. Unlike 1.15, no restart is needed.',
    ],
    bad: 'Install ModPageSpeed 2.0 as a dynamic module on an existing nginx with load_module.',
    good: 'The mod_pagespeed 1.15 nginx module adds a load_module line; the package auto-includes it.',
    variantsBad: [
      'ModPageSpeed 2.0 plugs into your existing nginx as a dynamic module.',
      'Add ModPageSpeed 2.0 with a load_module directive.',
    ],
    variantsGood: [
      // Corrective / contrast — must be exempt by negation/contrast:
      'ModPageSpeed 2.0 does not use a load_module directive — it bundles nginx in a Docker reverse proxy.',
      "Unlike the 1.15 module's load_module line, ModPageSpeed 2.0 needs no nginx config.",
    ],
  },
  {
    id: 'b3-2.0-from-apt-or-yum-repo',
    why: '2.0 does NOT ship from apt/yum repositories (those carry the 1.15 native modules). 2.0 = Docker + Helm + NuGet.',
    // FORWARD-only: "ModPageSpeed 2.0" is the subject, tied to an apt/yum
    // repo/install/package within a short window. Single-channel (apt-only OR
    // yum-only) is the most likely reintroduction and is covered.
    re: /ModPageSpeed 2\.0\b[^\n]{0,50}?\b(?:apt|yum)\b[^\n]{0,25}?(?:repositor|repos?\b|install|package)/i,
    // A line co-mentioning 1.15 is a combined product-line description ("1.15 +
    // 2.0 — with a signed apt/yum repo"): the apt/yum repo belongs to the 1.15
    // line, not a 2.0 distribution claim. Don't fire on those.
    exemptIf: /\b1\.15\b/,
    exemptionMustNotDisarm: [
      // "1.15" is real here but governs the PREVIOUS clause (the thing being
      // migrated FROM). Clause scoping is what makes this flag.
      'Migrating from 1.15? Install ModPageSpeed 2.0 from the signed apt repository',
      // negationExempt probes: the denial/replacement sits in the OTHER clause.
      'Install ModPageSpeed 2.0 from the signed apt repository. Windows builds are not on apt.',
      'ModPageSpeed 2.0 is available in our yum repository; the Helm chart replaces apt for Kubernetes.',
      // Span-widening probes: the rule regex itself straddles the clause
      // boundary, so the scope must key on where the match STARTS.
      'Install ModPageSpeed 2.0 from apt. 1.15 packages are signed too.',
      // …and an opening parenthesis is a clause boundary too.
      'Install ModPageSpeed 2.0 from the apt repository (1.15 users: see below).',
      // Multi-match probe: match #1 lands in an exempt clause (1.15), match #2
      // does not. Evaluating only the first match would miss this.
      'ModPageSpeed 2.0 works with the 1.15 apt repo; install ModPageSpeed 2.0 from the yum repository.',
    ],
    // Corrective forms for THIS claim: copy that DENIES 2.0 ships from apt/yum,
    // or states 2.0 SUPERSEDES/REPLACES those packages (migration copy). Both
    // are anchored on an apt/yum object, so an unrelated negation cannot disarm.
    negationExempt: new RegExp(
      [
        /\b(?:supersedes?|replaces?|retires?)\b[^\n]{0,40}?\b(?:apt|yum)\b/.source,
        /\b(?:is not|are not|does not|do not|isn['’]t|doesn['’]t|never)\b[^\n]{0,40}?\b(?:apt|yum)\b/
          .source,
        /\b(?:not|rather than|instead of)\s+(?:from\s+)?(?:the\s+)?(?:signed\s+)?(?:apt|yum)\b/
          .source,
      ].join('|'),
      'i',
   ),
    bad: 'Install ModPageSpeed 2.0 from the signed apt and yum repositories.',
    good: 'mod_pagespeed 1.15 ships as signed apt and yum packages, and the from-scratch ModPageSpeed 2.0 rewrite goes GA.',
    variantsBad: [
      'Install ModPageSpeed 2.0 with apt-get install modpagespeed.',
      'ModPageSpeed 2.0 is available in our apt repository.',
      'Get ModPageSpeed 2.0 from the yum repository.',
      'ModPageSpeed 2.0 packages are in the apt and yum repos.',
    ],
    variantsGood: [
      // Reverse contrast (1.15 ships apt/yum; 2.0 ships Docker) — must NOT fire:
      'While the 1.15 module ships from apt and yum repositories, ModPageSpeed 2.0 ships via Docker, Helm, and NuGet.',
      // Combined product-line description — apt/yum repo is the 1.15 line's:
      // (The trailing clause used to read "; AVIF in 2.0." — corrected in the
      // 1.15-AVIF re-anchor: 1.15 ships AVIF too, and rule f1 rightly flags
      // that phrasing. The fixture, not the rule, was wrong.)
      'We-Amp maintains mod_pagespeed 1.15 + ModPageSpeed 2.0 — with security patches and a signed apt/yum repo; AVIF across 1.15 and 2.0.',
      // Migration copy — 2.0 replaces the legacy apt/yum path (exempt by "supersedes"):
      'ModPageSpeed 2.0 supersedes the apt and yum packages of the legacy engine.',
      // Corrective — exempt by negation:
      'ModPageSpeed 2.0 is not distributed via apt and yum; those carry only the 1.15 native module.',
    ],
  },
  {
    id: 'c-nginx-1.26-plus-as-module-range',
    why: 'The 1.15 native module is per-distro, pinned to each distro\'s STOCK nginx (1.18.0 … 1.26.3). It is NOT an open "nginx 1.26+" / "1.26 or newer" range.',
    re: /(?:mod_pagespeed 1\.15|1\.15 (?:nginx)?module|native module)[\s\S]{0,60}?nginx 1\.26(?:\+|\s*(?:or newer|or later|and later|and up|minimum))|nginx 1\.26(?:\+|\s*(?:or newer|or later|and later|and up|minimum))[\s\S]{0,60}?(?:mod_pagespeed 1\.15|1\.15 (?:nginx)?module|native module)|nginx >=?\s?1\.26\b/i,
    bad: 'The mod_pagespeed 1.15 native module supports nginx 1.26+ on every distro.',
    good: 'The mod_pagespeed 1.15 native module is pinned per distro, stock nginx 1.18 to 1.26.3.',
    variantsBad: [
      'The 1.15 module supports nginx 1.26 or newer.',
      'The native module requires nginx 1.26+ (stable).',
    ],
    variantsGood: [
      // Debian 13 trixie's stock nginx IS 1.26.3 — describing that per-distro build
      // as "nginx 1.26 (stable channel)" is accurate and closed, not an open range:
      'On Debian 13 the 1.15 native module is built for nginx 1.26 (stable channel).',
    ],
  },
  {
    id: 'd-apache-el9-arm64-overclaim',
    why: 'Apache yum repo on el9 (AlmaLinux/RHEL 9) is x86_64 ONLY; arm64 is via direct download. amd64+arm64 is the apt (Debian/Ubuntu) repo only.',
    // Branch 1: the explicit "(amd64 + arm64)" overclaim attached to el9 / RHEL 9
    // / AlmaLinux 9 (the exact PR-#705 finding + synonyms). Branch 2: an
    // affirmative verb tying arm64 INTO the el9 yum repo. Branch 3: arm64
    // in/from/via the el9 yum repo. The negation/contrast exemption + the tight
    // proximity clear correct arch-split descriptions.
    // Inter-token gaps use [^.;\n] so a clause boundary (`.` or `;`) stops a
    // match — "el9 yum carries x86_64; arm64 from apt" must not chain "carries"
    // (which governs x86_64) to the arm64 in the NEXT clause.
    re: /(?:AlmaLinux\/)?(?:RHEL ?9|el9|AlmaLinux ?9) ?\(amd64 ?\+ ?arm64\)|(?:el9|RHEL ?9|AlmaLinux ?9)\b[^.;\n]{0,40}?\byum\b[^.;\n]{0,25}?(?:include|includes|have|has|offers?|ships?|provides?|carr(?:y|ies)|supports?|gets?)\b[^.;\n]{0,15}?\barm64\b|\barm64\b[^.;\n]{0,20}?\b(?:in|from|via)\b[^.;\n]{0,20}?(?:el9|RHEL ?9|AlmaLinux ?9)\b[^.;\n]{0,20}?\byum\b/i,
    // Corrective form: copy stating arm64 is NOT in the el9 yum repo. Branch 3
    // ("arm64 in/from/via … el9 … yum") matches the shape of that denial, so it
    // needs the exemption. Anchored on the negation directly governing arm64's
    // presence — an affirmative overclaim carries no such denial.
    // (Beyond the a/b2/b3 set the task predicted: FP_CORPUS "Note: arm64 RPMs
    // are not in the RHEL 9 yum repo — fetch them from the release archive
    // instead." forced this.)
    negationExempt: /\barm64\b[^\n]{0,30}?\b(?:not|never)\b[^\n]{0,15}?\b(?:in|from|via)\b/i,
    exemptionMustNotDisarm: [
      // The denial is about the APT repo, in clause 2. The el9-yum overclaim in
      // clause 1 stands and must flag. (Doubles as a span-widening probe.)
      'The el9 yum repo also offers arm64 RPMs. arm64 is not in the apt repo.',
    ],
    bad: 'Apache ships from the yum repo for AlmaLinux/RHEL 9 (amd64 + arm64).',
    good: 'The signed yum repo (AlmaLinux/RHEL 9) ships x86_64; for arm64 / aarch64 RPMs, use the direct-download fallback below.',
    variantsBad: [
      'el9 (amd64 + arm64) RPMs are signed in the yum repo.',
      'The el9 yum repo also offers arm64 RPMs.',
    ],
    variantsGood: [
      // Correct arch-split descriptions — must NOT fire (token order / clause boundary / x86_64-only):
      'The yum repo serves x86_64 RPMs on RHEL 9; arm64 (aarch64) is available only through the apt channel on Debian/Ubuntu.',
      'On RHEL 9, the yum repo carries x86_64 packages; for arm64, build from source or use the apt-based images.',
      'On RHEL 9 the signed yum repo is x86_64 only, while the apt repo also offers arm64 builds for Debian and Ubuntu.',
      'The el9 yum repo carries x86_64; arm64 ships from the apt repo only.',
    ],
  },
  {
    id: 'e-2.0-dotnet-9-requirement',
    why: 'The ModPageSpeed 2.0 ASP.NET Core middleware targets net8.0 + net10.0 (ASPNETCORE_TFMS) — .NET 8 or .NET 10, NOT .NET 9. Only a REQUIREMENT claim is wrong; a "skipped .NET 9"/"newer than .NET 9" comparison is correct.',
    // Requires an UNAMBIGUOUS requirement-verb tying 2.0 to .NET 9
    // (targets/requires/needs/built for), or ".NET 9 or newer"/"9.0+". The bare
    // "on"/"runs on"/"supports"/"built on" verbs were dropped: they collide with
    // accurate copy ("2.0 runs on your servers", "supports .NET 9 apps as a
    // sidecar", "builds on .NET 9-era tooling") that does NOT claim 2.0's own
    // runtime is .NET 9. A skip/comparison ("skipped .NET 9", "newer than .NET 9")
    // carries no requirement verb, so the regex's own directionality clears it —
    // rule e needs no exemption of its own.
    re: /ModPageSpeed 2\.0[\s\S]{0,40}?(?:targets?|requires?|needs?|built for)\s+\.NET ?9\b|ModPageSpeed 2\.0[\s\S]{0,40}?\.NET ?9(?:\.0)?\s*(?:\+|or (?:newer|later))|ModPageSpeed 2\.0[\s\S]{0,40}?\b9\.0\+/i,
    bad: 'ModPageSpeed 2.0 targets .NET 9 and later.',
    good: 'ModPageSpeed 2.0 targets .NET 8 and .NET 10 (net8.0 and net10.0).',
    variantsBad: ['Run ModPageSpeed 2.0 on .NET 9 or newer.', 'ModPageSpeed 2.0 requires .NET 9.'],
    variantsGood: [
      // Corrective / comparison / coexistence — must NOT fire:
      'ModPageSpeed 2.0 does not support .NET 9; it targets net8.0 and net10.0.',
      'ModPageSpeed 2.0 skipped .NET 9: it targets the net8.0 and net10.0 LTS releases.',
      'ModPageSpeed 2.0 runs on net10.0, which is newer than .NET 9.',
      'ModPageSpeed 2.0 runs on .NET 9 hosts too, even though the package targets net8.0 and net10.0.',
      'ModPageSpeed 2.0 supports .NET 9 apps as a sidecar while the middleware itself targets net8.0/net10.0.',
      'ModPageSpeed 2.0 builds on .NET 9-era runtime improvements, shipping for net8.0 and net10.0.',
    ],
  },
  {
    id: 'f1-avif-exclusive-to-2.0',
    why: 'mod_pagespeed 1.15 SHIPS AVIF encoding (Apache, nginx, IIS native). AVIF is NOT exclusive to ModPageSpeed 2.0 — see IMAGE_FORMAT_SUPPORT in src/data/product-facts.mjs. Still 2.0-only: SVG auto-vectorization, Jpegli, ML-predicted quality, variant-aware caching.',
    // No negationExempt: the wrong claim is often itself negation-shaped ("this
    // package does not do AVIF — AVIF is 2.0 only"), so ANY negation tolerance
    // would neuter this rule. Exemptions live in exemptIf instead.
    // All branches are DIRECTIONAL — AVIF must be the SUBJECT being scoped to
    // 2.0. That keeps correct copy that mentions a real 2.0-only feature and
    // then AVIF ("…remain 2.0-only; AVIF now ships across 1.15 and 2.0") clear.
    re: new RegExp(
      [
        // "AVIF is ModPageSpeed 2.0 only" / "AVIF: 2.0-only"
        /\bAVIF\b[^\n]{0,60}?\b(?:ModPageSpeed)?2\.0[ -]only\b/.source,
        // "AVIF is available only in / only on / only from 2.0"
        /\bAVIF\b[^\n]{0,60}?\bonly\b[^\n]{0,20}?\b(?:in|on|from|with|via)\b[^\n]{0,20}?(?:ModPageSpeed |the)?2\.0\b/
          .source,
        // "Only ModPageSpeed 2.0 produces AVIF"
        /\bonly\b[^\n]{0,20}?(?:ModPageSpeed |the)?2\.0\b[^\n]{0,35}?\bAVIF\b/.source,
        // "AVIF is exclusive to 2.0"
        /\bAVIF\b[^\n]{0,40}?\bexclusive(?:ly)?\b[^\n]{0,25}?2\.0\b/.source,
        // "AVIF ships only here" (a 2.0-page "here")
        /\bAVIF\b[^\n]{0,30}?\bonly here\b/.source,
        // "AVIF requires / needs ModPageSpeed 2.0"
        /\bAVIF\b[^\n]{0,30}?\b(?:requires?|needs?)\b[^\n]{0,20}?(?:ModPageSpeed |the)?2\.0\b/
          .source,
        // The vs/* contrast cell: WebP scoped to both lines, AVIF fenced to 2.0
        // ("…(1.15 + 2.0); AVIF in 2.0", "…across 1.15 and 2.0, AVIF from the
        // 2.0 worker"). The connector must sit DIRECTLY after AVIF, so
        // "On 1.15 AVIF is opt-in; in 2.0 it is on by default" does not fire.
        /\b1\.15\b[^\n]{0,90}?\bAVIF\b[\s,—-]{0,3}\b(?:in|from)\b\s+(?:the |ModPageSpeed)?2\.0\b/
          .source,
      ].join('|'),
      'i',
   ),
    // A claim correctly scoped to the ARCHIVED open-source build (which
    // genuinely never had AVIF) is accurate and must survive.
    // NARROWED: bare \bGoogle\b is gone. The entire site is about Google
    // heritage, so that token appears in ordinary marketing prose and disarmed
    // f1 wherever it did — "Google never shipped AVIF, and AVIF remains
    // exclusive to ModPageSpeed 2.0" passed while being false. What actually
    // licenses the claim is the ARCHIVE marker (archived / 1.13.35.2 /
    // 1.14.36.1 / upstream), not the word Google. Note "original" is
    // deliberately NOT an archive marker: "Unlike Google's original, AVIF is
    // 2.0 only" is false and must flag.
    exemptIf: /1\.13\.35\.2|1\.14\.36\.1|\barchived\b|\bupstream\b/i,
    exemptionMustNotDisarm: [
      // "Google" appears naturally; the exclusivity claim is still false.
      'Google never shipped AVIF, and AVIF remains exclusive to ModPageSpeed 2.0',
      "Unlike Google's original, AVIF is ModPageSpeed 2.0 only",
    ],
    bad: 'AVIF is ModPageSpeed 2.0 only.',
    good: 'AVIF encoding is available across 1.15 and 2.0.',
    variantsBad: [
      'This package does not do AVIF — AVIF is ModPageSpeed 2.0 only.',
      'AVIF ships only here.',
      'AVIF is exclusive to ModPageSpeed 2.0.',
      'AVIF is available only in ModPageSpeed 2.0.',
      'AVIF requires ModPageSpeed 2.0.',
      'Only ModPageSpeed 2.0 produces AVIF.',
      'WebP + responsive variants at origin (1.15 + 2.0); AVIF in 2.0',
      'WebP conversion across 1.15 and 2.0, AVIF from the 2.0 worker.',
      'SVG auto-vectorization, Jpegli, and AVIF are 2.0-only.',
    ],
    variantsGood: [
      // Correctly scoped to Google's archived 1.13.35.2 build — still true:
      "Google's archived 1.13.35.2 build never had AVIF; the maintained 1.15 line does.",
      'AVIF was only in ModPageSpeed 2.0 back when the archived Google build was current.',
      // SVG auto-vectorization really is 2.0-only — must not fire:
      'SVG auto-vectorization is ModPageSpeed 2.0 only.',
      'SVG auto-vectorization, Jpegli, and ML-predicted quality remain 2.0-only; AVIF now ships across 1.15 and 2.0.',
      // The package-scoped "no AVIF" statements — still true (see f2):
      'This package does not do AVIF; use the IIS module on Windows.',
      // Correct edition scoping (what editionClause() renders):
      'WebP and AVIF across 1.15 and 2.0, SVG in 2.0.',
      'On 1.15 AVIF is opt-in; in 2.0 it is on by default.',
    ],
  },
  {
    id: 'f2-1.15-does-not-ship-avif',
    why: 'The inverse form: denying that mod_pagespeed 1.15 does AVIF. 1.15 ships AVIF encoding on Apache, nginx (standard and lite) and the native IIS module (opt-in via four filters outside rewrite_images/CoreFilters). The old "ROADMAP lists AVIF as Planned" claim is stale.',
    // No negationExempt: every phrasing of this claim IS a negation, so any
    // negation tolerance would zero the rule. The legitimate "X has no AVIF"
    // statements (Google's archived build, the NuGet package) are in exemptIf.
    re: new RegExp(
      [
        // "1.15 does not ship / cannot produce / will never encode AVIF"
        /\b1\.1(?:5)?\b[^\n]{0,60}?\b(?:does not|doesn['’]t|do not|don['’]t|cannot|can['’]t|won['’]t|will not|never)\b[^\n]{0,25}?\b(?:ship|ships|do|does|support|supports|have|has|produce|produces|emit|emits|generate|generates|include|includes|encode|encodes|output|outputs)\b[^\n]{0,20}?\bAVIF\b/
          .source,
        // "no AVIF in / for mod_pagespeed 1.15"
        /\bno AVIF\b[^\n]{0,30}?\b(?:in|on|for|from)\b[^\n]{0,25}?(?:mod_pagespeed)?1\.1(?:5)?\b/
          .source,
        // "1.15 lacks AVIF"
        /\b1\.1(?:5)?\b[^\n]{0,40}?\blacks?\b[^\n]{0,20}?\bAVIF\b/.source,
        // "AVIF is not available / not supported in 1.15"
        /\bAVIF\b[^\n]{0,40}?\bnot (?:available|supported|present|shipped|implemented)\b[^\n]{0,30}?1\.1(?:5)?\b/
          .source,
        // The stale ROADMAP claim: AVIF "planned" / "on the roadmap" for 1.15
        /\b1\.1(?:5)?\b[^\n]{0,60}?\bAVIF\b[^\n]{0,30}?\b(?:is |as)?(?:still)?(?:planned|on the roadmap)\b/
          .source,
      ].join('|'),
      'i',
   ),
    // Exempt the denials that remain TRUE: Google's archived open-source build,
    // and the WeAmp.PageSpeed.AspNetCore NuGet package (a 2.0 product whose
    // win-x64 build lacks libaom, so it does not inherit AVIF from 1.15).
    // NOT exempt any more: the ASP.NET Core Sidecar. It bundles nginx with the
    // pagespeed module, so it carries the same opt-in AVIF filters -- "the
    // Sidecar does not do AVIF" is now FALSE and must be caught. The old blanket
    // /packages?/ exemption is gone with it: it re-opened the same hole by
    // matching "the published sidecar packages ..".
    // NARROWED twice:
    //   - \bngx_pagespeed\b removed. It re-opened the exact hole the /packages?/
    //     removal closed: this rule's own `why` says 1.15 ships AVIF ON NGINX,
    //     and ngx_pagespeed IS that nginx module. "ngx_pagespeed 1.15 does not
    //     ship AVIF" passed while being false.
    //   - \bGoogle\b removed, same reasoning as f1: the archive markers license
    //     the denial, the word Google does not.
    // What remains is genuinely exempt: the archived open-source build, and the
    // WeAmp.PageSpeed.AspNetCore NuGet package (a 2.0 product whose win-x64
    // build lacks libaom, so it does not inherit AVIF from 1.15).
    exemptIf: /1\.13\.35\.2|1\.14\.36\.1|\barchived\b|\bupstream\b|\bAspNetCore\b|\bNuGet\b/i,
    exemptionMustNotDisarm: [
      // ngx_pagespeed IS the 1.15 nginx module, and it ships AVIF.
      'ngx_pagespeed 1.15 does not ship AVIF',
    ],
    bad: 'mod_pagespeed 1.15 does not ship AVIF.',
    good: 'mod_pagespeed 1.15 ships AVIF on Apache, nginx, and the native IIS module.',
    variantsBad: [
      'The 1.15 module cannot produce AVIF.',
      'There is no AVIF in mod_pagespeed 1.15.',
      '1.15 lacks AVIF support.',
      'AVIF is not supported in 1.15.',
      'For 1.15, AVIF is still planned.',
    ],
    variantsGood: [
      // Google's archived open-source build genuinely never had AVIF:
      "Google's archived 1.13.35.2 build does not ship AVIF.",
      'The archived open-source line never had an AVIF encoder; 1.15 added one.',
      // Package-scoped denial that is STILL true (2.0 product, win-x64 has no libaom):
      'The WeAmp.PageSpeed.AspNetCore NuGet package does not get AVIF from 1.15.',
      // Unrelated 2.0-only features:
      'mod_pagespeed 1.15 does not do SVG auto-vectorization; that is 2.0-only.',
      // Correct:
      'mod_pagespeed 1.15 ships AVIF, but the four AVIF filters are opt-in.',
      // The Sidecar bundles nginx + the pagespeed module, so it DOES do AVIF:
      'The ASP.NET Core sidecar transcodes AVIF through the same opt-in filters.',
    ],
  },
  {
    id: 'f3-zero-copy-and-variant-cache-are-not-2.0-only',
    why: "Zero-copy serving and the variant-aware Cyclone cache are NOT 2.0-only. mod_pagespeed 1.15 ships zero-copy serving on nginx, Apache and IIS as of v1.15.0+r19 (opt-in via CycloneZeroCopy / CycloneZeroCopyServe), and 1.15 already varies its cache on client capability -- image format, mobile UA, Save-Data and small-screen. What is genuinely 2.0-only is NARROWER: tablet/desktop viewport classes, pixel density (Sec-CH-DPR), transfer-encoding alternates, and PROACTIVE generation of the full variant matrix. Re-anchor exclusivity onto those, never onto 'variant-aware caching' or 'zero-copy' as whole categories. This class of error already shipped once: AVIF was replaced as the 2.0 differentiator by claims that were themselves false. NOTE: the widened scan surfaced ONE real false positive, a corrective comment in src/data/product-facts.mjs that told future authors 'NOT 2.0-only, never add them to a 2.0-only list: variant-aware caching'. A per-rule comment opt-out was tried and REVERTED — blinding f3 across every comment line in the canonical fact record to spare one line is a terrible trade. The comment was reworded instead ('Ships on BOTH lines, never fence to 2.0: ..'), which states the same facts without the exclusivity shape. Reword; never blind the rule.",
    // No negationExempt: exclusivity claims are negation-shaped ("ship ONLY
    // here", "1.15 does NOT have it"), so negation tolerance would neuter this
    // rule to zero hits against live false copy -- exactly as for f1/f2.
    re: new RegExp(
      [
        // "variant-aware caching and zero-copy serving ship only here / only in 2.0"
        /\b(?:variant-aware|zero-copy)\b[^\n]{0,80}?\b(?:ships?|available|found|exists?)\b[^\n]{0,20}?\bonly\b/
          .source,
        // "only 2.0 has variant-aware caching / zero-copy serving"
        /\bonly\b[^\n]{0,40}?\b2\.0\b[^\n]{0,60}?\b(?:variant-aware|zero-copy)\b/.source,
        // "variant-aware caching / zero-copy is 2.0-only"
        /\b(?:variant-aware|zero-copy)\b[^\n]{0,60}?\b(?:is |are)?2\.0[-\s]only\b/.source,
        // "1.15 does not have / lacks zero-copy or the variant-aware cache"
        /\b1\.1(?:5)?\b[^\n]{0,60}?\b(?:does not|doesn['’]t|cannot|can['’]t|lacks?|without)\b[^\n]{0,30}?\b(?:variant-aware|zero-copy)\b/
          .source,
      ].join('|'),
      'i',
   ),
    // 2.0 genuinely owns the NARROWER properties, so exclusivity phrased around
    // those is legitimate. Also exempt the ASP.NET Core middleware, which really
    // does copy through the managed response stream rather than serving zero-copy.
    // NARROWED: \bviewport\b, \bdensity\b and bare \bmiddleware\b are gone.
    // They are THE TOPIC'S OWN VOCABULARY — the sentences this rule exists to
    // catch are about viewport/density-scoped variant caching, so those tokens
    // sit in the false claims as readily as in the true ones and disarmed f3 on
    // ~13% of relevant lines. "Viewport-aware variants and zero-copy serving
    // ship only in 2.0" passed while being false. The genuinely-2.0-only
    // properties that DO license an exclusivity claim are the narrower
    // mechanisms below (Sec-CH-DPR, proactive generation, warmup,
    // transfer-encoding alternates), and none of the true phrasings this rule
    // was measured against relies on the dropped tokens.
    // \bmiddleware\b survives only in its subject-changing sense: the AspNetCore
    // middleware really does COPY through the managed response stream rather
    // than serve zero-copy, so a denial scoped to it is true.
    exemptIf: new RegExp(
      [
        /\bSec-CH-DPR\b/.source,
        /\bproactive(?:ly)?\b/.source,
        /\bwarmup\b/.source,
        /\btransfer[-\s]encoding\b/.source,
        /\bAspNetCore\b/.source,
        // "the middleware copies / copy path" — the subject really is the
        // copying middleware, not zero-copy serving as a category.
        /\bmiddleware\b[^\n]{0,40}?\bcop(?:y|ies|ying)\b/.source,
        /\bcop(?:y|ies|ying)\b[^\n]{0,40}?\bmiddleware\b/.source,
      ].join('|'),
      'i',
   ),
    exemptionMustNotDisarm: [
      // Carries "viewport"; still a false exclusivity claim (1.15 has zero-copy).
      'Viewport-aware variants and zero-copy serving ship only in 2.0',
      // Carries "middleware"; the 1.15 denial is still false.
      'mod_pagespeed 1.15 lacks zero-copy serving; the middleware adds it',
    ],
    bad: 'Variant-aware caching and zero-copy serving ship only here.',
    good: 'Viewport- and density-aware variants, generated proactively rather than on demand, ship only here.',
    variantsBad: [
      'Only ModPageSpeed 2.0 has variant-aware caching.',
      'Zero-copy serving is 2.0-only.',
      'mod_pagespeed 1.15 lacks zero-copy serving.',
      'The 1.15 module does not have a variant-aware cache.',
    ],
    variantsGood: [
      // TRUE: 1.15 has both.
      'mod_pagespeed 1.15 reads from Cyclone zero-copy; in 2.0 the same cache is shared with the worker.',
      'Zero-copy serving is opt-in on 1.15 and available on nginx, Apache, and IIS.',
      // TRUE: the narrower 2.0-only properties.
      'Proactive generation of the full variant matrix ships only in 2.0.',
      'Viewport class and pixel density enter the cache key only in 2.0.',
      // TRUE: the ASP.NET Core middleware really does copy.
      'The AspNetCore middleware copies through the managed response stream instead of serving zero-copy.',
    ],
  },
  {
    id: 'f4-implicit-exclusivity-by-scoping',
    why:
      "IMPLICIT exclusivity: a capability is fenced to 2.0 without the word 'only' or 'exclusive', so f1/f2/f3 all miss it. This is the shape the two motivating defects actually had. " +
      "(1) PARENTHETICAL FENCE — src/data/psi-mps-mapping.ts:250 and :649 read 'WebP (and AVIF on 2.0)'. The parenthetical scopes AVIF to 2.0 while WebP stays unscoped, so the reader concludes 1.15 has WebP but not AVIF. False: 1.15 ships AVIF too. Note the LINE never names 1.15 — the 1.1 scoping sits on neighbouring lines (availableIn, snippet11) — so a line-based rule must key on the fence itself, not on a 1.15 co-mention. " +
      '(2) ADJACENCY — src/data/cwv-pillars.ts:157 granted WebP+AVIF to 2.0 and then described 1.15 purely by WebP-producing filters (recompress_images + convert_jpeg_to_webp), granting it no AVIF. Same false implication, built entirely out of true-sounding parts. ' +
      'Target capabilities: AVIF, zero-copy serving, variant-aware caching.',
    re: (() => {
      // The capabilities that are NOT 2.0-exclusive and so must not be fenced.
      const CAP = /(?:AVIF|zero-copy(?:\s+serving)?|variant-aware(?:\s+caching)?)/.source;
      return new RegExp(
        [
          // (1) PARENTHETICAL FENCE: "WebP (and AVIF on 2.0)", "[plus AVIF in 2.0]".
          // The capability is bracketed off and tied to 2.0, which fences it away
          // from whatever the unbracketed subject is.
          //
          // TIGHTENED (an FP probe found 8 of 11 plausible ACCURATE sentences
          // firing here — routine release-note and blog shapes). Four guards:
          //   (i)  INCLUSION MARKER EXCLUDED (also/too/as well/both/alike).
          //        A bracket that is its own clause ("(AVIF is opt-in in 2.0
          //        as well)", "(variant-aware caching in 2.0 too)") is ordinary
          //        prose, and the inclusion word is precisely what says so: it
          //        asserts 2.0 ALSO has the capability, the opposite of fencing
          //        it there. An optional connector (and/plus/with/+) marks the
          //        appendage form but is NOT required — requiring it was tried
          //        and let a bare "WebP (AVIF on 2.0)" through, which is the
          //        motivating defect with one word removed.
          //   (ii) 2.0 must not be a VERSION PREFIX — "(AVIF in 2.0.21)",
          //        "(AVIF from 2.0.4 onward)", "(zero-copy on 2.0.14+)" are
          //        release refs, not fences. Same digit-flanked-period guard
          //        isClauseBoundary() uses.
          //   (iii) A MARKDOWN LINK "[AVIF in 2.0](/blog/…)" is linking, not
          //        fencing: a ']' followed by '(' does not close a fence.
          //   (iv) A bracket that CO-MENTIONS 1.15/1.1 is by definition not a
          //        fence, in EITHER order. Previously only the 1.15-first order
          //        was tolerated (by the {0,25} window), so "(zero-copy serving
          //        in 2.0 and 1.15)" fired while "(and AVIF on 1.15 and 2.0)"
          //        passed — pure ordering luck.
          `[([](?![^)\\]\\n]*\\b1\\.1(?:5)?\\b)(?![^)\\]\\n]*\\b(?:also|too|as well|both|alike)\\b)(?:(?:and|plus|with|\\+)\\s*)?${CAP}\\b[^)\\]\\n]{0,25}?\\b(?:on|in|from|with)\\b\\s*(?:ModPageSpeed\\s+)?2\\.0\\b(?!\\.\\d)[^)\\]\\n]{0,10}(?:\\)|\\](?!\\())`,
          // (2) ADJACENCY: 2.0 is granted the capability, then 1.15 is described
          // with a capability-granting verb and an image/format capability that
          // is NOT this one — and the capability never reappears for 1.15.
          `\\b2\\.0\\b[^\\n]{0,250}?\\b${CAP}\\b[^\\n]{0,500}?\\b(?:mod_pagespeed\\s+)?1\\.1(?:5)?\\b[^\\n]{0,60}?\\b(?:uses?|ships?|does|has|have|provides?|offers?|supports?|adds?|relies on)\\b(?=(?:(?!${CAP})[^\\n])*(?:WebP|convert_|recompress_|_images))(?:(?!${CAP})[^\\n])*$`,
        ].join('|'),
        'i',
     );
    })(),
    // No negationExempt: like f1/f2/f3, the claim shape is not a denial —
    // fencing is done affirmatively, so negation tolerance has nothing to add.
    bad: 'ModPageSpeed transcodes JPEG/PNG/GIF to WebP (and AVIF on 2.0), recompresses with quality-aware encoders.',
    good: 'ModPageSpeed transcodes JPEG/PNG/GIF to WebP and AVIF across 1.15 and 2.0.',
    variantsBad: [
      // (1) parenthetical fences, incl. the real :649 line:
      'JPEG/PNG/GIF transcoded to WebP (and AVIF on 2.0) when the client advertises support via `Accept`.',
      'Images are converted to WebP [plus AVIF in 2.0] on the fly.',
      'Serves responsive variants (with zero-copy serving on 2.0).',
      // (2) adjacency — capability granted to 2.0, 1.15 left with WebP filters:
      'ModPageSpeed 2.0 transcodes images to WebP and AVIF based on the client Accept header; mod_pagespeed 1.15 uses recompress_images + convert_jpeg_to_webp.',
    ],
    variantsGood: [
      // Both lines granted the capability — no fence:
      'WebP and AVIF across 1.15 and 2.0, SVG in 2.0.',
      'ModPageSpeed 2.0 transcodes to WebP and AVIF; mod_pagespeed 1.15 ships the same AVIF filters opt-in.',
      // A genuinely 2.0-only capability may be fenced — SVG/Jpegli are not targets:
      'Images are converted to WebP (and SVG on 2.0).',
      'Vector output (Jpegli in 2.0) is a 2.0 addition.',
      // AVIF fenced to BOTH lines is not a fence at all:
      'JPEG/PNG/GIF transcoded to WebP (and AVIF on 1.15 and 2.0).',
    ],
  },
];

// ---------------------------------------------------------------------------
// FALSE-POSITIVE CORPUS — real, ACCURATE corrective/contrast phrasings an
// adversarial review surfaced. None of these may be flagged by ANY rule.
// (Machine-enforces the negation/contrast exemption design — §2.)
// ---------------------------------------------------------------------------
const FP_CORPUS = [
  'On RHEL 9 the signed yum repo is x86_64 only, while the apt repo also offers arm64 builds for Debian and Ubuntu.',
  'The yum repo serves x86_64 RPMs on RHEL 9; arm64 (aarch64) is available only through the apt channel on Debian/Ubuntu.',
  'Whereas the apt repo covers amd64 and arm64, the RHEL 9 yum repo is x86_64 only.',
  'We do not publish arm64 packages to the RHEL 9 yum repo; only x86_64 is signed there.',
  'On RHEL 9, the yum repo carries x86_64 packages; for arm64, build from source or use the apt-based images.',
  'For RHEL 9 on arm64, install the package manually rather than from the yum repo.',
  'Note: arm64 RPMs are not in the RHEL 9 yum repo — fetch them from the release archive instead.',
  'Older docs wrongly listed the RHEL 9 yum repo as amd64 + arm64; it has always been x86_64 only.',
  "Unlike the 1.15 module's load_module line, ModPageSpeed 2.0 needs no nginx config.",
  'ModPageSpeed 2.0 does not use a load_module directive — it bundles nginx in a Docker reverse proxy.',
  'There is no load_module line for ModPageSpeed 2.0; it ships as a Docker reverse proxy.',
  'Whereas 1.15 runs as a dynamic module on an existing nginx, ModPageSpeed 2.0 bundles its own nginx.',
  'ModPageSpeed 2.0 is not installed as a dynamic module on an existing nginx; it ships its own.',
  'answer: ModPageSpeed 2.0 has no load_module directive because it bundles nginx 1.30.2 in a Docker reverse proxy.',
  'ModPageSpeed 2.0 is not distributed via apt and yum; those carry only the 1.15 native module.',
  'ModPageSpeed 2.0 does not install from apt and yum repos — use Docker, Helm, or NuGet.',
  'ModPageSpeed 2.0 ships via Docker, not from apt and yum repos.',
  'ModPageSpeed 2.0 ships through Docker rather than apt and yum.',
  'ModPageSpeed 2.0 differs from the 1.15 apt and yum module entirely.',
  'There is no v1.0.0 release of ModPageSpeed 2.0; GA was v2.0.0.',
  'While the 1.15 module ships from apt and yum repositories, ModPageSpeed 2.0 ships via Docker, Helm, and NuGet.',
  // Added by the 2026-06-15 FP-probe-v2 (all accurate; none may flag):
  'A drop-in nginx image optimization module — install mod_pagespeed 1.15 from signed apt/yum.', // 1.15 IS a drop-in module
  'The drop-in nginx optimization module for the open-source crowd: mod_pagespeed 1.15.',
  'ModPageSpeed 2.0 skipped .NET 9: it targets the net8.0 and net10.0 LTS releases.',
  'ModPageSpeed 2.0 runs on net10.0, which is newer than .NET 9.',
  'The el9 yum repo carries x86_64; arm64 ships from the apt repo only.',
  'On Debian 13 the 1.15 native module is built for nginx 1.26 (stable channel).', // trixie stock nginx IS 1.26.3
  'ModPageSpeed 2.0 supersedes the apt and yum packages of the legacy engine.',
  // Added by the 2026-06-15 FP-probe-v3 (accurate .NET-9 coexistence/era/"post-1.0"; none may flag):
  'ModPageSpeed 2.0 runs on .NET 9 hosts too, even though the package targets net8.0 and net10.0.',
  'ModPageSpeed 2.0 supports .NET 9 apps as a sidecar while the middleware itself targets net8.0/net10.0.',
  'ModPageSpeed 2.0 builds on .NET 9-era runtime improvements, shipping for net8.0 and net10.0.',
  'The post-1.0 cleanup of the Envoy shim landed in 2.0.16.',
  // Added by the f4 parenthetical-fence FP probe. All are ordinary, ACCURATE
  // release-note / blog / docs shapes that the untightened branch flagged:
  // version-prefixed release refs, markdown links, self-contained bracketed
  // clauses, and brackets that name BOTH product lines.
  'See the changelog entry (AVIF in 2.0.21) for the encoder bump.',
  'Read more about [AVIF in 2.0](/blog/avif-2/) and the 1.15 backport.',
  'Enable the four filters (AVIF is opt-in in 2.0 as well).',
  'Zero-copy serving is everywhere (zero-copy on 2.0.14+).',
  'Benchmarks below (AVIF on 2.0 vs 1.15).',
  'The cache is shared (variant-aware caching in 2.0 too).',
  'Docs: (zero-copy serving in 2.0 and 1.15).',
  'Full matrix (AVIF from 2.0.4 onward).',
];

// ENGINE-TAG CORPUS — correct v1.1.0+rN engine-tag refs. The guard
// must never flag these (the 1.1 engine tags are real; iis-configuration.md uses
// v1.1.0+r11 twice). Machine-enforces §5 rather than relying on it being only
// structurally true today.
const ENGINE_TAG_CORPUS = [
  'Available from v1.1.0+r11 onward.',
  'AutoCreateCachePath shipped in v1.1.0+r11 (PR #174).',
  'See the v1.1.0+r3 manifest and the v1.1.0+r20 engine tag.',
];

// ---------------------------------------------------------------------------
// HISTORICAL DEFECT CORPUS — the REAL pre-fix lines that motivated rule f4,
// copied VERBATIM from the versions before commit 771866c2 ("re-anchor AVIF
// claims for mod_pagespeed 1.15"):
//   src/data/psi-mps-mapping.ts:250  src/data/psi-mps-mapping.ts:649
//   src/data/cwv-pillars.ts:157
// Every entry MUST be flagged by f4. A rule that misses its own motivating case
// is worse than no rule, so this is asserted rather than assumed — and asserted
// against the verbatim text, not a paraphrase that could be tuned to pass.
// ---------------------------------------------------------------------------
const HISTORICAL_DEFECTS: Array<{ site: string; line: string }> = [
  {
    site: 'src/data/psi-mps-mapping.ts:250 (pre-771866c2)',
    line: "      'ModPageSpeed transcodes JPEG/PNG/GIF to WebP (and AVIF on 2.0), recompresses with quality-aware encoders, resizes to the rendered display size, and strips EXIF/ICC metadata. Typical result: 40–70% smaller images at visually equivalent quality.',",
  },
  {
    site: 'src/data/psi-mps-mapping.ts:649 (pre-771866c2)',
    line: "      'JPEG/PNG/GIF transcoded to WebP (and AVIF on 2.0) when the client advertises support via `Accept`.',",
  },
  {
    site: 'src/data/cwv-pillars.ts:157 (pre-771866c2)',
    line: "        body: 'This attacks resource load duration. ModPageSpeed 2.0 transcodes images to WebP and AVIF based on the client\\'s <code>Accept</code> header and serves <a href=\"/blog/viewport-aware-image-optimization/\">viewport-aware responsive variants</a> (mobile/tablet/desktop, 1x/2x), so the hero downloads sooner. A single decode produces up to 37 cache variants, and every variant is verified against the original with SSIMULACRA2 before it is cached. mod_pagespeed 1.15 uses <code>recompress_images</code> + <code>convert_jpeg_to_webp</code>. Optimization is transparent: no URL rewrites, no markup changes, no build-pipeline changes.',",
  },
];

// ALLOWLIST — legit historical version refs that must always survive the scan.
const ALLOWLIST_SAMPLES = [
  'On releases before 2.0.14, the worker did not coordinate.',
  "You're on 2.0.14 or newer — dotnet list package should show it.",
  'From 2.0.14 the default is an auto-resolved per-process socket.',
  'Upgrade to 2.0.16+ and confirm.',
  'Bundled libpng 1.6.58; the Envoy 1.0.0 shim is internal.',
  'native module pinned per distro: stock nginx 1.18 to 1.26.3.',
  'Apache yum repo: AlmaLinux/RHEL 9 (x86_64; arm64 via direct download).',
  'mod_pagespeed 1.15 ships as signed apt and yum packages.',
  'ModPageSpeed 2.0 ships as a Docker reverse proxy bundling nginx 1.30.2.',
];

// ---------------------------------------------------------------------------
// Flagging predicate: a line is flagged by a rule iff the regex matches AND the
// line is not corrective (negation/contrast). This is the single predicate used
// by BOTH the content scan and the matcher/corpus tests — no divergence.
// ---------------------------------------------------------------------------
// A COMMENT line, judged line-level (no parser) and LANGUAGE-AWARE. The
// language matters: in Markdown '#' opens a HEADING and '*' opens a BULLET —
// both are real published copy, and classifying them as comments would misread
// most of the prose in this scan surface.
//
// The .tmpl/.txt bucket is MARKDOWN-SHAPED, not shell-shaped: '#' there opens a
// HEADING too. Measured over public/llms.txt, public/llms-full.txt and
// scripts/llms-templates/*.tmpl, 114 of 114 '^#' lines are headings and ZERO
// are comments — llms-full.txt.tmpl even carries '## Variant-Aware Caching',
// which is rule f3's exact topic. So '#' is NOT a comment opener here.
//
// Nothing currently skips comment lines (the `notInComment` opt-out was tried
// and reverted — see the SCAN SURFACE note above). This classifier is kept, and
// unit-tested below, because getting it wrong is what made the opt-out look
// cheap: a rule that skips '#' lines in .tmpl/.txt goes blind on every heading.
const COMMENT_OPENER = {
  markdown: /^\s*<!--/, // ONLY the HTML comment form
  code: /^\s*(?:\/\/|\/\*|\*)/, // .ts / .mjs / .astro
  text: /^\s*\/\//, // .tmpl / .txt — '#' is a HEADING, not a comment
} as const;

function commentOpenerFor(file?: string): RegExp {
  if (!file) return COMMENT_OPENER.code; // bare-string unit tests
  const ext = path.extname(file).toLowerCase();
  if (ext === '.md' || ext === '.mdx') return COMMENT_OPENER.markdown;
  if (ext === '.ts' || ext === '.mjs' || ext === '.astro') return COMMENT_OPENER.code;
  return COMMENT_OPENER.text;
}

function isCommentLine(line: string, file?: string): boolean {
  return commentOpenerFor(file).test(line);
}

// ---------------------------------------------------------------------------
// CLAUSE SCOPING for BOTH exemptions (`exemptIf` and `negationExempt`).
//
//   Both used to be tested against the WHOLE LINE, with no requirement that the
//   exempting term govern the same clause as the match. That let an exempting
//   term anywhere in a sentence disarm a rule for a claim it has nothing to do
//   with:
//     "Migrating from 1.15? Install ModPageSpeed 2.0 from the signed apt
//      repository"  -> b3 disarmed by a "1.15" in the PREVIOUS sentence.
//     "Install ModPageSpeed 2.0 from the signed apt repository. Windows builds
//      are not on apt."  -> b3 disarmed by a TRUE denial in the NEXT sentence.
//   Now the exempting term must appear in the clause the match STARTS in. See
//   `exemptionMustNotDisarm` for the machine-enforced probes.
//
//   Boundary set: ; — ? ! (and a sentence period. The period is guarded
//   against splitting inside a version number: "ModPageSpeed 2.0", "1.15" and
//   "v1.0.0" must stay intact, so a '.' between two digits is NOT a boundary.
//   '?' and '!' are included beyond a bare [.;—] because a question mark ends a
//   clause just as firmly as a period. '(' opens an aside that is its own
//   clause — "Install ModPageSpeed 2.0 from the apt repository (1.15 users: see
//   below)" is a 2.0-from-apt claim regardless of who the aside addresses.
// ---------------------------------------------------------------------------
function isClauseBoundary(line: string, i: number): boolean {
  const ch = line[i];
  if (ch === ';' || ch === '—' || ch === '?' || ch === '!' || ch === '(') return true;
  if (ch !== '.') return false;
  // A '.' flanked by digits is a version separator, not a sentence end.
  return !(/\d/.test(line[i - 1] ?? '') && /\d/.test(line[i + 1] ?? ''));
}

function clauseRanges(line: string): Array<{ start: number; end: number }> {
  const ranges: Array<{ start: number; end: number }> = [];
  let start = 0;
  for (let i = 0; i < line.length; i++) {
    if (isClauseBoundary(line, i)) {
      ranges.push({ start, end: i });
      start = i + 1;
    }
  }
  ranges.push({ start, end: line.length });
  return ranges;
}

// The text of the SINGLE clause the match STARTS in.
//
// This deliberately does NOT union every clause the match overlaps. Several
// rule regexes use wide [\s\S]{0,80} / [^\n]{0,50} windows and so straddle a
// boundary; unioning re-imported the neighbouring clause's exempting tokens and
// re-opened the exact hole clause scoping exists to close:
//   "Install ModPageSpeed 2.0 from apt. 1.15 packages are signed too."
//     -> the b3 match runs past the period to reach "packages", dragging the
//        next sentence's "1.15" into scope and disarming itself.
// The claim's SUBJECT sits where the match begins, so that clause is the scope.
function clauseScopeOfMatch(line: string, start: number): string {
  const ranges = clauseRanges(line);
  const hit =
    ranges.find((r) => r.start <= start && start < r.end) ??
    // `start` landed exactly on a boundary character: attribute it to the
    // clause that boundary closes.
    [..ranges].reverse().find((r) => r.start <= start);
  return hit ? line.slice(hit.start, hit.end) : line;
}

// A per-rule global clone of the rule regex, so exec() can walk EVERY match on
// the line. Cached: recompiling per line per file is measurable at scan size.
const GLOBAL_RE = new Map<DenyRule, RegExp>();
function globalReFor(rule: DenyRule): RegExp {
  let re = GLOBAL_RE.get(rule);
  if (!re) {
    re = new RegExp(
      rule.re.source,
      rule.re.flags.includes('g') ? rule.re.flags : rule.re.flags + 'g',
   );
    GLOBAL_RE.set(rule, re);
  }
  return re;
}

// A line is flagged iff ANY match on it survives that match's own exemptions.
//
// Evaluating only the FIRST match was a silent miss: once match #1 landed in an
// exempt clause the rule stopped looking, so a second, unexempt claim on the
// same line was never tested —
//   "ModPageSpeed 2.0 works with the 1.15 apt repo; install ModPageSpeed 2.0
//    from the yum repository."
// (`file` is accepted for call-site symmetry with the scanner; no rule
// currently varies by file type.)
function isFlaggedByRule(rule: DenyRule, line: string, _file?: string): boolean {
  const re = globalReFor(rule);
  re.lastIndex = 0;
  let m: RegExpExecArray | null;
  while ((m = re.exec(line)) !== null) {
    if (m[0] === '') {
      re.lastIndex++; // zero-width match — never stall
      continue;
    }
    const scope = clauseScopeOfMatch(line, m.index);
    const exempt =
      (rule.negationExempt?.test(scope) ?? false) || (rule.exemptIf?.test(scope) ?? false);
    if (!exempt) return true;
  }
  return false;
}
function rulesFlagging(line: string): DenyRule[] {
  return DENYLIST.filter((r) => isFlaggedByRule(r, line));
}

type Violation = { ruleId: string; location: string; line: number };

function scanFilesForRule(rule: DenyRule, files: string[]): Violation[] {
  const violations: Violation[] = [];
  for (const file of files) {
    const text = readFileSync(file, 'utf8');
    const lines = text.split('\n');
    const rel = path.relative(WEBSITE_ROOT, file);
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const m = line.match(rule.re);
      if (m && isFlaggedByRule(rule, line, file)) {
        violations.push({ ruleId: rule.id, location: `${rel}:${i + 1}: ${m[0]}`, line: i + 1 });
      }
    }
  }
  return violations;
}

function scanForRule(rule: DenyRule): Violation[] {
  return scanFilesForRule(rule, SCAN_FILES);
}

// ---------------------------------------------------------------------------
// (3) CANONICAL-SOURCE consistency — the ground truth the denylist relies on.
// ---------------------------------------------------------------------------
describe('canonical sources hold ground-truth product facts', () => {
  it('product-facts.mjs SIDECAR_NGINX_VERSION === 1.30.2', () => {
    expect(facts.SIDECAR_NGINX_VERSION).toBe('1.30.2');
  });

  it('ASPNETCORE_RIDS includes linux-arm64 (2.0 is multi-arch, not x86_64-only)', () => {
    expect(facts.ASPNETCORE_RIDS).toContain('linux-arm64');
    for (const rid of ['linux-x64', 'linux-arm64', 'osx-arm64', 'win-x64']) {
      expect(facts.ASPNETCORE_RIDS).toContain(rid);
    }
  });

  it('ASPNETCORE_TFMS is net8.0 + net10.0 (the 2.0 middleware runtime; NOT .NET 9)', () => {
    expect(facts.ASPNETCORE_TFMS).toEqual(['net8.0', 'net10.0']);
    expect(facts.ASPNETCORE_TFMS).not.toContain('net9.0');
  });

  it('NGINX_APT_ARCHES is amd64 + arm64', () => {
    expect(facts.NGINX_APT_ARCHES).toBe('amd64 + arm64');
  });

  it('nginx distro matrix matches the known per-distro stock-nginx pins', () => {
    const matrix = Object.fromEntries(
      (facts.NGINX_APT_DISTROS as Array<{ distro: string; nginx: string }>).map((d) => [
        d.distro,
        d.nginx,
      ]),
   );
    expect(matrix).toEqual({
      'Debian 11 bullseye': '1.18.0',
      'Debian 12 bookworm': '1.22.1',
      'Debian 13 trixie': '1.26.3',
      'Ubuntu 22.04 jammy': '1.18.0',
      'Ubuntu 24.04 noble': '1.24.0',
    });
  });

  it('2.0 manifest semver is plain SemVer (no +rN), derived as the scan baseline', () => {
    expect(manifest2_0.release.revision).toBeNull();
    expect(V2_SEMVER).toMatch(/^2\.0\.\d+$/);
    expect(manifest2_0.release.tag).toBe(`v${V2_SEMVER}`);
    expect(manifest2_0.release.tag).not.toBe('v1.0.0');
  });

  it('IMAGE_FORMAT_SUPPORT claims no Envoy port for ANY format (AVIF included)', () => {
    // An absent port is a port we do NOT claim. The 1.15 Envoy port is not
    // shipped, is excluded from CI, and its AVIF status is UNVERIFIED — so
    // Envoy must appear in no port list. Do NOT fix a failure here by relaxing
    // the assertion; remove Envoy from product-facts.mjs instead.
    type Row = { format: string; editions: Record<string, { ports: string[] }> };
    const rows = facts.IMAGE_FORMAT_SUPPORT as Row[];
    const offenders: string[] = [];
    for (const row of rows) {
      for (const [edition, { ports }] of Object.entries(row.editions)) {
        for (const port of ports) {
          if (/envoy/i.test(port)) offenders.push(`${row.format}/${edition}: ${port}`);
        }
      }
    }
    expect(offenders, offenders.length ? `\n${offenders.join('\n')}\n` : undefined).toEqual([]);
    // Belt and braces: AVIF specifically, via the public accessor.
    for (const edition of facts.editionsFor('AVIF')) {
      expect(facts.portsFor('AVIF', edition)).not.toContain('Envoy');
    }
  });

  it('IMAGE_FORMATS stays the flat list ["WebP","AVIF","SVG"] (public contract)', () => {
    expect(facts.IMAGE_FORMATS).toEqual(['WebP', 'AVIF', 'SVG']);
    // Now derived from the converged line; the value must not move.
    expect(facts.formatsFor(facts.CURRENT_LINE)).toEqual(facts.IMAGE_FORMATS);
  });

  it('AVIF is claimed on every line; SVG only where the worker runs (2.0 + 2.1)', () => {
    expect(facts.editionsFor('AVIF')).toEqual([facts.V1_LINE, facts.V2_LINE, facts.CURRENT_LINE]);
    expect(facts.editionsFor('WebP')).toEqual([facts.V1_LINE, facts.V2_LINE, facts.CURRENT_LINE]);
    expect(facts.editionsFor('SVG')).toEqual([facts.V2_LINE, facts.CURRENT_LINE]);
    // WebP and AVIF share an edition set, so the copy clause collapses; derive
    // the expected clause from the table instead of hardcoding a rendering.
    const clause = facts.editionClause(['WebP', 'AVIF']);
    expect(clause.startsWith('WebP and AVIF across ')).toBe(true);
    for (const line of [facts.V1_LINE, facts.V2_LINE, facts.CURRENT_LINE]) {
      expect(clause).toContain(line);
    }
    // …and no rendered clause may leak a port (founder decision, mps2 #1000).
    for (const c of [facts.editionClause(), facts.editionClause(['AVIF'])]) {
      for (const port of ['Apache', 'IIS', 'ASP.NET Core', 'Envoy']) {
        expect(c).not.toContain(port);
      }
    }
  });

  it('1.1 manifest tag === v{semver}+r{revision} (catches a "1.1.0 base" regression)', () => {
    const { semver, revision, tag } = manifest1_1.release;
    expect(revision).not.toBeNull();
    expect(tag).toBe(`v${semver}+r${revision}`);
    // Derived, not hardcoded: the manifest's semver must sit on the V1 line.
    expect(semver.startsWith(`${facts.V1_LINE}.`)).toBe(true);
  });
});

// ---------------------------------------------------------------------------
// (5) MATCHER UNIT TESTS — prove every regex + the exemption discriminate bad
//     vs good (incl. variants) without touching real content.
// ---------------------------------------------------------------------------
describe('denylist matchers discriminate bad vs good samples', () => {
  for (const rule of DENYLIST) {
    it(`[${rule.id}] flags its known-bad sample`, () => {
      expect(isFlaggedByRule(rule, rule.bad)).toBe(true);
    });
    it(`[${rule.id}] does NOT flag its known-good sample`, () => {
      expect(rulesFlagging(rule.good)).toEqual([]);
    });
    if (rule.variantsBad?.length) {
      it(`[${rule.id}] flags all bad variants (synonym/word-order coverage)`, () => {
        const missed = rule.variantsBad!.filter((s) => !isFlaggedByRule(rule, s));
        expect(
          missed,
          missed.length ? `\nnot flagged:\n  ${missed.join('\n  ')}\n` : undefined,
       ).toEqual([]);
      });
    }
    if (rule.variantsGood?.length) {
      it(`[${rule.id}] does NOT flag any good variant`, () => {
        const wrong = rule.variantsGood!.filter((s) => rulesFlagging(s).length > 0);
        expect(
          wrong,
          wrong.length ? `\nwrongly flagged:\n  ${wrong.join('\n  ')}\n` : undefined,
       ).toEqual([]);
      });
    }
  }

  // ADVERSARIAL: an exemption must not disarm a rule on a sentence that carries
  // the exemption term naturally but is FALSE.
  for (const rule of DENYLIST) {
    if (!rule.exemptionMustNotDisarm?.length) continue;
    it(`[${rule.id}] exemptions do NOT disarm it on false copy that carries an exemption term`, () => {
      const disarmed = rule.exemptionMustNotDisarm!.filter((s) => !isFlaggedByRule(rule, s));
      expect(
        disarmed,
        disarmed.length
          ? `\nexemption wrongly disarmed ${rule.id} on:\n  ${disarmed.join('\n  ')}\n`
          : undefined,
     ).toEqual([]);
    });
  }

  // f4 must catch the two defects that motivated it, in their REAL wording.
  it('[f4] flags every historical pre-fix defect line verbatim', () => {
    const f4 = DENYLIST.find((r) => r.id.startsWith('f4-'))!;
    const missed = HISTORICAL_DEFECTS.filter((d) => !isFlaggedByRule(f4, d.line)).map(
      (d) => d.site,
   );
    expect(
      missed,
      missed.length
        ? `\nf4 MISSED its own motivating defects:\n  ${missed.join('\n  ')}\n`
        : undefined,
   ).toEqual([]);
  });

  // The comment classifier is LANGUAGE-AWARE; the .tmpl/.txt bucket is
  // markdown-shaped, so '#' there is a HEADING (114/114 measured), not a
  // comment. Getting this wrong is what made a comment opt-out look cheap.
  it('the comment classifier does not mistake Markdown headings for comments', () => {
    // '#' is a HEADING in every bucket that has one — never a comment.
    expect(
      isCommentLine('## Variant-Aware Caching', 'scripts/llms-templates/llms-full.txt.tmpl'),
   ).toBe(false);
    expect(isCommentLine('# ModPageSpeed', 'public/llms.txt')).toBe(false);
    expect(isCommentLine('## AVIF', 'src/content/blog/x.md')).toBe(false);
    expect(isCommentLine('- a bullet', 'src/content/blog/x.md')).toBe(false);
    // Real comment forms, per language.
    expect(isCommentLine('<!-- editorial note -->', 'src/content/blog/x.md')).toBe(true);
    expect(isCommentLine('// a note', 'src/data/product-facts.mjs')).toBe(true);
    expect(isCommentLine(' * jsdoc', 'src/data/psi-mps-mapping.ts')).toBe(true);
    expect(isCommentLine('// a note', 'public/llms.txt')).toBe(true);
    // Markdown's '//' is NOT a comment — it is ordinary text (and a URL tail).
    expect(isCommentLine('// not a comment in markdown', 'src/content/blog/x.md')).toBe(false);
  });

  it('no denylist rule fires on any allowlisted legit historical/dependency ref', () => {
    const wrongly = ALLOWLIST_SAMPLES.flatMap((s) => rulesFlagging(s).map((r) => `[${r.id}] ${s}`));
    expect(wrongly).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// (4) CORPUS TESTS — corrective copy and engine-tag refs must never be flagged.
// ---------------------------------------------------------------------------
describe('false-positive corpus: correct corrective/contrast copy is never flagged', () => {
  it('no rule flags any FP-corpus sample (negation/contrast exemption holds)', () => {
    const wrongly = FP_CORPUS.flatMap((s) => rulesFlagging(s).map((r) => `[${r.id}] ${s}`));
    expect(wrongly, wrongly.length ? `\n${wrongly.join('\n')}\n` : undefined).toEqual([]);
  });

  it('no rule flags a correct v1.1.0+rN engine-tag reference', () => {
    const wrongly = ENGINE_TAG_CORPUS.flatMap((s) => rulesFlagging(s).map((r) => `[${r.id}] ${s}`));
    expect(wrongly, wrongly.length ? `\n${wrongly.join('\n')}\n` : undefined).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// (1) THE GUARD — scan real content/pages; each rule must produce zero hits.
// ---------------------------------------------------------------------------
describe('content accuracy guard (anti-drift; PR #705 must not regress)', () => {
  // SCAN FLOORS — one per bucket. `walk()` swallows a missing directory and
  // returns [], so without these a renamed/moved tree turns the whole guard
  // green while scanning nothing.
  for (const bucket of SCAN_BUCKETS) {
    it(`scan floor: bucket "${bucket.name}" collects >= ${bucket.floor} files`, () => {
      expect(
        bucket.files.length,
        `bucket "${bucket.name}" collected ${bucket.files.length} files (floor ${bucket.floor}) — ` +
          'a path typo or moved tree silently degrades this guard to green',
     ).toBeGreaterThanOrEqual(bucket.floor);
    });

    // A bucket TOTAL is blind to losing one whole extension inside a mixed
    // bucket — see the SCAN_BUCKETS note. Assert each extension separately.
    for (const [ext, min] of Object.entries(bucket.extFloors ?? {})) {
      it(`scan floor: bucket "${bucket.name}" collects >= ${min} ${ext} files`, () => {
        const n = bucket.files.filter((f) => path.extname(f).toLowerCase() === ext).length;
        expect(
          n,
          `bucket "${bucket.name}" collected ${n} ${ext} files (floor ${min}) — the bucket ` +
            'total can stay green while an entire extension drops out of the scan',
       ).toBeGreaterThanOrEqual(min);
      });
    }
  }

  it('finds files to scan (extraction sanity)', () => {
    expect(SCAN_FILES.length).toBeGreaterThan(0);
    expect(CONTENT_FILES.length).toBeGreaterThan(0);
  });

  it('excluded paths are absent from the scan set', () => {
    const leaked = SCAN_FILES.filter((f) => isExcluded(f)).map((f) =>
      path.relative(WEBSITE_ROOT, f),
   );
    expect(leaked, leaked.length ? `\n${leaked.join('\n')}\n` : undefined).toEqual([]);
  });

  for (const rule of DENYLIST) {
    it(`[${rule.id}] no violations across the full scan surface`, () => {
      const violations = scanForRule(rule);
      expect(
        violations,
        violations.length
          ? `\n${rule.id} — ${rule.why}\n` +
              violations.map((v) => `  ${v.location}`).join('\n') +
              '\n'
          : undefined,
     ).toEqual([]);
    });
  }

  it('aggregate: zero denylist violations across all rules', () => {
    const all = DENYLIST.flatMap((r) => scanForRule(r));
    expect(
      all,
      all.length
        ? '\n' + all.map((v) => `  [${v.ruleId}] ${v.location}`).join('\n') + '\n'
        : undefined,
   ).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// (6) CANARY FIXTURES — prove the SCAN PATH itself still works.
//
//   The matcher unit tests above call isFlaggedByRule() on string literals, so
//   they bypass the scanner entirely: the walker, the extension filter and the
//   line splitter are all unexercised by them. If walk() silently stopped
//   returning files, if an extension dropped out of a bucket, or if the line
//   splitter broke, every one of those tests would still pass and the guard
//   would be green while scanning nothing.
//
//   The canaries close that gap. test/fixtures/canary/<rule-id>.md each hold
//   that rule's `bad` sample on a known line, and live under test/** so they
//   are OUTSIDE SCAN_FILES (they would otherwise self-flag the real guard).
//   They are then scanned with the SAME walk() and the SAME scan function, and
//   each rule must produce exactly one hit at exactly the expected line.
// ---------------------------------------------------------------------------
const CANARY_DIR = path.join(WEBSITE_ROOT, 'test/fixtures/canary');
const CANARY_BAD_LINE = 5; // '---', title, '---', blank, then the bad sample
const CANARY_FILES = walk(CANARY_DIR, ['.md'], /* honorExclusions */ false);

describe('canary fixtures: the scan path (walker, filter, splitter) still works', () => {
  it('the canary fixtures are NOT part of the real scan surface', () => {
    const leaked = SCAN_FILES.filter((f) => f.startsWith(CANARY_DIR));
    expect(leaked, leaked.length ? `\n${leaked.join('\n')}\n` : undefined).toEqual([]);
  });

  it('the walker collects one canary fixture per denylist rule', () => {
    expect(CANARY_FILES.length).toBe(DENYLIST.length);
    const missing = DENYLIST.map((r) => `${r.id}.md`).filter(
      (n) => !CANARY_FILES.some((f) => path.basename(f) === n),
   );
    expect(
      missing,
      missing.length ? `\nmissing fixtures:\n  ${missing.join('\n  ')}\n` : undefined,
   ).toEqual([]);
  });

  for (const rule of DENYLIST) {
    it(`[${rule.id}] the SCANNER finds its canary at line ${CANARY_BAD_LINE}`, () => {
      const fixture = CANARY_FILES.find((f) => path.basename(f) === `${rule.id}.md`);
      expect(fixture, `no canary fixture for ${rule.id}`).toBeTruthy();

      // The fixture must carry this rule's CURRENT bad sample verbatim, so the
      // fixture cannot drift away from the rule it is guarding.
      const lines = readFileSync(fixture!, 'utf8').split('\n');
      expect(lines[CANARY_BAD_LINE - 1]).toBe(rule.bad);

      // Scan it through the real scan function — not the string-literal path.
      const hits = scanFilesForRule(rule, [fixture!]);
      expect(
        hits.map((h) => h.line),
        `expected exactly one hit at line ${CANARY_BAD_LINE}, got:\n${hits
          .map((h) => `  ${h.location}`)
          .join('\n')}`,
     ).toEqual([CANARY_BAD_LINE]);
    });
  }
});
