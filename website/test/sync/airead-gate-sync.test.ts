// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// airead-gate-sync.test.ts — GOLDEN-CONTRACT sync-check for the demand-capture
// gate predicates inlined into the public RenderPeek page.
//
// CONTEXT
//   src/pages/ai-readability/index.astro carries a VERBATIM inline copy of eight
//   functions whose canonical source of truth is the scanner repo's
//   the scanner's demand gate (canonical copy maintained with the scanner). The page inlines them (rather
//   than importing) because in production it is the Astro-built
//   modpagespeed.com/ai-readability/ page and only /ai-readability/api/* is
//   proxied to the render service — there is no /src/ route to import from. The
//   page and scanner each carry a "KEEP IN SYNC" comment.
//
// WHY A GOLDEN CONTRACT (not an import-and-compare)
//   The canonical implementation lives in the scanner and is not vendored
//   here; this test pins the inline copy against golden fixtures. So instead of
//   comparing the two implementations directly, we:
//     (a) read index.astro and extract the eight inline function definitions
//         straight out of the <script is:inline> region (between the
//         KEEP-IN-SYNC comment and the normalizeUrl helper),
//     (b) evaluate them in an isolated VM sandbox,
//     (c) run them over a fixture set that exercises every branch, and
//     (d) assert the outputs equal a hardcoded GOLDEN expected-output set that
//         encodes the canonical contract documented in demand.mjs.
//   If the Astro copy drifts from the canonical contract (a branch flips, a
//   threshold moves, a field is dropped from lensSignal, the message format
//   changes), the extracted functions produce different output and this test
//   fails — which is exactly the drift this sync-check must catch.
//
// CANONICAL CONTRACT (mirror of the scanner's demand gate; canonical copy maintained with the scanner)
//   tollboothCtaApplies(agentVerifiability): true iff
//     detail.verifiable === false && detail.renderOk === true &&
//     Array.isArray(detail.aiCrawlersAllowed) && detail.aiCrawlersAllowed.length > 0 &&
//     detail.exposurePct >= 30
//   agentPassCtaApplies(signedAgentVerification): true iff
//     status === 'ok' && classification in {'verifying','signature-aware'}
//   complianceFixCtaApplies(accessibility): true iff
//     accessibility.available && accessibility.detail.fixableInline > 0
//   consentEnforcementCtaApplies(preConsentLeak): true iff
//     status === 'ok' && verdict === 'leaks' && trackerCount > 0
//   pageIntegrityCtaApplies(scriptInventory): true iff
//     status === 'ok' && verdict === 'attention'
//   thirdPartyFreezeCtaApplies(scriptInventory): true iff
//     status === 'ok' && verdict !== 'unknown' && totals &&
//     totals.thirdPartyExternal > totals.thirdPartyWithIntegrity
//   edgeSeoCtaApplies(seoDefects): true iff
//     status === 'ok' && verdict === 'attention' && defects is a non-empty array
//   buildLeadPayload({wedge,email,name,note,report}) -> { topic, email, name,
//     url, wedge, lensSignal, message }; lensSignal shape is lens-specific
//     (tollbooth / agentpass / compliancefix / consent-enforcement /
//     page-integrity + third-party-freeze, which share one script-inventory
//     signal: hosts only, never script URLs / edge-seo: ids, counts, booleans
//     and two fixed tokens, never page URLs or text), null for a
//     wedge with no scanner lens behind it (then no Signal line), and message
//     is the human-readable mirror.
//
// This test asserts behavior only; it does NOT modify the page.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import { describe, it, expect } from 'vitest';

const ASTRO_PAGE = fileURLToPath(
  new URL('../../src/pages/ai-readability/index.astro', import.meta.url),
);

// ---------------------------------------------------------------------------
// (a) Extract the seven inline gate functions from index.astro and evaluate
//     them in an isolated VM sandbox. We slice the source between the
//     KEEP-IN-SYNC anchor (start of the gate block) and the normalizeUrl
//     comment (first line after buildLeadPayload). If those anchors ever move,
//     this extraction throws loudly rather than silently testing nothing.
// ---------------------------------------------------------------------------
type Gate = {
  tollboothCtaApplies: (av: unknown) => boolean;
  agentPassCtaApplies: (sa: unknown) => boolean;
  complianceFixCtaApplies: (ax: unknown) => boolean;
  consentEnforcementCtaApplies: (pcl: unknown) => boolean;
  pageIntegrityCtaApplies: (si: unknown) => boolean;
  thirdPartyFreezeCtaApplies: (si: unknown) => boolean;
  edgeSeoCtaApplies: (sd: unknown) => boolean;
  responseFirewallCtaApplies?: (rx: unknown) => boolean;
  buildLeadPayload: (opts: unknown) => Record<string, unknown>;
};

