// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import type { Page, Route } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { stubUmami, trackedEvents } from './helpers/umami';

// Capture on the free tools: the AI-readability checker (/ai-readability/)
// and the PageSpeed analyzer (/analyze/). The checker cards post to the
// shared endpoint /ai-readability/api/contact; every successful submit fires
// umami lead_submit with {channel, topic, wedge, source_path} and removes the
// form. The analyzer's report download is fully client-side (Blob, no
// network) and fires analyze_report_download once per click. No card on
// either page promises a re-check or an emailed report: the checker's result
// keeps itself via "Copy link to this result" (the scanner-minted permalink,
// copied client-side; fires airead-copy-link) with no email field and no
// request to the /notify endpoint.

// Minimal scanner response that renders the full result card without firing
// any wedge (no tollbooth/agentpass/compliancefix signals).
const SCAN_OK = {
  report: {
    url: 'https://example.com/',
    grade: 'B',
    score: 72,
    categories: {
      'Content fidelity': { score: 18, max: 30, verdict: 'Most content is in the raw HTML.' },
    },
    diff: {
      staticCleanTokens: 100,
      staticMarkdown: '# Example\nSome readable text.',
      renderedCleanTokens: 120,
      renderedMarkdown: '# Example\nSome readable text, plus a little more.',
    },
  },
};

// Same minimal scan, but the signed-agent probe fires the agentpass wedge, so
// the per-wedge lead form (and its disclosure) renders.
const SCAN_AGENTPASS = {
  report: {
    ...SCAN_OK.report,
    signedAgentVerification: {
      status: 'ok',
      classification: 'verifying',
      evidence: ['valid signature accepted, corrupted signature rejected'],
      probes: [{ name: 'valid' }, { name: 'corrupted' }, { name: 'unsigned' }],
    },
  },
};

// Score 0 must survive the signup lead message as "0/100" — a `||` fallback
// would blank it.
const SCAN_ZERO = {
  report: {
    ...SCAN_OK.report,
    grade: 'F',
    score: 0,
  },
};

// The scanner mints a permalink for every successful scan (GET /scan responds
// { id, permalink, badge, report }); the result card's share section renders
// from it, and the copy-link control copies exactly this URL.
const PERMALINK = 'https://modpagespeed.com/ai-readability/r/testscan1';

// SCAN_OK plus the permalink every real scan response carries.
const SCAN_LINK = { permalink: PERMALINK, report: { ...SCAN_OK.report } };

// One wedge fires (agentpass): the wedge lead form renders, and the old
// secondary "Email me this result instead" button must not.
const SCAN_AGENTPASS_LINK = { permalink: PERMALINK, report: { ...SCAN_AGENTPASS.report } };

// Two wedges fire (agentpass + compliancefix): the topic chooser renders and
// must offer only the wedge topics — no "Just email me this result" radio.
const SCAN_TWO_WEDGES_LINK = {
  permalink: PERMALINK,
  report: {
    ...SCAN_OK.report,
    signedAgentVerification: {
      status: 'ok',
      classification: 'verifying',
      evidence: ['valid signature accepted, corrupted signature rejected'],
      probes: [{ name: 'valid' }, { name: 'corrupted' }, { name: 'unsigned' }],
    },
    accessibility: {
      available: true,
      detail: {
        total: 4,
        fixableInline: 3,
        byImpact: { serious: 4 },
        topRules: [{ id: 'image-alt', impact: 'serious', help: 'Images need alt text', nodes: 4 }],
      },
    },
  },
};

// The pre-consent lens observed tracker requests before any interaction: the
// "Pre-consent leak" row reads "leaks before consent" and the consent-
// enforcement card renders with its own form (no other wedge fires, so the
// consolidated next-step form must not).
const PCL_LEAKS = {
  status: 'ok',
  verdict: 'leaks',
  trackerCount: 4,
  thirdPartyCount: 5,
  thirdPartyHosts: [
    { host: 'www.google-analytics.com', requests: 3, category: 'analytics', tracker: true },
    { host: 'www.googletagmanager.com', requests: 1, category: 'tag-manager', tracker: true },
    { host: 'connect.facebook.net', requests: 1, category: 'social', tracker: true },
    { host: 'static.hotjar.com', requests: 1, category: 'session-replay', tracker: true },
    { host: 'consent.cookiebot.com', requests: 1, category: 'cmp', tracker: false },
  ],
  cmpDetected: { detected: true, vendor: 'Cookiebot' },
  firstParty: ['example.com'],
  requestsObserved: 22,
};
const SCAN_CONSENT_LEAKS = { report: { ...SCAN_OK.report, preConsentLeak: PCL_LEAKS } };

// Third parties but no tracker: the row reads "clean" and no card renders.
const SCAN_CONSENT_CLEAN = {
  report: {
    ...SCAN_OK.report,
    preConsentLeak: {
      status: 'ok',
      verdict: 'clean',
      trackerCount: 0,
      thirdPartyCount: 1,
      thirdPartyHosts: [
        { host: 'fonts.googleapis.com', requests: 1, category: 'cdn', tracker: false },
      ],
      cmpDetected: { detected: false, vendor: null },
      firstParty: ['example.com'],
      requestsObserved: 5,
    },
  },
};

// The lens is switched off at the scanner: stable shape, nothing rendered.
const SCAN_CONSENT_DISABLED = {
  report: {
    ...SCAN_OK.report,
    preConsentLeak: {
      status: 'disabled',
      verdict: 'unknown',
      trackerCount: 0,
      thirdPartyCount: 0,
      thirdPartyHosts: [],
      cmpDetected: { detected: false, vendor: null },
    },
  },
};

