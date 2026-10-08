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
// either page promises a re-check or an emailed report.

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