function extractGateFunctions(): Gate {
  const src = readFileSync(ASTRO_PAGE, 'utf8');

  const startMarker = 'function tollboothCtaApplies(';
  const endMarker = '// Normalize + validate the URL';

  const startIdx = src.indexOf(startMarker);
  const endIdx = src.indexOf(endMarker);

  if (startIdx === -1) {
    throw new Error(
      `Could not find "${startMarker}" in index.astro. The inline gate block ` +
        `moved or was renamed — update this sync-check's extraction anchors.`,
    );
  }
  if (endIdx === -1 || endIdx <= startIdx) {
    throw new Error(
      `Could not find the "${endMarker}" end anchor after the gate block in ` +
        `index.astro — update this sync-check's extraction anchors.`,
    );
  }

  const block = src.slice(startIdx, endIdx).trim();

  // Sanity: all eight canonical functions must be present in the extracted slice.
  for (const fn of [
    'tollboothCtaApplies',
    'agentPassCtaApplies',
    'complianceFixCtaApplies',
    'consentEnforcementCtaApplies',
    'pageIntegrityCtaApplies',
    'thirdPartyFreezeCtaApplies',
    'edgeSeoCtaApplies',
    'buildLeadPayload',
  ]) {
    if (!block.includes(`function ${fn}(`)) {
      throw new Error(
        `Extracted gate block from index.astro is missing function "${fn}". ` +
          `The inline copy was restructured — review against demand.mjs.`,
      );
    }
  }

  const sandbox: Record<string, unknown> = {};
  const context = vm.createContext(sandbox);
  // Declaring the functions then exposing them via globals lets us pull the
  // function objects back out of the sandbox without trusting the page's own
  // call sites.
  const wrapped = `${block}\nglobalThis.__gate__ = { tollboothCtaApplies, agentPassCtaApplies, complianceFixCtaApplies, consentEnforcementCtaApplies, pageIntegrityCtaApplies, thirdPartyFreezeCtaApplies, edgeSeoCtaApplies, buildLeadPayload };`;
  vm.runInContext(wrapped, context, { filename: 'index.astro:inline-gate' });

  const gate = (sandbox as { __gate__?: Record<string, unknown> }).__gate__;
  if (!gate) {
    throw new Error('Extracted gate block did not expose the expected functions.');
  }
  return gate as Gate;
}

// The second source: the scanner's demand module as vendored for the v2
// interface. It carries all eight gates; the page's inline copy predates the
// response-exposure gate, so that predicate is checked on this copy only.
const DEMAND_MODULE = fileURLToPath(new URL('../../src/lib/scan/demand.mjs', import.meta.url));

function loadDemandModule(): Gate {
  const src = readFileSync(DEMAND_MODULE, 'utf8').replace(/^export /gm, '');
  const sandbox: Record<string, unknown> = {};
  const context = vm.createContext(sandbox);
  const names = [
    'tollboothCtaApplies',
    'agentPassCtaApplies',
    'complianceFixCtaApplies',
    'consentEnforcementCtaApplies',
    'pageIntegrityCtaApplies',
    'thirdPartyFreezeCtaApplies',
    'edgeSeoCtaApplies',
    'responseFirewallCtaApplies',
    'buildLeadPayload',
  ];
  for (const fn of names) {
    if (!src.includes(`function ${fn}(`)) {
      throw new Error(`demand.mjs is missing function "${fn}".`);
    }
  }
  vm.runInContext(`${src}\nglobalThis.__gate__ = { ${names.join(', ')} };`, context, {
    filename: 'demand.mjs',
  });
  return (sandbox as { __gate__: Gate }).__gate__;
}

const SOURCES: Array<[string, Gate]> = [
  ['inline copy in the v1 page', extractGateFunctions()],
  ['src/lib/scan/demand.mjs', loadDemandModule()],
];

// ---------------------------------------------------------------------------
// (b) Fixtures — cover every branch of each predicate and both lenses of the
//     payload builder. Each fixture is { name, input, expected }.
// ---------------------------------------------------------------------------

// --- tollboothCtaApplies: every guard branch ---
const tollboothFixtures: Array<{ name: string; input: unknown; expected: boolean }> = [
  { name: 'null agentVerifiability', input: null, expected: false },
  { name: 'undefined agentVerifiability', input: undefined, expected: false },
  { name: 'missing detail', input: {}, expected: false },
  {
    name: 'all conditions met (canonical positive)',
    input: {
      detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 30 },
    },
    expected: true,
  },
  {
    name: 'verifiable true blocks',
    input: {
      detail: { verifiable: true, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 80 },
    },
    expected: false,
  },
  {
    name: 'verifiable truthy-but-not-strict-false blocks (must be === false)',
    input: {
      detail: { verifiable: 0, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 80 },
    },
    expected: false,
  },
  {
    name: 'renderOk false blocks',
    input: {
      detail: {
        verifiable: false,
        renderOk: false,
        aiCrawlersAllowed: ['GPTBot'],
        exposurePct: 80,
      },
    },
    expected: false,
  },
  {
    name: 'renderOk truthy-but-not-strict-true blocks (must be === true)',
    input: {
      detail: { verifiable: false, renderOk: 1, aiCrawlersAllowed: ['GPTBot'], exposurePct: 80 },
    },
    expected: false,
  },
  {
    name: 'aiCrawlersAllowed not an array blocks',
    input: {
      detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: 'GPTBot', exposurePct: 80 },
    },
    expected: false,
  },
  {
    name: 'aiCrawlersAllowed empty array blocks',
    input: {
      detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: [], exposurePct: 80 },
    },
    expected: false,
  },
  {
    name: 'exposurePct at threshold 30 passes (>=)',
    input: {
      detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 30 },
    },
    expected: true,
  },
  {
    name: 'exposurePct just below threshold (29) blocks',
    input: {
      detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 29 },
    },
    expected: false,
  },
  {
    name: 'exposurePct well above threshold passes',
    input: {
      detail: {
        verifiable: false,
        renderOk: true,
        aiCrawlersAllowed: ['GPTBot', 'CCBot'],
        exposurePct: 95,
      },
    },
    expected: true,
  },
];