// The render hit a bot wall or a challenge page: the scanner reports the lens
// as blocked with a note. The row says so, carries no counts, and no card
// renders. An errored lens without a note renders the same way with the
// default line.
const SCAN_CONSENT_BLOCKED = {
  report: {
    ...SCAN_OK.report,
    preConsentLeak: {
      status: 'blocked',
      verdict: 'unknown',
      trackerCount: 0,
      thirdPartyCount: 0,
      thirdPartyHosts: [],
      cmpDetected: { detected: false, vendor: null },
      note: 'The site answered our render with a challenge page, so we could not observe its requests.',
    },
  },
};
const SCAN_CONSENT_ERROR = {
  report: { ...SCAN_OK.report, preConsentLeak: { status: 'error', reason: 'boom' } },
};

// Minimal PSI response: a 0.9 performance score and one flagged audit
// (render-blocking-resources maps to mod_pagespeed coverage "full").
const PSI_OK = {
  lighthouseResult: {
    categories: { performance: { score: 0.9 } },
    audits: {
      'render-blocking-resources': {
        id: 'render-blocking-resources',
        title: 'Eliminate render-blocking resources',
        score: 0.4,
      },
    },
  },
};

async function stubContact(page: Page, status = 200) {
  const posted: Array<Record<string, unknown>> = [];
  await page.route('**/ai-readability/api/contact', async (route: Route) => {
    posted.push(route.request().postDataJSON() as Record<string, unknown>);
    await route.fulfill({
      status,
      contentType: 'application/json',
      body: status === 200 ? '{"ok":true}' : '{"error":"nope"}',
    });
  });
  return posted;
}

async function runScan(page: Page, scanBody: unknown = SCAN_OK) {
  await page.route('**/ai-readability/api/scan**', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(scanBody),
    });
  });
  await page.goto('/ai-readability/');
  await page.fill('#ar-url', 'example.com');
  await page.click('#ar-go');
  // Generous timeout: the dev server compiles the page on first hit.
  await expect(page.locator('#ar-watch-form')).toBeVisible({ timeout: 15000 });
}

async function runAnalysis(page: Page) {
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(PSI_OK),
    });
  });
  await page.goto('/analyze/');
  await page.fill('#psi-url', 'https://example.com');
  await page.click('#psi-submit');
  // Generous timeout: the dev server compiles the page on first hit.
  await expect(page.locator('#psi-report-download')).toBeVisible({ timeout: 15000 });
}

