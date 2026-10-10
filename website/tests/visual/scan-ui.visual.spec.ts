// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Visual suite for the scanner pages. Opt-in: VISUAL=1 npx playwright test --project=visual
// Writes test-results/visual/{page}-{NN}-{state}-{viewport}.png for review; it asserts
// nothing about pixels, so it never gates CI.

import { mkdirSync } from 'node:fs';
import { test, type Page } from '@playwright/test';
import { installMocks, makeDeterministic, settle, type Mocks } from './mocks';

const OUT_DIR = 'test-results/visual';

const PAGES = [
  { name: 'analyze', path: '/analyze/' },
  { name: 'ai-readability', path: '/ai-readability/' },
] as const;

const VIEWPORTS = [
  { name: 'desktop', width: 1280, height: 800 },
  { name: 'mobile', width: 390, height: 844 },
] as const;

type PageDef = (typeof PAGES)[number];
type ViewportDef = (typeof VIEWPORTS)[number];

interface Ctx {
  page: Page;
  mocks: Mocks;
  target: PageDef;
  viewport: ViewportDef;
  /** Writes test-results/visual/{page}-{state}-{viewport}.png. */
  shot: (state: string, options?: { fullPage?: boolean }) => Promise<void>;
}

interface State {
  /** State number from the plan; also the filename prefix. */
  n: number;
  slug: string;
  title: string;
  /** Mock timing the state needs; the state body reads this. */
  mock: string;
  /** Present once the v2 DOM exists; absent states are registered as skipped. */
  run?: (ctx: Ctx) => Promise<void>;
}

const pad = (n: number) => String(n).padStart(2, '0');

const STATES: State[] = [
  {
    n: 1,
    slug: 'landing',
    title: 'landing',
    mock: 'No requests; the page as first loaded, above the fold.',
    run: async ({ shot }) => {
      await shot('landing');
    },
  },
  {
    n: 2,
    slug: 'loading',
    title: 'loading, all three Checking',
    mock: 'scan, psiMobile and psiDesktop all gated (never released); capture after submit.',
  },
  {
    n: 3,
    slug: 'partial',
    title: 'partial, scan in and PSI pending',
    mock: 'scan fixture full answers at 0 ms; psiMobile and psiDesktop gated.',
  },
  {
    n: 4,
    slug: 'results-collapsed',
    title: 'results, all panels collapsed',
    mock: 'scan full and both PSI answer at 0 ms; capture once all three tiles settle.',
  },
  {
    n: 5,
    slug: 'panel-speed',
    title: 'Speed panel open',
    mock: 'As state 4, then open the Speed panel.',
  },
  {
    n: 6,
    slug: 'panel-ai',
    title: 'AI readability panel open',
    mock: 'As state 4, then open the AI readability panel.',
  },
  {
    n: 7,
    slug: 'panel-risk',
    title: 'Risk and SEO panel open',
    mock: 'As state 4, then open the Risk and SEO panel.',
  },
  {
    n: 8,
    slug: 'form-preselected',
    title: 'form with pre-selected chips',
    mock: 'As state 4; scroll to the form; gated chips are pre-selected from the full report.',
  },
  {
    n: 9,
    slug: 'form-sites-field',
    title: 'form with the sites field',
    mock: 'As state 8, then deselect one pre-selected chip and select SEO fixes or Legacy sites (only four are pre-selected) so the sites field shows.',
  },
  {
    n: 10,
    slug: 'form-cap-message',
    title: 'topic cap message',
    mock: 'As state 8, then select a fifth chip so the "Pick up to four topics." message shows.',
  },
  {
    n: 11,
    slug: 'submitted',
    title: 'submitted',
    mock: 'As state 8; contact answers 200 at 0 ms; fill email, submit, capture the confirmation.',
  },
  {
    n: 12,
    slug: 'form-partial-failure',
    title: 'form partial failure, one topic not sent',
    mock: 'As state 8 with two or more chips selected; contact status [200, 500] (first POST ok, second fails); capture the "We could not send" message.',
  },
  {
    n: 13,
    slug: 'scan-error',
    title: 'scan error',
    mock: 'scan status 503 with an error body (0 ms); both PSI answer 200.',
  },
  {
    n: 14,
    slug: 'psi-rate-limited',
    title: 'PSI rate-limited',
    mock: 'both PSI status 429 with the default nginx HTML 429 page as body; scan fixture full at 0 ms.',
  },
  {
    n: 15,
    slug: 'clean',
    title: 'clean report and all-clean speed',
    mock: 'scan fixture clean; both PSI answer 200 with every audit passing (body override).',
  },
];

test.describe('scan UI visual states', () => {
  test.beforeAll(() => {
    mkdirSync(OUT_DIR, { recursive: true });
  });

  for (const target of PAGES) {
    for (const viewport of VIEWPORTS) {
      for (const state of STATES) {
        const name = `${pad(state.n)} ${state.title} [${target.name}, ${viewport.name}]`;
        if (!state.run) {
          // Needs the v2 DOM. Mock timing: state.mock
          test.skip(name, async () => {});
          continue;
        }
        const run = state.run;
        test(name, async ({ page }) => {
          await page.setViewportSize({ width: viewport.width, height: viewport.height });
          await makeDeterministic(page);
          const mocks = await installMocks(page);
          // Warm-up visit: the dev server re-optimises its module graph on the
          // first hit of a page and reloads once; do that outside the capture.
          await page.goto(target.path);
          await page.waitForLoadState('networkidle');
          await page.goto(target.path);
          await page.waitForLoadState('networkidle');
          await settle(page);
          await run({
            page,
            mocks,
            target,
            viewport,
            shot: async (slug, options) => {
              await settle(page);
              const n = STATES.find((s) => s.slug === slug)?.n ?? 0;
              await page.screenshot({
                // The telemetry strip shows live timings; mask it for stable runs.
                mask: [page.locator('[data-ui="telemetry-strip"]')],
                maskColor: '#070809',
                path: `${OUT_DIR}/${target.name}-${pad(n)}-${slug}-${viewport.name}.png`,
                animations: 'disabled',
                caret: 'hide',
                fullPage: options?.fullPage ?? false,
              });
            },
          });
        });
      }
    }
  }
});