// --- agentPassCtaApplies: every guard branch ---
const agentPassFixtures: Array<{ name: string; input: unknown; expected: boolean }> = [
  { name: 'null signedAgentVerification', input: null, expected: false },
  { name: 'undefined signedAgentVerification', input: undefined, expected: false },
  { name: 'empty object', input: {}, expected: false },
  {
    name: 'verifying passes',
    input: { status: 'ok', classification: 'verifying' },
    expected: true,
  },
  {
    name: 'signature-aware passes',
    input: { status: 'ok', classification: 'signature-aware' },
    expected: true,
  },
  {
    name: 'no-signal blocks (absence of signal is not a finding)',
    input: { status: 'ok', classification: 'no-signal' },
    expected: false,
  },
  {
    name: 'errored lens blocks even with a classification',
    input: { status: 'error', classification: 'verifying' },
    expected: false,
  },
  {
    name: 'unknown classification blocks',
    input: { status: 'ok', classification: 'maybe' },
    expected: false,
  },
];

// --- complianceFixCtaApplies: every guard branch ---
const complianceFixtures: Array<{ name: string; input: unknown; expected: boolean }> = [
  { name: 'null accessibility', input: null, expected: false },
  { name: 'undefined accessibility', input: undefined, expected: false },
  {
    name: 'not available',
    input: { available: false, detail: { fixableInline: 5 } },
    expected: false,
  },
  { name: 'available but no detail', input: { available: true }, expected: false },
  {
    name: 'available, detail.fixableInline = 0 blocks',
    input: { available: true, detail: { fixableInline: 0 } },
    expected: false,
  },
  {
    name: 'available, detail.fixableInline > 0 passes',
    input: { available: true, detail: { fixableInline: 1 } },
    expected: true,
  },
  {
    name: 'available, detail.fixableInline large passes',
    input: { available: true, detail: { fixableInline: 42 } },
    expected: true,
  },
];

// --- consentEnforcementCtaApplies: every guard branch ---
const consentFixtures: Array<{ name: string; input: unknown; expected: boolean }> = [
  { name: 'null preConsentLeak', input: null, expected: false },
  { name: 'undefined preConsentLeak', input: undefined, expected: false },
  { name: 'empty object', input: {}, expected: false },
  {
    name: 'ok + leaks + trackerCount > 0 passes',
    input: { status: 'ok', verdict: 'leaks', trackerCount: 1 },
    expected: true,
  },
  {
    name: 'ok + leaks + many trackers passes',
    input: { status: 'ok', verdict: 'leaks', trackerCount: 12 },
    expected: true,
  },
  {
    name: 'clean blocks (no observed request is not a finding)',
    input: { status: 'ok', verdict: 'clean', trackerCount: 0 },
    expected: false,
  },
  {
    name: 'unknown blocks (render failed)',
    input: { status: 'ok', verdict: 'unknown', trackerCount: 0 },
    expected: false,
  },
  {
    name: 'disabled lens blocks',
    input: { status: 'disabled', verdict: 'unknown', trackerCount: 0 },
    expected: false,
  },
  {
    name: 'errored lens blocks even with a leaks verdict',
    input: { status: 'error', verdict: 'leaks', trackerCount: 3 },
    expected: false,
  },
  {
    name: 'blocked render (bot wall / challenge page) blocks',
    input: { status: 'blocked', verdict: 'unknown', trackerCount: 0, note: 'challenge page' },
    expected: false,
  },
  {
    name: 'leaks verdict with trackerCount 0 blocks (inconsistent count)',
    input: { status: 'ok', verdict: 'leaks', trackerCount: 0 },
    expected: false,
  },
];

// --- buildLeadPayload: all four lenses, a lens-free wedge, name/note variations ---
const tollboothReport = {
  url: 'https://example.com/',
  agentVerifiability: {
    detail: {
      verifiable: false,
      exposurePct: 87,
      aiCrawlersAllowed: ['GPTBot', 'CCBot'],
      aiCrawlersBlocked: ['ClaudeBot'],
      botAuthHeaders: false,
      renderOk: true,
    },
  },
};

const agentPassReport = {
  url: 'https://origin.example/',
  signedAgentVerification: {
    status: 'ok',
    classification: 'signature-aware',
    probes: {
      signed: { status: 403, headers: {}, bodyLength: 120 },
      broken: { status: 403, headers: {}, bodyLength: 120 },
      unsigned: { status: 200, headers: {}, bodyLength: 5120 },
    },
    evidence: ['broken-signature vs unsigned: status 403 vs 200'],
  },
};

const complianceReport = {
  url: 'https://shop.example.org/checkout',
  accessibility: {
    detail: {
      total: 12,
      fixableInline: 7,
      byImpact: { critical: 2, serious: 5 },
      topRules: ['image-alt', 'color-contrast'],
    },
  },
};

const consentReport = {
  url: 'https://shop.example.com/',
  preConsentLeak: {
    status: 'ok',
    verdict: 'leaks',
    trackerCount: 4,
    thirdPartyCount: 7,
    thirdPartyHosts: [
      { host: 'www.google-analytics.com', requests: 3, category: 'analytics', tracker: true },
      { host: 'www.googletagmanager.com', requests: 1, category: 'tag-manager', tracker: true },
      { host: 'connect.facebook.net', requests: 1, category: 'social', tracker: true },
      { host: 'static.hotjar.com', requests: 1, category: 'session-replay', tracker: true },
      { host: 'consent.cookiebot.com', requests: 1, category: 'cmp', tracker: false },
    ],
    cmpDetected: { detected: true, vendor: 'Cookiebot' },
    firstParty: ['shop.example.com'],
    requestsObserved: 22,
  },
};