test.describe('AI-readability checker capture cards', () => {
  test('Follow AI-readability work posts ai-readability-updates, then the form is gone', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    // The reframed card: no promise of a re-check or of an emailed report.
    const section = page.locator('section[aria-labelledby="ar-h-watch"]');
    await expect(page.locator('#ar-h-watch')).toHaveText('Follow AI-readability work');
    await expect(section).toContainText(
      'Leave your email if you want to hear when we publish new checks or research on AI crawlers. We send nothing else.',
    );
    await expect(section).toContainText(
      'Email me about new checks and research. I can ask to be removed at any time',
    );
    await expect(section).not.toContainText('re-check');

    await page.fill('#ar-watch-email', 'watcher@example.com');
    await page.check('#ar-watch-consent');
    await page.click('#ar-watch-form button[type="submit"]');

    await expect(page.locator('#ar-watch-msg')).toContainText('Thanks');
    // The form removes itself on success, so a repeat click cannot post again.
    await expect(page.locator('#ar-watch-form')).toHaveCount(0);
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'ai-readability-updates',
      email: 'watcher@example.com',
      url: 'https://example.com/',
    });
    expect(String(posted[0].message)).toContain('Scanned: https://example.com/');
    expect(String(posted[0].message)).toContain('Grade: B (72/100)');

    const events = await trackedEvents(page);
    const leads = events.filter((e) => e.name === 'lead_submit');
    expect(leads).toEqual([
      {
        name: 'lead_submit',
        data: {
          channel: 'scan',
          topic: 'ai-readability-updates',
          wedge: '',
          source_path: '/ai-readability/',
        },
      },
    ]);
  });

  test('the consent checkbox is required for the updates signup', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.fill('#ar-watch-email', 'watcher@example.com');
    await page.click('#ar-watch-form button[type="submit"]');
    // Native validation blocks the submit; nothing is posted, no lead fires.
    await expect(page.locator('#ar-watch-msg')).toHaveText('');
    await expect(page.locator('#ar-watch-form')).toBeVisible();
    expect(posted).toHaveLength(0);
  });

  test('the agents form posts topic agents with the chosen need as wedge, then disappears', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    // Statement heading, empty placeholder option, and the submit disclosure.
    await expect(page.locator('#ar-h-agents')).toHaveText('Working on agent access at the origin');
    const section = page.locator('section[aria-labelledby="ar-h-agents"]');
    await expect(section).toContainText(
      'Submitting this sends us your email, your answer and this scan’s URL. We use them only for this request; see our privacy policy.',
    );
    const placeholder = page.locator('#ar-agents-need option[value=""]');
    await expect(placeholder).toHaveText('Choose one');
    await expect(placeholder).toHaveAttribute('disabled', '');

    await page.selectOption('#ar-agents-need', 'agent-verification');
    await page.fill('#ar-agents-email', 'operator@example.com');
    await page.fill('#ar-agents-log', 'GPTBot: 12000/mo, ClaudeBot: 8000/mo');
    await page.click('#ar-agents-form button[type="submit"]');

    await expect(page.locator('#ar-agents-msg')).toContainText('Thanks');
    await expect(page.locator('#ar-agents-msg')).not.toContainText('in touch');
    await expect(page.locator('#ar-agents-form')).toHaveCount(0);
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'agents',
      email: 'operator@example.com',
      wedge: 'agent-verification',
      url: 'https://example.com/',
    });
    expect(String(posted[0].message)).toContain('Need: Signed-agent verification');
    expect(String(posted[0].message)).toContain('GPTBot: 12000/mo, ClaudeBot: 8000/mo');

    const events = await trackedEvents(page);
    const leads = events.filter((e) => e.name === 'lead_submit');
    expect(leads).toEqual([
      {
        name: 'lead_submit',
        data: {
          channel: 'scan',
          topic: 'agents',
          wedge: 'agent-verification',
          source_path: '/ai-readability/',
        },
      },
    ]);
  });

  test('the agents form requires a chosen need (empty option keeps required working)', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.fill('#ar-agents-email', 'operator@example.com');
    await page.click('#ar-agents-form button[type="submit"]');
    // No need selected: native validation blocks the submit; nothing posts and
    // the form stays put so the visitor can pick an option.
    await expect(page.locator('#ar-agents-msg')).toHaveText('');
    await expect(page.locator('#ar-agents-form')).toBeVisible();
    expect(posted).toHaveLength(0);
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(0);
  });

  test('the wedge form posts the fired wedge and its copy promises no follow-up', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page, SCAN_AGENTPASS);

    // The per-request disclosure states what is sent without promising follow-up.
    const disclosure = page.locator('p.ar-disclosure', { hasText: 'summary of this scan' });
    await expect(disclosure).toContainText('We use them only for this request');
    await expect(disclosure).not.toContainText('follow up');

    await page.fill('#ar-next-email', 'operator@example.com');
    await page.click('#ar-next-form button[type="submit"]');

    await expect(page.locator('#ar-next-msg')).toContainText('Thanks');
    await expect(page.locator('#ar-next-msg')).not.toContainText('in touch');
    await expect(page.locator('#ar-next-form')).toHaveCount(0);
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'agentpass',
      email: 'operator@example.com',
      wedge: 'agentpass',
      url: 'https://example.com/',
    });
  });

  test('a score of 0 posts as 0/100 in the signup lead message', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page, SCAN_ZERO);

    await page.fill('#ar-watch-email', 'watcher@example.com');
    await page.check('#ar-watch-consent');
    await page.click('#ar-watch-form button[type="submit"]');

    await expect(page.locator('#ar-watch-msg')).toContainText('Thanks');
    expect(String(posted[0].message)).toContain('Grade: F (0/100)');
  });

  test('the operator log is optional', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.selectOption('#ar-agents-need', 'ai-crawler-content');
    await page.fill('#ar-agents-email', 'operator@example.com');
    await page.click('#ar-agents-form button[type="submit"]');
    await expect(page.locator('#ar-agents-msg')).toContainText('Thanks');
    await expect(page.locator('#ar-agents-msg')).not.toContainText('in touch');
    expect(String(posted[0].message)).not.toContain('AI-crawler request counts');
  });

  test('the operator log posts at most 1500 characters', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.selectOption('#ar-agents-need', 'ai-crawler-content');
    await page.fill('#ar-agents-email', 'operator@example.com');
    // A scripted submit bypasses the textarea's maxlength, so set the value
    // directly: the submit handler slices it to 1500.
    await page.evaluate(() => {
      (document.querySelector('#ar-agents-log') as HTMLTextAreaElement).value = 'x'.repeat(1600);
    });
    await page.click('#ar-agents-form button[type="submit"]');
    await expect(page.locator('#ar-agents-msg')).toContainText('Thanks');
    const message = String(posted[0].message);
    expect(message).toContain('AI-crawler request counts:\n' + 'x'.repeat(1500));
    expect(message).not.toContain('x'.repeat(1501));
  });

  test('a 500 from the contact endpoint shows the error path and fires no lead', async ({
    page,
  }) => {
    const posted = await stubContact(page, 500);
    await stubUmami(page);
    await runScan(page);

    await page.fill('#ar-watch-email', 'watcher@example.com');
    await page.check('#ar-watch-consent');
    await page.click('#ar-watch-form button[type="submit"]');

    await expect(page.locator('#ar-watch-msg')).toContainText('Something went wrong');
    // Failure leaves the form in place so the visitor can retry.
    await expect(page.locator('#ar-watch-form')).toBeVisible();
    expect(posted).toHaveLength(1);
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(0);
  });
});

