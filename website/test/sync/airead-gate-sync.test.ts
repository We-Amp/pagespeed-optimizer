// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// airead-gate-sync.test.ts — GOLDEN-CONTRACT sync-check for the demand-capture
// gate predicates inlined into the public RenderPeek page.
//
// CONTEXT
//   src/pages/ai-readability/index.astro carries a VERBATIM inline copy of four
//   functions whose canonical source of truth is the scanner repo's
//   src/demand.mjs (agent-readability-scanner). The page inlines them (rather
//   than importing) because in production it is the Astro-built
//   modpagespeed.com/ai-readability/ page and only /ai-readability/api/* is
//   proxied to the render service — there is no /src/ route to import from. The
//   page and scanner each carry a "KEEP IN SYNC" comment.
//
// WHY A GOLDEN CONTRACT (not an import-and-compare)
//   This (mps2) repo cannot import the scanner's demand.mjs — it lives in a
//   separate private repo and is not vendored here. So instead of comparing the
//   two implementations directly, we:
//     (a) read index.astro and extract the four inline function definitions
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
// CANONICAL CONTRACT (mirror of agent-readability-scanner:src/demand.mjs)
//   tollboothCtaApplies(agentVerifiability): true iff
//     detail.verifiable === false && detail.renderOk === true &&
//     Array.isArray(detail.aiCrawlersAllowed) && detail.aiCrawlersAllowed.length > 0 &&
//     detail.exposurePct >= 30
//   agentPassCtaApplies(signedAgentVerification): true iff
//     status === 'ok' && classification in {'verifying','signature-aware'}
//   complianceFixCtaApplies(accessibility): true iff
//     accessibility.available && accessibility.detail.fixableInline > 0
//   buildLeadPayload({wedge,email,name,note,report}) -> { topic, email, name,
//     url, wedge, lensSignal, message }; lensSignal shape is lens-specific and
//     message is the human-readable mirror.
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
// (a) Extract the three inline gate functions from index.astro and evaluate
//     them in an isolated VM sandbox. We slice the source between the
//     KEEP-IN-SYNC anchor (start of the gate block) and the normalizeUrl
//     comment (first line after buildLeadPayload). If those anchors ever move,
//     this extraction throws loudly rather than silently testing nothing.
// ---------------------------------------------------------------------------
function extractGateFunctions(): {
  tollboothCtaApplies: (av: unknown) => boolean;
  agentPassCtaApplies: (sa: unknown) => boolean;
  complianceFixCtaApplies: (ax: unknown) => boolean;
  buildLeadPayload: (opts: unknown) => Record<string, unknown>;
} {
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

  // Sanity: all four canonical functions must be present in the extracted slice.
  for (const fn of ['tollboothCtaApplies', 'agentPassCtaApplies', 'complianceFixCtaApplies', 'buildLeadPayload']) {
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
  const wrapped = `${block}\nglobalThis.__gate__ = { tollboothCtaApplies, agentPassCtaApplies, complianceFixCtaApplies, buildLeadPayload };`;
  vm.runInContext(wrapped, context, { filename: 'index.astro:inline-gate' });

  const gate = (sandbox as { __gate__?: Record<string, unknown> }).__gate__;
  if (!gate) {
    throw new Error('Extracted gate block did not expose the expected functions.');
  }
  return gate as ReturnType<typeof extractGateFunctions>;
}

const { tollboothCtaApplies, agentPassCtaApplies, complianceFixCtaApplies, buildLeadPayload } =
  extractGateFunctions();

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
    input: { detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 30 } },
    expected: true,
  },
  {
    name: 'verifiable true blocks',
    input: { detail: { verifiable: true, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 80 } },
    expected: false,
  },
  {
    name: 'verifiable truthy-but-not-strict-false blocks (must be === false)',
    input: { detail: { verifiable: 0, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 80 } },
    expected: false,
  },
  {
    name: 'renderOk false blocks',
    input: { detail: { verifiable: false, renderOk: false, aiCrawlersAllowed: ['GPTBot'], exposurePct: 80 } },
    expected: false,
  },
  {
    name: 'renderOk truthy-but-not-strict-true blocks (must be === true)',
    input: { detail: { verifiable: false, renderOk: 1, aiCrawlersAllowed: ['GPTBot'], exposurePct: 80 } },
    expected: false,
  },
  {
    name: 'aiCrawlersAllowed not an array blocks',
    input: { detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: 'GPTBot', exposurePct: 80 } },
    expected: false,
  },
  {
    name: 'aiCrawlersAllowed empty array blocks',
    input: { detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: [], exposurePct: 80 } },
    expected: false,
  },
  {
    name: 'exposurePct at threshold 30 passes (>=)',
    input: { detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 30 } },
    expected: true,
  },
  {
    name: 'exposurePct just below threshold (29) blocks',
    input: { detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: ['GPTBot'], exposurePct: 29 } },
    expected: false,
  },
  {
    name: 'exposurePct well above threshold passes',
    input: { detail: { verifiable: false, renderOk: true, aiCrawlersAllowed: ['GPTBot', 'CCBot'], exposurePct: 95 } },
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
  { name: 'not available', input: { available: false, detail: { fixableInline: 5 } }, expected: false },
  { name: 'available but no detail', input: { available: true }, expected: false },
  { name: 'available, detail.fixableInline = 0 blocks', input: { available: true, detail: { fixableInline: 0 } }, expected: false },
  { name: 'available, detail.fixableInline > 0 passes', input: { available: true, detail: { fixableInline: 1 } }, expected: true },
  { name: 'available, detail.fixableInline large passes', input: { available: true, detail: { fixableInline: 42 } }, expected: true },
];

// --- buildLeadPayload: both lenses + name/note variations ---
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

const buildLeadFixtures: Array<{ name: string; input: unknown; expected: Record<string, unknown> }> = [
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
];

// ---------------------------------------------------------------------------
// (c)+(d) Run fixtures and assert against the golden outputs.
// ---------------------------------------------------------------------------
describe('ai-readability inline gate predicates (drift sync-check vs scanner src/demand.mjs)', () => {
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

  describe('buildLeadPayload', () => {
    for (const f of buildLeadFixtures) {
      it(f.name, () => {
        expect(buildLeadPayload(f.input)).toEqual(f.expected);
      });
    }
  });
});