// A leaks verdict whose host list is missing: topHosts falls back to [].
const consentReportNoHosts = {
  url: 'https://shop.example.com/',
  preConsentLeak: {
    status: 'ok',
    verdict: 'leaks',
    trackerCount: 2,
    thirdPartyCount: 2,
    cmpDetected: { detected: false, vendor: null },
  },
};

const buildLeadFixtures: Array<{
  name: string;
  input: unknown;
  expected: Record<string, unknown>;
}> = [
  {
    name: 'tollbooth lens with name + note',
    input: {
      wedge: 'tollbooth',
      email: 'ops@example.com',
      name: 'Pat Operator',
      note: 'GPTBot ~50k req/day',
      report: tollboothReport,
    },
    expected: {
      topic: 'tollbooth',
      email: 'ops@example.com',
      name: 'Pat Operator',
      url: 'https://example.com/',
      wedge: 'tollbooth',
      lensSignal: {
        verifiable: false,
        exposurePct: 87,
        aiCrawlersAllowed: ['GPTBot', 'CCBot'],
        aiCrawlersBlocked: ['ClaudeBot'],
        botAuthHeaders: false,
        renderOk: true,
      },
      message:
        '[tollbooth] lead from RenderPeek result\n' +
        'Scanned: https://example.com/\n' +
        'Operator note / crawl volume: GPTBot ~50k req/day\n' +
        'Signal: {"verifiable":false,"exposurePct":87,"aiCrawlersAllowed":["GPTBot","CCBot"],"aiCrawlersBlocked":["ClaudeBot"],"botAuthHeaders":false,"renderOk":true}',
    },
  },
  {
    name: 'tollbooth lens without name (defaults to empty) and without note (line omitted)',
    input: {
      wedge: 'tollbooth',
      email: 'ops@example.com',
      report: tollboothReport,
    },
    expected: {
      topic: 'tollbooth',
      email: 'ops@example.com',
      name: '',
      url: 'https://example.com/',
      wedge: 'tollbooth',
      lensSignal: {
        verifiable: false,
        exposurePct: 87,
        aiCrawlersAllowed: ['GPTBot', 'CCBot'],
        aiCrawlersBlocked: ['ClaudeBot'],
        botAuthHeaders: false,
        renderOk: true,
      },
      message:
        '[tollbooth] lead from RenderPeek result\n' +
        'Scanned: https://example.com/\n' +
        'Signal: {"verifiable":false,"exposurePct":87,"aiCrawlersAllowed":["GPTBot","CCBot"],"aiCrawlersBlocked":["ClaudeBot"],"botAuthHeaders":false,"renderOk":true}',
    },
  },
  {
    name: 'agentpass lens with note (no name)',
    input: {
      wedge: 'agentpass',
      email: 'ops@origin.example',
      note: 'WAF verifies at the edge',
      report: agentPassReport,
    },
    expected: {
      topic: 'agentpass',
      email: 'ops@origin.example',
      name: '',
      url: 'https://origin.example/',
      wedge: 'agentpass',
      lensSignal: {
        classification: 'signature-aware',
        evidence: ['broken-signature vs unsigned: status 403 vs 200'],
        probes: {
          signed: { status: 403, headers: {}, bodyLength: 120 },
          broken: { status: 403, headers: {}, bodyLength: 120 },
          unsigned: { status: 200, headers: {}, bodyLength: 5120 },
        },
      },
      message:
        '[agentpass] lead from RenderPeek result\n' +
        'Scanned: https://origin.example/\n' +
        'Operator note / crawl volume: WAF verifies at the edge\n' +
        'Signal: {"classification":"signature-aware","evidence":["broken-signature vs unsigned: status 403 vs 200"],"probes":{"signed":{"status":403,"headers":{},"bodyLength":120},"broken":{"status":403,"headers":{},"bodyLength":120},"unsigned":{"status":200,"headers":{},"bodyLength":5120}}}',
    },
  },
  {
    name: 'compliancefix lens with name + note',
    input: {
      wedge: 'compliancefix',
      email: 'a11y@example.org',
      name: 'Sam',
      note: 'EAA deadline June',
      report: complianceReport,
    },
    expected: {
      topic: 'compliancefix',
      email: 'a11y@example.org',
      name: 'Sam',
      url: 'https://shop.example.org/checkout',
      wedge: 'compliancefix',
      lensSignal: {
        total: 12,
        fixableInline: 7,
        byImpact: { critical: 2, serious: 5 },
        topRules: ['image-alt', 'color-contrast'],
      },
      message:
        '[compliancefix] lead from RenderPeek result\n' +
        'Scanned: https://shop.example.org/checkout\n' +
        'Operator note / crawl volume: EAA deadline June\n' +
        'Signal: {"total":12,"fixableInline":7,"byImpact":{"critical":2,"serious":5},"topRules":["image-alt","color-contrast"]}',
    },
  },
  {
    name: 'consent-enforcement lens with name + note: top three hosts, tracker flag dropped',
    input: {
      wedge: 'consent-enforcement',
      email: 'ops@shop.example.com',
      name: 'Ada',
      note: 'Cookiebot, GTM consent mode',
      report: consentReport,
    },
    expected: {
      topic: 'consent-enforcement',
      email: 'ops@shop.example.com',
      name: 'Ada',
      url: 'https://shop.example.com/',
      wedge: 'consent-enforcement',
      lensSignal: {
        verdict: 'leaks',
        trackerCount: 4,
        thirdPartyCount: 7,
        topHosts: [
          { host: 'www.google-analytics.com', requests: 3, category: 'analytics' },
          { host: 'www.googletagmanager.com', requests: 1, category: 'tag-manager' },
          { host: 'connect.facebook.net', requests: 1, category: 'social' },
        ],
        cmpDetected: { detected: true, vendor: 'Cookiebot' },
      },
      message:
        '[consent-enforcement] lead from RenderPeek result\n' +
        'Scanned: https://shop.example.com/\n' +
        'Operator note / crawl volume: Cookiebot, GTM consent mode\n' +
        'Signal: {"verdict":"leaks","trackerCount":4,"thirdPartyCount":7,"topHosts":[{"host":"www.google-analytics.com","requests":3,"category":"analytics"},{"host":"www.googletagmanager.com","requests":1,"category":"tag-manager"},{"host":"connect.facebook.net","requests":1,"category":"social"}],"cmpDetected":{"detected":true,"vendor":"Cookiebot"}}',
    },
  },
  {
    name: 'consent-enforcement lens without a host list (topHosts falls back to [])',
    input: {
      wedge: 'consent-enforcement',
      email: 'ops@shop.example.com',
      report: consentReportNoHosts,
    },
    expected: {
      topic: 'consent-enforcement',
      email: 'ops@shop.example.com',
      name: '',
      url: 'https://shop.example.com/',
      wedge: 'consent-enforcement',
      lensSignal: {
        verdict: 'leaks',
        trackerCount: 2,
        thirdPartyCount: 2,
        topHosts: [],
        cmpDetected: { detected: false, vendor: null },
      },
      message:
        '[consent-enforcement] lead from RenderPeek result\n' +
        'Scanned: https://shop.example.com/\n' +
        'Signal: {"verdict":"leaks","trackerCount":2,"thirdPartyCount":2,"topHosts":[],"cmpDetected":{"detected":false,"vendor":null}}',
    },
  },
  {
    name: 'lens-free wedge id: lensSignal null and no Signal line',
    input: {
      wedge: 'pricing',
      email: 'ops@shop.example.com',
      name: 'Ada',
      note: 'count us in',
      report: consentReport,
    },
    expected: {
      topic: 'pricing',
      email: 'ops@shop.example.com',
      name: 'Ada',
      url: 'https://shop.example.com/',
      wedge: 'pricing',
      lensSignal: null,
      message:
        '[pricing] lead from RenderPeek result\n' +
        'Scanned: https://shop.example.com/\n' +
        'Operator note / crawl volume: count us in',
    },
  },
];