test.describe('AI-readability pre-consent leak lens and consent-enforcement card', () => {
  test('a leaking page gets the row, the card, and a consent-enforcement lead with the lens signal', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page, SCAN_CONSENT_LEAKS);

    // The row: verdict label, counts, consent banner, the top three hosts (trackers first).
    const row = page.locator('.ar-lens', { hasText: 'Pre-consent leak' });
    await expect(row).toHaveCount(1);
    await expect(row).toHaveClass(/ar-finding/);
    await expect(row.locator('.ar-h4-r')).toHaveText('leaks before consent');
    await expect(row).toContainText(
      '4 tracker host(s) contacted before any interaction, in a fresh browser with no consent given: www.google-analytics.com (analytics), www.googletagmanager.com (tag-manager), connect.facebook.net (social), ….',
    );
    await expect(row).toContainText('A consent banner (Cookiebot) is present');
    await expect(row.locator('.ar-chip-label')).toHaveText(
      '4 tracker hosts · 5 third-party hosts · consent banner: Cookiebot',
    );
    await expect(row.locator('.ar-chip')).toHaveText([
      'www.google-analytics.com · analytics',
      'www.googletagmanager.com · tag-manager',
      'connect.facebook.net · social',
    ]);

    // The card: heading, the one-sentence fix, the platform link, the disclosure.
    const card = page.locator('section[aria-labelledby="ar-h-consent"]');
    await expect(page.locator('#ar-h-consent')).toHaveText('Tags fire before consent');
    await expect(card).toContainText(
      'With consent enforcement at the origin, third-party tags stay inert at the server until the consent cookie grants them.',
    );
    await expect(card.locator('a[href="/platform/consent/"]')).toHaveText(
      'Consent enforcement at the origin →',
    );
    await expect(card.locator('p.ar-disclosure')).toContainText(
      'We use them only for this request',
    );
    // No other wedge fired, so the consolidated next-step form is absent.
    await expect(page.locator('#ar-next-form')).toHaveCount(0);

    await page.fill('#ar-consent-email', 'operator@example.com');
    await page.click('#ar-consent-form button[type="submit"]');

    await expect(page.locator('#ar-consent-msg')).toContainText('Thanks');
    await expect(page.locator('#ar-consent-form')).toHaveCount(0);
    expect(posted).toHaveLength(1);
    expect(posted[0]).toEqual({
      topic: 'consent-enforcement',
      email: 'operator@example.com',
      name: '',
      url: 'https://example.com/',
      wedge: 'consent-enforcement',
      lensSignal: {
        verdict: 'leaks',
        trackerCount: 4,
        thirdPartyCount: 5,
        topHosts: [
          { host: 'www.google-analytics.com', requests: 3, category: 'analytics' },
          { host: 'www.googletagmanager.com', requests: 1, category: 'tag-manager' },
          { host: 'connect.facebook.net', requests: 1, category: 'social' },
        ],
        cmpDetected: { detected: true, vendor: 'Cookiebot' },
      },
      message:
        '[consent-enforcement] lead from RenderPeek result\n' +
        'Scanned: https://example.com/\n' +
        'Signal: {"verdict":"leaks","trackerCount":4,"thirdPartyCount":5,"topHosts":[{"host":"www.google-analytics.com","requests":3,"category":"analytics"},{"host":"www.googletagmanager.com","requests":1,"category":"tag-manager"},{"host":"connect.facebook.net","requests":1,"category":"social"}],"cmpDetected":{"detected":true,"vendor":"Cookiebot"}}',
    });

    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toEqual([
      {
        name: 'lead_submit',
        data: {
          channel: 'scan',
          topic: 'consent-enforcement',
          wedge: 'consent-enforcement',
          source_path: '/ai-readability/',
        },
      },
    ]);
  });

  test('a clean page gets a neutral row and no card', async ({ page }) => {
    await stubUmami(page);
    await runScan(page, SCAN_CONSENT_CLEAN);

    const row = page.locator('.ar-lens', { hasText: 'Pre-consent leak' });
    await expect(row).toHaveCount(1);
    await expect(row).not.toHaveClass(/ar-finding/);
    await expect(row.locator('.ar-h4-r')).toHaveText('clean');
    await expect(row).toContainText(
      'No tracker requests observed before any interaction (1 third-party host(s) contacted, none in a tracker category).',
    );
    await expect(row.locator('.ar-chip-label')).toHaveText(
      '0 tracker hosts · 1 third-party host · consent banner: none detected',
    );
    await expect(row.locator('.ar-chip')).toHaveText(['fonts.googleapis.com · cdn']);
    await expect(page.locator('#ar-h-consent')).toHaveCount(0);
    await expect(page.locator('#ar-consent-form')).toHaveCount(0);
  });

  test('a disabled lens renders neither the row nor the card', async ({ page }) => {
    await stubUmami(page);
    await runScan(page, SCAN_CONSENT_DISABLED);

    await expect(page.locator('.ar-lens', { hasText: 'Pre-consent leak' })).toHaveCount(0);
    await expect(page.locator('#ar-h-consent')).toHaveCount(0);
    await expect(page.locator('#ar-consent-form')).toHaveCount(0);
  });

  test('a blocked render gets a muted not-measured row with the note and no card', async ({
    page,
  }) => {
    await stubUmami(page);
    await runScan(page, SCAN_CONSENT_BLOCKED);

    const row = page.locator('.ar-lens', { hasText: 'Pre-consent leak' });
    await expect(row).toHaveCount(1);
    await expect(row).not.toHaveClass(/ar-finding/);
    await expect(row.locator('.ar-h4-r')).toHaveText('not measured');
    await expect(row.locator('p.ar-def')).toHaveText(
      'The site answered our render with a challenge page, so we could not observe its requests.',
    );
    await expect(row.locator('.ar-chip-label')).toHaveCount(0);
    await expect(row.locator('.ar-chip')).toHaveCount(0);
    await expect(page.locator('#ar-h-consent')).toHaveCount(0);
    await expect(page.locator('#ar-consent-form')).toHaveCount(0);
  });

  test('an errored lens without a note gets the default not-measured line and no card', async ({
    page,
  }) => {
    await stubUmami(page);
    await runScan(page, SCAN_CONSENT_ERROR);

    const row = page.locator('.ar-lens', { hasText: 'Pre-consent leak' });
    await expect(row).toHaveCount(1);
    await expect(row.locator('.ar-h4-r')).toHaveText('not measured');
    await expect(row.locator('p.ar-def')).toHaveText('Not measured: the site blocked the scanner.');
    await expect(row.locator('.ar-chip-label')).toHaveCount(0);
    await expect(page.locator('#ar-h-consent')).toHaveCount(0);
    await expect(page.locator('#ar-consent-form')).toHaveCount(0);
  });

  test('a scan without the lens key renders neither the row nor the card', async ({ page }) => {
    await stubUmami(page);
    await runScan(page);

    await expect(page.locator('.ar-lens', { hasText: 'Pre-consent leak' })).toHaveCount(0);
    await expect(page.locator('#ar-h-consent')).toHaveCount(0);
    await expect(page.locator('#ar-consent-form')).toHaveCount(0);
  });
});

