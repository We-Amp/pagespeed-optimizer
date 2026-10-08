// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import type { Page, Route } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// Lead capture on the free tools (tranche T4a): the AI-readability checker
// (/ai-readability/) and the PageSpeed analyzer (/analyze/). Both post to the
// shared lead endpoint /ai-readability/api/contact; every successful submit
// fires umami lead_submit with {channel, topic, wedge, source_path}.

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

// Minimal PSI response: one score per strategy, no flagged audits.
const PSI_OK = {
  lighthouseResult: {
    categories: { performance: { score: 0.9 } },
    audits: {},
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

async function runScan(page: Page) {
  await page.route('**/ai-readability/api/scan**', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(SCAN_OK),
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
  await expect(page.locator('#psi-report-form')).toBeVisible({ timeout: 15000 });
}

test.describe('AI-readability checker lead capture', () => {
  test('Watch this URL posts monitor-url with the scanned URL and grade', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.fill('#ar-watch-email', 'watcher@example.com');
    await page.check('#ar-watch-consent');
    await page.click('#ar-watch-form button[type="submit"]');

    await expect(page.locator('#ar-watch-msg')).toContainText('Thanks');
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'monitor-url',
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
          topic: 'monitor-url',
          wedge: '',
          source_path: '/ai-readability/',
        },
      },
    ]);
  });

  test('the consent checkbox is required for Watch this URL', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.fill('#ar-watch-email', 'watcher@example.com');
    await page.click('#ar-watch-form button[type="submit"]');
    // Native validation blocks the submit; nothing is posted, no lead fires.
    await expect(page.locator('#ar-watch-msg')).toHaveText('');
    expect(posted).toHaveLength(0);
  });

  test('the agents form posts topic agents with the chosen need as wedge', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.selectOption('#ar-agents-need', 'provenance');
    await page.fill('#ar-agents-email', 'operator@example.com');
    await page.fill('#ar-agents-log', 'GPTBot: 12000/mo, ClaudeBot: 8000/mo');
    await page.click('#ar-agents-form button[type="submit"]');

    await expect(page.locator('#ar-agents-msg')).toContainText('Thanks');
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'agents',
      email: 'operator@example.com',
      wedge: 'provenance',
      url: 'https://example.com/',
    });
    expect(String(posted[0].message)).toContain('Need: Provenance of served content');
    expect(String(posted[0].message)).toContain('GPTBot: 12000/mo, ClaudeBot: 8000/mo');

    const events = await trackedEvents(page);
    const leads = events.filter((e) => e.name === 'lead_submit');
    expect(leads).toEqual([
      {
        name: 'lead_submit',
        data: {
          channel: 'scan',
          topic: 'agents',
          wedge: 'provenance',
          source_path: '/ai-readability/',
        },
      },
    ]);
  });

  test('the operator log is optional and capped at 1500 characters', async ({ page }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runScan(page);

    await page.selectOption('#ar-agents-need', 'signed-agent-verification');
    await page.fill('#ar-agents-email', 'operator@example.com');
    await page.click('#ar-agents-form button[type="submit"]');
    await expect(page.locator('#ar-agents-msg')).toContainText('Thanks');
    expect(String(posted[0].message)).not.toContain('AI-crawler request counts');
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
    expect(posted).toHaveLength(1);
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(0);
  });
});

test.describe('PageSpeed analyzer lead capture', () => {
  test('Email me this report posts analyze-report with URL and headline scores', async ({
    page,
  }) => {
    const posted = await stubContact(page);
    await stubUmami(page);
    await runAnalysis(page);

    await page.fill('#psi-report-email', 'ops@example.com');
    await page.click('#psi-report-submit');

    await expect(page.locator('#psi-report-msg')).toContainText('Thanks');
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({ topic: 'analyze-report', email: 'ops@example.com' });
    expect(String(posted[0].message)).toContain('Analyzed: https://example.com/');
    expect(String(posted[0].message)).toContain('mobile 90/100');
    expect(String(posted[0].message)).toContain('desktop 90/100');

    const events = await trackedEvents(page);
    const leads = events.filter((e) => e.name === 'lead_submit');
    expect(leads).toEqual([
      {
        name: 'lead_submit',
        data: {
          channel: 'analyze',
          topic: 'analyze-report',
          wedge: '',
          source_path: '/analyze/',
        },
      },
    ]);
  });

  test('a 500 from the contact endpoint shows the error path and fires no lead', async ({
    page,
  }) => {
    const posted = await stubContact(page, 500);
    await stubUmami(page);
    await runAnalysis(page);

    await page.fill('#psi-report-email', 'ops@example.com');
    await page.click('#psi-report-submit');

    await expect(page.locator('#psi-report-msg')).toContainText('couldn’t send');
    expect(posted).toHaveLength(1);
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(0);
  });

  test('the result keeps the install CTAs and adds the consulting CTA', async ({ page }) => {
    await stubContact(page);
    await runAnalysis(page);

    const ctas = page.locator('#psi-ctas');
    await expect(ctas.locator('a[href="/download/"]').first()).toBeVisible();
    const consulting = ctas.locator('a[href="https://we-amp.com/consulting/"]');
    await expect(consulting).toBeVisible();
    await expect(consulting).toContainText('Want it fixed for you?');
    await expect(consulting).toHaveAttribute('data-umami-event', 'cta_commercial');
    await expect(consulting).toHaveAttribute('data-umami-event-offer', 'consulting');
    await expect(consulting).toHaveAttribute('data-umami-event-surface', 'analyze');
    await expect(consulting).toHaveAttribute('data-umami-event-position', 'result');
  });
});