// --- script-inventory gates ---
const siTotals = (ext: number, pinned: number) => ({
  scripts: 12,
  inline: 3,
  thirdPartyExternal: ext,
  thirdPartyHosts: 2,
  thirdPartyWithIntegrity: pinned,
});
const pageIntegrityFixtures: Array<{ name: string; input: unknown; expected: boolean }> = [
  { name: 'missing lens', input: undefined, expected: false },
  { name: 'null lens', input: null, expected: false },
  { name: 'ok + attention', input: { status: 'ok', verdict: 'attention' }, expected: true },
  { name: 'ok + clean', input: { status: 'ok', verdict: 'clean' }, expected: false },
  { name: 'ok + unknown', input: { status: 'ok', verdict: 'unknown' }, expected: false },
  {
    name: 'blocked + attention (hostile)',
    input: { status: 'blocked', verdict: 'attention' },
    expected: false,
  },
  {
    name: 'error + attention (hostile)',
    input: { status: 'error', verdict: 'attention' },
    expected: false,
  },
  {
    name: 'disabled + attention (hostile)',
    input: { status: 'disabled', verdict: 'attention' },
    expected: false,
  },
];
const thirdPartyFreezeFixtures: Array<{ name: string; input: unknown; expected: boolean }> = [
  { name: 'missing lens', input: undefined, expected: false },
  { name: 'null lens', input: null, expected: false },
  {
    name: 'ok + clean + unpinned third party',
    input: { status: 'ok', verdict: 'clean', totals: siTotals(2, 1) },
    expected: true,
  },
  {
    name: 'ok + attention + unpinned third party',
    input: { status: 'ok', verdict: 'attention', totals: siTotals(3, 0) },
    expected: true,
  },
  {
    name: 'ok + clean + all pinned',
    input: { status: 'ok', verdict: 'clean', totals: siTotals(2, 2) },
    expected: false,
  },
  {
    name: 'ok + clean + no third party',
    input: { status: 'ok', verdict: 'clean', totals: siTotals(0, 0) },
    expected: false,
  },
  {
    name: 'ok + unknown with counts',
    input: { status: 'ok', verdict: 'unknown', totals: siTotals(2, 0) },
    expected: false,
  },
  { name: 'ok without totals', input: { status: 'ok', verdict: 'clean' }, expected: false },
  {
    name: 'blocked + attention (hostile)',
    input: { status: 'blocked', verdict: 'attention', totals: siTotals(2, 0) },
    expected: false,
  },
  {
    name: 'error (hostile)',
    input: { status: 'error', verdict: 'clean', totals: siTotals(2, 0) },
    expected: false,
  },
  {
    name: 'disabled (hostile)',
    input: { status: 'disabled', verdict: 'clean', totals: siTotals(2, 0) },
    expected: false,
  },
];