// Script-inventory lens: one row after the pre-consent row, and one card whose two
// submit buttons each post their own wedge.
const SI_TOTALS = {
  scripts: 14,
  inline: 3,
  external: 11,
  firstPartyExternal: 6,
  thirdPartyExternal: 5,
  thirdPartyHosts: 3,
  thirdPartyWithIntegrity: 1,
  nonExecutable: 0,
};
const SI_HOSTS = [
  {
    host: 'cdnjs.cloudflare.com',
    party: 'third',
    scripts: 3,
    withIntegrity: 0,
    category: 'cdn',
    listed: true,
  },
  {
    host: 'www.googletagmanager.com',
    party: 'third',
    scripts: 1,
    withIntegrity: 1,
    category: 'tag-manager',
    listed: true,
  },
  {
    host: 'widgets.example.net',
    party: 'third',
    scripts: 1,
    withIntegrity: 0,
    category: 'unlisted',
    listed: false,
  },
  {
    host: 'example.com',
    party: 'first',
    scripts: 6,
    withIntegrity: 0,
    category: 'first-party',
    listed: true,
  },
];
const SI_CSP = { enforced: false, reportOnly: true, scriptSrc: [], reporting: false };
const SI_ATTENTION = {
  status: 'ok',
  verdict: 'attention',
  reasons: ['sri-missing'],
  blockedBy: [],
  totals: SI_TOTALS,
  scriptHosts: SI_HOSTS,
  sriMissing: [
    { host: 'cdnjs.cloudflare.com', path: '/ajax/libs/lodash.js/4.17.21/lodash.min.js' },
  ],
  floating: [],
  duplicates: [],
  csp: SI_CSP,
  firstParty: ['example.com'],
  scriptListVersion: 'si-1',
  note: 'x',
};
const SI_CLEAN_UNPINNED = {
  ...SI_ATTENTION,
  verdict: 'clean',
  reasons: [],
  sriMissing: [],
  totals: { ...SI_TOTALS, thirdPartyExternal: 2, thirdPartyHosts: 2, thirdPartyWithIntegrity: 0 },
};
const SI_CLEAN_NONE = {
  ...SI_ATTENTION,
  verdict: 'clean',
  reasons: [],
  sriMissing: [],
  totals: { ...SI_TOTALS, thirdPartyExternal: 0, thirdPartyHosts: 0, thirdPartyWithIntegrity: 0 },
  scriptHosts: [SI_HOSTS[3]],
};
const siScan = (si?: unknown) => ({
  report: { ...SCAN_OK.report, ...(si ? { scriptInventory: si } : {}) },
});
const SI_ROW = (page: Page) => page.locator('.ar-lens', { hasText: 'Script inventory' });
const SI_PI_TEXT = [
  'PCI DSS 4.0 (requirement 6.4.3) asks payment pages to keep an inventory of the scripts they load and to authorise each one; requirement 11.6.1 asks for detection of unauthorised changes to those pages. If this is a payment page, those requirements may apply to you.',
  'We are building a server module that keeps a list of approved scripts at your origin.',
];
const SI_TF_TEXT = (n: number) =>
  `${n} third-party script(s) on this page load without an integrity hash, so their content can change upstream without notice. We are building a way to serve each one at a version you approved until you accept the update.`;

test.describe('AI-readability script-inventory lens and script-control card', () => {
  test('an attention page gets the row, both buttons, and each button posts its own wedge', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page, siScan(SI_ATTENTION));

    const row = SI_ROW(page);
    await expect(row).toHaveCount(1);
    await expect(row).toHaveClass(/ar-finding/);
    await expect(row.locator('.ar-h4-r')).toHaveText('attention');
    await expect(row).toContainText(
      '14 script(s) · 5 from other hosts · 1 with integrity · CSP: report-only.',
    );
    await expect(row.locator('.ar-chip')).toHaveText([
      'cdnjs.cloudflare.com · cdn',
      'www.googletagmanager.com · tag-manager',
      'widgets.example.net · unlisted',
    ]);
    await expect(row.locator('.ar-chip-block')).toHaveText(['cdnjs.cloudflare.com · cdn']);

    const card = page.locator('section[aria-labelledby="ar-h-scripts"]');
    await expect(page.locator('#ar-h-scripts')).toHaveText('Scripts you do not control');
    await expect(card.locator('p.ar-v')).toHaveText([
      ...SI_PI_TEXT,
      SI_TF_TEXT(4),
      'Tell us what you run.',
    ]);
    await expect(card.locator('a[href="/platform/"]')).toHaveText(
      'Origin modules we are building →',
    );
    await expect(card.locator('button[type="submit"]')).toHaveText([
      'Talk to us about script control',
      'Talk to us about pinning scripts',
    ]);
    await expect(card.locator('p.ar-disclosure')).toContainText(
      'We use them only for this request',
    );

    await page.fill('#ar-scripts-email', 'operator@example.com');
    await page.click('#ar-scripts-form button[data-wedge="third-party-freeze"]');
    await expect(page.locator('#ar-scripts-msg')).toHaveText(
      'Thanks. We will be in touch by email.',
    );
    await expect(page.locator('#ar-scripts-form')).toHaveCount(0);

    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'third-party-freeze',
      email: 'operator@example.com',
      url: 'https://example.com/',
      wedge: 'third-party-freeze',
      lensSignal: {
        verdict: 'attention',
        reasons: ['sri-missing'],
        totals: {
          scripts: 14,
          inline: 3,
          thirdPartyExternal: 5,
          thirdPartyHosts: 3,
          thirdPartyWithIntegrity: 1,
        },
        topHosts: [
          { host: 'cdnjs.cloudflare.com', scripts: 3, category: 'cdn' },
          { host: 'www.googletagmanager.com', scripts: 1, category: 'tag-manager' },
          { host: 'widgets.example.net', scripts: 1, category: 'unlisted' },
        ],
        duplicates: [],
        csp: SI_CSP,
      },
    });
    expect(JSON.stringify(posted[0])).not.toContain('lodash');

    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toEqual([
      {
        name: 'lead_submit',
        data: {
          channel: 'scan',
          topic: 'third-party-freeze',
          wedge: 'third-party-freeze',
          source_path: '/ai-readability/',
        },
      },
    ]);
  });

  test('the script control button posts the page-integrity wedge', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page, siScan(SI_ATTENTION));

    await page.fill('#ar-scripts-email', 'operator@example.com');
    await page.click('#ar-scripts-form button[data-wedge="page-integrity"]');
    await expect(page.locator('#ar-scripts-msg')).toHaveText(
      'Thanks. We will be in touch by email.',
    );
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'page-integrity',
      wedge: 'page-integrity',
      lensSignal: { verdict: 'attention', reasons: ['sri-missing'] },
    });
  });

  test('a clean page with unpinned third-party scripts shows only the pinning button', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page, siScan(SI_CLEAN_UNPINNED));

    const row = SI_ROW(page);
    await expect(row.locator('.ar-h4-r')).toHaveText('clean');
    await expect(row).not.toHaveClass(/ar-finding/);
    const card = page.locator('section[aria-labelledby="ar-h-scripts"]');
    await expect(card.locator('p.ar-v')).toHaveText([SI_TF_TEXT(2), 'Tell us what you run.']);
    await expect(card.locator('button[type="submit"]')).toHaveText([
      'Talk to us about pinning scripts',
    ]);

    await page.fill('#ar-scripts-email', 'operator@example.com');
    await page.click('#ar-scripts-form button[type="submit"]');
    await expect(page.locator('#ar-scripts-msg')).toHaveText(
      'Thanks. We will be in touch by email.',
    );
    expect(posted[0]).toMatchObject({ topic: 'third-party-freeze', wedge: 'third-party-freeze' });
  });

  test('a clean page with no third-party scripts shows the row and no card', async ({ page }) => {
    await stubUmami(page);
    await runScan(page, siScan(SI_CLEAN_NONE));

    await expect(SI_ROW(page).locator('.ar-h4-r')).toHaveText('clean');
    await expect(page.locator('#ar-h-scripts')).toHaveCount(0);
    await expect(page.locator('#ar-scripts-form')).toHaveCount(0);
  });

  test('a blocked render gets a muted not-measured row with the note and no card', async ({
    page,
  }) => {
    await stubUmami(page);
    await runScan(
      page,
      siScan({
        status: 'blocked',
        verdict: 'attention',
        reasons: [],
        totals: SI_TOTALS,
        note: 'The site answered our render with a challenge page, so we could not inventory its scripts.',
      }),
    );

    const row = SI_ROW(page);
    await expect(row).toHaveCount(1);
    await expect(row).not.toHaveClass(/ar-finding/);
    await expect(row.locator('.ar-h4-r')).toHaveText('not measured');
    await expect(row.locator('p.ar-def')).toHaveText(
      'The site answered our render with a challenge page, so we could not inventory its scripts.',
    );
    await expect(row.locator('.ar-chip')).toHaveCount(0);
    await expect(page.locator('#ar-h-scripts')).toHaveCount(0);
    await expect(page.locator('#ar-scripts-form')).toHaveCount(0);
  });

  test('an errored lens gets the default not-measured line and no card', async ({ page }) => {
    await stubUmami(page);
    await runScan(page, siScan({ status: 'error', reason: 'boom' }));

    const row = SI_ROW(page);
    await expect(row).toHaveCount(1);
    await expect(row.locator('.ar-h4-r')).toHaveText('not measured');
    await expect(row.locator('p.ar-def')).toHaveText('Not measured: the site blocked the scanner.');
    await expect(page.locator('#ar-h-scripts')).toHaveCount(0);
    await expect(page.locator('#ar-scripts-form')).toHaveCount(0);
  });

  test('a disabled lens renders neither the row nor the card', async ({ page }) => {
    await stubUmami(page);
    await runScan(page, siScan({ status: 'disabled', verdict: 'unknown', reasons: [] }));

    await expect(SI_ROW(page)).toHaveCount(0);
    await expect(page.locator('#ar-h-scripts')).toHaveCount(0);
  });

  test('a scan without the lens key renders neither the row nor the card', async ({ page }) => {
    await stubUmami(page);
    await runScan(page);

    await expect(SI_ROW(page)).toHaveCount(0);
    await expect(page.locator('#ar-h-scripts')).toHaveCount(0);
    await expect(page.locator('#ar-scripts-form')).toHaveCount(0);
  });
});