// 30-host, 5-duplicate inventory with script paths present: the payload keeps
// the first three third-party hosts and three duplicates, and never a path.
const siHosts = [
  {
    host: 'shop.example.com',
    party: 'first',
    scripts: 9,
    withIntegrity: 0,
    category: 'first-party',
    listed: true,
  },
  ...Array.from({ length: 29 }, (_, i) => ({
    host: `cdn${i}.example.net`,
    party: 'third',
    scripts: 30 - i,
    withIntegrity: 0,
    category: i === 0 ? 'cdn' : 'other',
    listed: i === 0,
  })),
];
const siReport = {
  url: 'https://shop.example.com/checkout',
  scriptInventory: {
    status: 'ok',
    verdict: 'attention',
    reasons: ['sri-missing', 'duplicate-library'],
    blockedBy: [],
    totals: {
      scripts: 40,
      inline: 4,
      external: 36,
      firstPartyExternal: 9,
      thirdPartyExternal: 27,
      thirdPartyHosts: 29,
      thirdPartyWithIntegrity: 1,
      nonExecutable: 0,
    },
    scriptHosts: siHosts,
    sriMissing: [{ host: 'cdn0.example.net', path: '/secret/path/lib.js' }],
    floating: [],
    duplicates: Array.from({ length: 5 }, (_, i) => ({
      library: `lib${i}`,
      versions: ['1.0.0', '2.0.0'],
    })),
    csp: { enforced: false, reportOnly: true, scriptSrc: ["'self'"], reporting: false },
    note: 'x',
  },
};
const siExpectedSignal = {
  verdict: 'attention',
  reasons: ['sri-missing', 'duplicate-library'],
  totals: {
    scripts: 40,
    inline: 4,
    thirdPartyExternal: 27,
    thirdPartyHosts: 29,
    thirdPartyWithIntegrity: 1,
  },
  topHosts: [
    { host: 'cdn0.example.net', scripts: 30, category: 'cdn' },
    { host: 'cdn1.example.net', scripts: 29, category: 'other' },
    { host: 'cdn2.example.net', scripts: 28, category: 'other' },
  ],
  duplicates: [0, 1, 2].map((i) => ({ library: `lib${i}`, versions: ['1.0.0', '2.0.0'] })),
  csp: { enforced: false, reportOnly: true, scriptSrc: ["'self'"], reporting: false },
};
const siPayloadFixtures = ['page-integrity', 'third-party-freeze'].map((wedge) => ({
  name: `${wedge}: shared script-inventory signal, hosts only`,
  input: { wedge, email: 'ops@shop.example.com', name: '', note: '', report: siReport },
  expected: {
    topic: wedge,
    email: 'ops@shop.example.com',
    name: '',
    url: 'https://shop.example.com/checkout',
    wedge,
    lensSignal: siExpectedSignal,
    message:
      `[${wedge}] lead from RenderPeek result\n` +
      'Scanned: https://shop.example.com/checkout\n' +
      `Signal: ${JSON.stringify(siExpectedSignal)}`,
  },
}));

// --- edgeSeoCtaApplies -----------------------------------------------------
const edgeSeoFixtures: Array<{ name: string; input: unknown; expected: boolean }> = [
  { name: 'null', input: null, expected: false },
  { name: 'undefined', input: undefined, expected: false },
  {
    name: 'ok + attention + defects',
    input: { status: 'ok', verdict: 'attention', defects: ['canonical-missing'] },
    expected: true,
  },
  {
    name: 'ok + attention + empty defects',
    input: { status: 'ok', verdict: 'attention', defects: [] },
    expected: false,
  },
  {
    name: 'ok + attention + defects not an array',
    input: { status: 'ok', verdict: 'attention', defects: 'canonical-missing' },
    expected: false,
  },
  {
    name: 'ok + attention + defects missing',
    input: { status: 'ok', verdict: 'attention' },
    expected: false,
  },
  {
    name: 'ok + clean',
    input: { status: 'ok', verdict: 'clean', defects: [] },
    expected: false,
  },
  {
    name: 'ok + unknown',
    input: { status: 'ok', verdict: 'unknown', defects: ['canonical-missing'] },
    expected: false,
  },
  {
    name: 'blocked + attention (hostile)',
    input: { status: 'blocked', verdict: 'attention', defects: ['canonical-missing'] },
    expected: false,
  },
  {
    name: 'error + attention (hostile)',
    input: { status: 'error', verdict: 'attention', defects: ['canonical-missing'] },
    expected: false,
  },
  {
    name: 'disabled + attention (hostile)',
    input: { status: 'disabled', verdict: 'attention', defects: ['canonical-missing'] },
    expected: false,
  },
];