test.describe('AI-readability result link (copy replaces the email-me path)', () => {
  // The copy assertions read the clipboard back, which needs the grant.
  test.use({ permissions: ['clipboard-read', 'clipboard-write'] });

  test('a no-wedge result has no email field and copies the permalink, never /notify', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    // Any request to the scanner's /notify endpoint fails the test.
    const notifyRequests: string[] = [];
    page.on('request', (req) => {
      if (req.url().includes('/ai-readability/api/notify')) notifyRequests.push(req.url());
    });
    await runScan(page, SCAN_LINK);

    // The old no-wedge form WAS the /notify email path; it is gone entirely.
    await expect(page.locator('#ar-next-form')).toHaveCount(0);
    await expect(page.locator('#ar-next-email')).toHaveCount(0);
    await expect(page.locator('#ar-notify-btn')).toHaveCount(0);
    const card = page.locator('#ar-out');
    await expect(card).not.toContainText(/email me this result/i);
    await expect(card).not.toContainText(/emails you this result/i);
    // "we'll send" in either apostrophe spelling (the page copy uses ’).
    await expect(card).not.toContainText(/we.{1,2}ll send/i);

    // The share section shows the permalink and the copy control copies it.
    await expect(page.locator('.ar-share a')).toHaveAttribute('href', PERMALINK);
    await page.click('#ar-copy-link');
    await expect(page.locator('#ar-copy-msg')).toContainText('Link copied');
    expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(PERMALINK);

    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'airead-copy-link')).toHaveLength(1);
    expect(notifyRequests).toEqual([]);
    expect(posted).toHaveLength(0);
  });

  test('a fired wedge keeps its lead form and drops the secondary email-me button', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    const notifyRequests: string[] = [];
    page.on('request', (req) => {
      if (req.url().includes('/ai-readability/api/notify')) notifyRequests.push(req.url());
    });
    await runScan(page, SCAN_AGENTPASS_LINK);

    // The wedge lead form (email + "Talk to us" -> /contact) stays.
    await expect(page.locator('#ar-next-form')).toBeVisible();
    await expect(page.locator('#ar-next-email')).toBeVisible();
    // The old secondary notify button and its email promise are gone.
    await expect(page.locator('#ar-notify-btn')).toHaveCount(0);
    const section = page.locator('section[aria-labelledby="ar-h-next"]');
    await expect(section).not.toContainText(/email me this result/i);
    await expect(section).not.toContainText(/emails you this result/i);
    await expect(page.locator('#ar-out')).not.toContainText(/we.{1,2}ll send/i);

    await page.click('#ar-copy-link');
    await expect(page.locator('#ar-copy-msg')).toContainText('Link copied');
    expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(PERMALINK);
    expect(notifyRequests).toEqual([]);
    expect(posted).toHaveLength(0);
  });

  test('the two-wedge chooser offers only wedge topics, and the copy link is there', async ({
    page,
  }) => {
    await stubContact(page);
    await stubUmami(page);
    await runScan(page, SCAN_TWO_WEDGES_LINK);

    await expect(page.locator('input[name="ar-topic"][value="notify"]')).toHaveCount(0);
    expect(await page.locator('input[name="ar-topic"]').count()).toBe(2);
    await expect(page.locator('#ar-out')).not.toContainText(/email me this result/i);

    await page.click('#ar-copy-link');
    await expect(page.locator('#ar-copy-msg')).toContainText('Link copied');
    expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(PERMALINK);
  });

  test('when the clipboard API refuses, the permalink text is selected to copy manually', async ({
    page,
  }) => {
    await stubContact(page);
    await stubUmami(page);
    // Deterministic fallback: make the clipboard API reject before the page loads.
    await page.addInitScript(() => {
      Object.defineProperty(Navigator.prototype, 'clipboard', {
        configurable: true,
        get: () => ({
          writeText: () => Promise.reject(new Error('clipboard denied')),
          readText: () => Promise.reject(new Error('clipboard denied')),
        }),
      });
    });
    await runScan(page, SCAN_LINK);

    await page.click('#ar-copy-link');
    await expect(page.locator('#ar-copy-msg')).toContainText('Link selected');
    // The visible permalink text itself is selected, so Ctrl/Cmd+C copies it.
    expect(await page.evaluate(() => String(window.getSelection()))).toBe(PERMALINK);
    // The copy never completed, so no event fires.
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'airead-copy-link')).toHaveLength(0);
  });

  test('the loading to result swap keeps CLS at or under 0.02 at a realistic scan latency', async ({
    page,
  }) => {
    // A live scan takes 5–10 s, so the result swap lands outside the 500 ms
    // input-exclusion window; an instant stub would hide the shift. 1.5 s
    // clears the window while keeping the suite fast. 1440×1000 is the
    // viewport where the un-fixed shift measured worst (0.063).
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.addInitScript(() => {
      const w = window as unknown as { __cls: number };
      w.__cls = 0;
      new PerformanceObserver((list) => {
        for (const entry of list.getEntries()) {
          const shift = entry as PerformanceEntry & { hadRecentInput?: boolean; value?: number };
          if (!shift.hadRecentInput) w.__cls += shift.value ?? 0;
        }
      }).observe({ type: 'layout-shift', buffered: true });
    });
    await page.route('**/ai-readability/api/scan**', async (route) => {
      await new Promise((resolve) => setTimeout(resolve, 1500));
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(SCAN_LINK),
      });
    });
    await page.goto('/ai-readability/');
    await page.fill('#ar-url', 'https://example.com');
    await page.click('#ar-go');
    await expect(page.locator('#ar-watch-form')).toBeVisible({ timeout: 15000 });
    // Let any post-render shifts flush before reading the accumulator.
    await page.waitForTimeout(500);
    const cls = await page.evaluate(() => (window as unknown as { __cls: number }).__cls);
    expect(cls).toBeLessThanOrEqual(0.02);
  });

  test('a failed scan keeps CLS at or under 0.02 at a realistic latency', async ({ page }) => {
    // Same latency rationale as the result-swap test above, but the scanner
    // fails after 1.5 s: the error card is far smaller than the reserved
    // area, and without the hold the content below snaps back up mid-read
    // (measured up to 0.085 at these five viewports before the fix).
    await page.addInitScript(() => {
      const w = window as unknown as { __cls: number };
      w.__cls = 0;
      new PerformanceObserver((list) => {
        for (const entry of list.getEntries()) {
          const shift = entry as PerformanceEntry & { hadRecentInput?: boolean; value?: number };
          if (!shift.hadRecentInput) w.__cls += shift.value ?? 0;
        }
      }).observe({ type: 'layout-shift', buffered: true });
    });
    await page.route('**/ai-readability/api/scan**', async (route) => {
      await new Promise((resolve) => setTimeout(resolve, 1500));
      await route.fulfill({
        status: 500,
        contentType: 'application/json',
        body: '{"error":"scanner unavailable"}',
      });
    });
    for (const vp of [
      { width: 1440, height: 900 },
      { width: 1440, height: 1000 },
      { width: 1440, height: 1200 },
      { width: 1920, height: 960 },
      { width: 1536, height: 730 },
    ]) {
      await page.setViewportSize(vp);
      await page.goto('/ai-readability/');
      await page.fill('#ar-url', 'https://example.com');
      await page.click('#ar-go');
      await expect(page.locator('#ar-out .ar-err')).toBeVisible({ timeout: 15000 });
      // The error keeps the reserved area until the visitor edits the URL.
      await expect(page.locator('#ar-out')).toHaveAttribute('data-hold', '1');
      // ...but only down to the fold, or to the card itself when the card
      // already reaches past the fold: no blank space is left below either.
      const box = await page.locator('#ar-out').boundingBox();
      const card = await page.locator('#ar-out .ar-err').boundingBox();
      expect(
        box!.y + box!.height,
        `hold ends at the fold at ${vp.width}x${vp.height}`,
      ).toBeLessThanOrEqual(Math.max(vp.height, card!.y + card!.height) + 1);
      // Let any post-render shifts flush before reading the accumulator.
      await page.waitForTimeout(500);
      const cls = await page.evaluate(() => (window as unknown as { __cls: number }).__cls);
      expect(cls, `CLS at ${vp.width}x${vp.height}`).toBeLessThanOrEqual(0.02);
      // Editing the URL releases the hold; that collapse follows a
      // keystroke, so it is input-excluded and never counted.
      await page.fill('#ar-url', 'https://example.org');
      await expect(page.locator('#ar-out')).not.toHaveAttribute('data-hold');
    }
  });
});

test.describe('PageSpeed analyzer report download', () => {
  test('Download this report saves a Markdown file with the result, no network', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runAnalysis(page);

    // No email capture remains on the result.
    await expect(page.locator('#psi-report-form')).toHaveCount(0);
    await expect(page.locator('#psi-report-email')).toHaveCount(0);

    // "No network" is checked, not assumed: between the click and the
    // completed download, no request may carry report content out (umami is
    // stubbed in-page, so even the analytics event stays local). Static
    // chrome assets can still settle during the window; a non-GET request or
    // report text in any URL/body fails the test.
    const leaks: string[] = [];
    page.on('request', (req) => {
      const sent = req.url() + '\n' + (req.postData() ?? '');
      if (req.method() !== 'GET' || /PageSpeed report|render-blocking/.test(sent)) {
        leaks.push(`${req.method()} ${req.url()}`);
      }
    });
    const downloadPromise = page.waitForEvent('download');
    await page.click('#psi-report-download');
    const download = await downloadPromise;

    expect(download.suggestedFilename()).toBe('pagespeed-report-example.com.md');
    const path = await download.path();
    expect(path).toBeTruthy();
    const content = await readFile(path!, 'utf8');
    expect(content).toContain('Analyzed: https://example.com/');
    expect(content).toMatch(/When: \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}/);
    expect(content).toContain('mobile 90/100');
    expect(content).toContain('desktop 90/100');
    expect(content).toContain('Eliminate render-blocking resources');

    // The download posts nothing to the contact endpoint and no request
    // carries report content — the Blob save is fully client-side.
    expect(posted).toHaveLength(0);
    expect(leaks, `requests carrying report content: ${leaks.join(', ')}`).toEqual([]);
    // …and fires exactly one analyze_report_download per click.
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'analyze_report_download')).toHaveLength(1);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(0);
  });

  test('the result keeps the install CTAs and the consulting CTA as a statement', async ({
    page,
  }) => {
    await stubContact(page);
    await runAnalysis(page);

    const ctas = page.locator('#psi-ctas');
    await expect(ctas.locator('a[href="/download/"]').first()).toBeVisible();
    const consulting = ctas.locator('a[href="https://we-amp.com/consulting/"]');
    await expect(consulting).toBeVisible();
    await expect(consulting).toContainText('Have us fix it for you');
    await expect(consulting).not.toContainText('?');
    await expect(consulting).toHaveAttribute('data-umami-event', 'cta_commercial');
    await expect(consulting).toHaveAttribute('data-umami-event-offer', 'consulting');
    await expect(consulting).toHaveAttribute('data-umami-event-surface', 'analyze');
    await expect(consulting).toHaveAttribute('data-umami-event-position', 'result');
  });
});