// edge-seo payload: ids, counts, booleans and two fixed tokens only.
const seoCounts = { total: 20, canonical: 5, hreflang: 5, title: 4, description: 3, jsonld: 3 };
const seoReport = {
  url: 'https://news.example.com/story',
  seoDefects: {
    status: 'ok',
    verdict: 'attention',
    defects: Array.from({ length: 20 }, (_, i) => `defect-${i}`),
    counts: seoCounts,
    blockedBy: [],
    staticCompared: false,
    notMeasured: ['canonical-js', 'hreflang-js', 'title-js', 'description-js', 'jsonld-js'],
    canonical: {
      static: { count: 0, href: 'https://news.example.com/secret-canonical' },
      rendered: { count: 1, href: 'https://news.example.com/secret-canonical' },
      header: null,
    },
    title: {
      static: { count: 1, text: 'Secret Title Text' },
      rendered: { count: 1, text: 'Secret Title Text' },
    },
    description: {
      static: { count: 1, text: 'Secret description text' },
      rendered: { count: 1, text: 'Secret description text' },
    },
    jsonLd: {
      static: { blocks: 0, invalid: 0, types: [] },
      rendered: { blocks: 2, invalid: 0, types: ['SecretType'] },
      renderOnlyTypes: ['SecretType'],
    },
    robots: { mismatch: true },
    delivery: { cdn: 'cloudflare', server: 'nginx' },
    note: 'x',
  },
};
const seoExpectedSignal = {
  verdict: 'attention',
  defects: seoReport.seoDefects.defects,
  counts: seoCounts,
  staticCompared: false,
  notMeasured: seoReport.seoDefects.notMeasured,
  robotsMismatch: true,
  jsonLdPresent: true,
  delivery: { cdn: 'cloudflare', server: 'nginx' },
};

// ---------------------------------------------------------------------------
// (c)+(d) Run fixtures and assert against the golden outputs.
// ---------------------------------------------------------------------------
describe.each(SOURCES)(
  'gate predicates: %s (drift sync-check vs scanner src/demand.mjs)',
  (_label, gate) => {
    const {
      tollboothCtaApplies,
      agentPassCtaApplies,
      complianceFixCtaApplies,
      consentEnforcementCtaApplies,
      pageIntegrityCtaApplies,
      thirdPartyFreezeCtaApplies,
      edgeSeoCtaApplies,
      responseFirewallCtaApplies,
      buildLeadPayload,
    } = gate;

    describe('tollboothCtaApplies', () => {
      for (const f of tollboothFixtures) {
        it(`${f.name} -> ${f.expected}`, () => {
          expect(tollboothCtaApplies(f.input)).toBe(f.expected);
        });
      }
    });

    describe('agentPassCtaApplies', () => {
      for (const f of agentPassFixtures) {
        it(`${f.name} -> ${f.expected}`, () => {
          expect(agentPassCtaApplies(f.input)).toBe(f.expected);
        });
      }
    });

    describe('complianceFixCtaApplies', () => {
      for (const f of complianceFixtures) {
        it(`${f.name} -> ${f.expected}`, () => {
          expect(complianceFixCtaApplies(f.input)).toBe(f.expected);
        });
      }
    });

    describe('consentEnforcementCtaApplies', () => {
      for (const f of consentFixtures) {
        it(`${f.name} -> ${f.expected}`, () => {
          expect(consentEnforcementCtaApplies(f.input)).toBe(f.expected);
        });
      }
    });

    describe('pageIntegrityCtaApplies', () => {
      for (const f of pageIntegrityFixtures) {
        it(`${f.name} -> ${f.expected}`, () => {
          expect(pageIntegrityCtaApplies(f.input)).toBe(f.expected);
        });
      }
    });

    describe('thirdPartyFreezeCtaApplies', () => {
      for (const f of thirdPartyFreezeFixtures) {
        it(`${f.name} -> ${f.expected}`, () => {
          expect(thirdPartyFreezeCtaApplies(f.input)).toBe(f.expected);
        });
      }
    });

    describe('script-inventory payload', () => {
      for (const f of siPayloadFixtures) {
        it(f.name, () => {
          const out = buildLeadPayload(f.input);
          expect(out).toEqual(f.expected);
          expect(JSON.stringify(out)).not.toContain('/secret/path');
          expect(JSON.stringify(out.lensSignal).length).toBeLessThan(2000);
        });
      }
    });

    describe('edgeSeoCtaApplies', () => {
      for (const f of edgeSeoFixtures) {
        it(`${f.name} -> ${f.expected}`, () => {
          expect(edgeSeoCtaApplies(f.input)).toBe(f.expected);
        });
      }
    });

    describe('edge-seo payload', () => {
      it('carries ids, counts, booleans and two tokens, never page text or URLs', () => {
        const out = buildLeadPayload({
          wedge: 'edge-seo',
          email: 'ops@agency.example.com',
          name: '',
          note: 'sites: 12; WordPress',
          report: seoReport,
        });
        expect(out).toEqual({
          topic: 'edge-seo',
          email: 'ops@agency.example.com',
          name: '',
          url: 'https://news.example.com/story',
          wedge: 'edge-seo',
          lensSignal: seoExpectedSignal,
          message:
            '[edge-seo] lead from RenderPeek result\n' +
            'Scanned: https://news.example.com/story\n' +
            'Operator note / crawl volume: sites: 12; WordPress\n' +
            `Signal: ${JSON.stringify(seoExpectedSignal)}`,
        });
        const signal = JSON.stringify(out.lensSignal);
        for (const leak of [
          'secret-canonical',
          'Secret Title',
          'Secret description',
          'SecretType',
        ]) {
          expect(signal).not.toContain(leak);
        }
        expect(signal.length).toBeLessThan(2000);
      });

      it('reports no robots mismatch, no JSON-LD and null delivery tokens when absent', () => {
        const out = buildLeadPayload({
          wedge: 'edge-seo',
          email: 'a@b.example',
          report: {
            url: 'https://x.example/',
            seoDefects: {
              verdict: 'attention',
              defects: ['canonical-missing'],
              counts: { total: 1, canonical: 1, hreflang: 0, title: 0, description: 0, jsonld: 0 },
              staticCompared: true,
              notMeasured: [],
              jsonLd: { static: { blocks: 0 }, rendered: { blocks: 0 } },
            },
          },
        });
        expect(out.lensSignal).toMatchObject({
          robotsMismatch: false,
          jsonLdPresent: false,
          delivery: { cdn: null, server: null },
        });
      });
    });

    describe('buildLeadPayload', () => {
      for (const f of buildLeadFixtures) {
        it(f.name, () => {
          expect(buildLeadPayload(f.input)).toEqual(f.expected);
        });
      }
    });

    describe('responseFirewallCtaApplies (demand module only)', () => {
      it.skipIf(!gate.responseFirewallCtaApplies)(
        'fires on debug output or an end-of-life product',
        () => {
          const f = responseFirewallCtaApplies!;
          const ok = (reasons: unknown) => f({ status: 'ok', verdict: 'attention', reasons });
          expect(f(null)).toBe(false);
          expect(f({ status: 'blocked', verdict: 'attention', reasons: ['debug-output'] })).toBe(
            false,
          );
          expect(f({ status: 'ok', verdict: 'clean', reasons: ['debug-output'] })).toBe(false);
          expect(f({ status: 'ok', verdict: 'attention', reasons: 'debug-output' })).toBe(false);
          expect(ok(['missing-headers'])).toBe(false);
          expect(ok(['debug-output'])).toBe(true);
          expect(ok(['end-of-life'])).toBe(true);
          expect(ok(['missing-headers', 'end-of-life'])).toBe(true);
        },
      );

      describe('response-firewall payload (demand module only)', () => {
        const rxOk = (o = {}) => ({
          status: 'ok',
          verdict: 'attention',
          reasons: ['debug-output', 'version-tell', 'missing-headers'],
          leaks: [{ id: 'php-error', count: 2, where: 'both' }],
          tells: [
            { product: 'apache', source: 'server', version: '2.4.29' },
            { product: 'php', source: 'x-powered-by', version: '7.4.33' },
          ],
          endOfLife: [{ product: 'php', line: '7.4', basis: 'eol-date', date: '2022-11-28' }],
          pagespeed: { header: 'x-mod-pagespeed', bucket: 'google-era' },
          headers: { https: true, present: ['hsts'], missing: ['nosniff', 'framing'] },
          delivery: { cdn: 'cloudflare', server: 'apache' },
          ...o,
        });
        const skip = !gate.responseFirewallCtaApplies;

        it.skipIf(skip)('carries ids, counts, product and line tokens and no versions', () => {
          const p = buildLeadPayload({
            wedge: 'response-firewall',
            email: 'op@hoster.example',
            note: 'sites: 40',
            report: { url: 'https://shop.example/', responseExposure: rxOk() },
          });
          expect(p.topic).toBe('response-firewall');
          expect(p.wedge).toBe('response-firewall');
          expect(p.lensSignal).toEqual({
            verdict: 'attention',
            reasons: ['debug-output', 'version-tell', 'missing-headers'],
            leaks: [{ id: 'php-error', count: 2 }],
            endOfLife: [{ product: 'php', line: '7.4', basis: 'eol-date' }],
            tells: ['apache', 'php'],
            missingHeaders: ['nosniff', 'framing'],
            https: true,
            pagespeedBucket: 'google-era',
            delivery: { cdn: 'cloudflare', server: 'apache' },
          });
          expect(String(p.message)).toContain('[response-firewall] lead from RenderPeek result');
          expect(String(p.message)).toContain('Operator note / crawl volume: sites: 40');
          const js = JSON.stringify(p.lensSignal);
          expect(js).not.toContain('://');
          expect(js).not.toContain('7.4.33');
          expect(js).not.toContain('2.4.29');
          expect(p.url).toBe('https://shop.example/');
        });

        it.skipIf(skip)('gives a null signal for a blocked, disabled or missing lens', () => {
          for (const rx of [
            { status: 'blocked' },
            { status: 'disabled' },
            { status: 'error' },
            undefined,
          ]) {
            const p = buildLeadPayload({
              wedge: 'response-firewall',
              email: 'op@hoster.example',
              report: { url: 'https://shop.example/', responseExposure: rx },
            });
            expect(p.lensSignal).toBeNull();
            expect(String(p.message)).not.toContain('Signal:');
          }
        });

        it.skipIf(skip)('tolerates missing headers, pagespeed and delivery', () => {
          const p = buildLeadPayload({
            wedge: 'response-firewall',
            email: 'op@hoster.example',
            report: {
              url: 'https://x.example/',
              responseExposure: { status: 'ok', verdict: 'attention', reasons: ['end-of-life'] },
            },
          });
          expect(p.lensSignal).toEqual({
            verdict: 'attention',
            reasons: ['end-of-life'],
            leaks: [],
            endOfLife: [],
            tells: [],
            missingHeaders: [],
            https: false,
            pagespeedBucket: null,
            delivery: { cdn: null, server: null },
          });
        });
      });
    });
  },
);
