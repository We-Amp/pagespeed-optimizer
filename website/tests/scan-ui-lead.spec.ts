// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect, type Page } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// The v2 lead form. The scanner, PageSpeed Insights and the contact endpoint
// are mocked; the real services are never called.

const fixture = (name: string) =>
  readFileSync(new URL(`./fixtures/scan/report-${name}.json`, import.meta.url), 'utf8');

type Posted = Array<Record<string, unknown>>;

async function mock(
  page: Page,
  opts: { report?: 'full' | 'clean' | object; psi?: number; failTopics?: string[] } = {},
): Promise<{ posted: Posted; failTopics: string[] }> {
  const posted: Posted = [];
  const failTopics = [...(opts.failTopics ?? [])];
  await page.route('**/psi/v5/runPagespeed**', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        lighthouseResult: { categories: { performance: { score: opts.psi ?? 0.9 } } },
      }),
    }),
  );
  await page.route('**/ai-readability/api/scan**', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body:
        typeof opts.report === 'object'
          ? JSON.stringify(opts.report)
          : fixture(opts.report ?? 'full'),
    }),
  );
  await page.route('**/ai-readability/api/contact', async (route) => {
    const body = route.request().postDataJSON() as Record<string, unknown>;
    posted.push(body);
    const fail = failTopics.includes(String(body.topic));
    await route.fulfill({
      status: fail ? 500 : 200,
      contentType: 'application/json',
      body: fail ? '{"error":"nope"}' : '{"ok":true}',
    });
  });
  return { posted, failTopics };
}

async function scanV2(page: Page, path = '/ai-readability/?ui=v2') {
  await stubUmami(page);
  await page.goto(path);
  await page.waitForLoadState('networkidle');
  await page.fill('#scan-url', 'shop.example.com');
  await page.click('#scan-submit');
  await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).not.toHaveText(
    'Checking…',
    { timeout: 15000 },
  );
  await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).not.toHaveText(
    'Checking…',
    { timeout: 15000 },
  );
}

const form = (page: Page) => page.locator('form[data-scan-lead]');
const chip = (page: Page, id: string) => page.locator(`[data-scan-chip="${id}"] input`);
const chipIds = (page: Page) =>
  page
    .locator('[data-scan-chip]')
    .evaluateAll((els) => els.map((e) => (e as HTMLElement).dataset.scanChip));
const checkedIds = (page: Page) =>
  page
    .locator('[data-scan-chip] input:checked')
    .evaluateAll((els) => els.map((e) => (e as HTMLInputElement).value));

const ALL = [
  'tollbooth',
  'agentpass',
  'compliancefix',
  'consent-enforcement',
  'page-integrity',
  'third-party-freeze',
  'edge-seo',
  'response-firewall',
  'consulting',
  'agents',
];

test.describe('the one lead form', () => {
  test('shows every gated chip on the full report, pre-selecting the first four', async ({
    page,
  }) => {
    await mock(page);
    await scanV2(page);
    await expect(form(page)).toBeVisible();
    await expect(form(page).locator('fieldset legend')).toHaveText('Topics');
    expect(await chipIds(page)).toEqual(ALL);
    expect(await checkedIds(page)).toEqual(ALL.slice(0, 4));
    await expect(form(page).locator('input[type="checkbox"]')).toHaveCount(10);
    await expect(page.locator('[data-scan-live]')).toHaveCount(1);
    await expect(
      page.locator('#scan-results [aria-live], #scan-results [role="status"]'),
    ).toHaveCount(1);
  });

  test('shows only the ungated chips on a clean report, none selected', async ({ page }) => {
    await mock(page, { report: 'clean' });
    await scanV2(page);
    expect(await chipIds(page)).toEqual(['consulting', 'agents']);
    expect(await checkedIds(page)).toEqual([]);
  });

  test('pre-selects the speed-help chip only when Speed is Poor', async ({ page }) => {
    await mock(page, { report: 'clean', psi: 0.3 });
    await scanV2(page);
    expect(await checkedIds(page)).toEqual(['consulting']);
  });

  test('still renders with the ungated chips when the scan fails', async ({ page }) => {
    await mock(page);
    await page.route('**/ai-readability/api/scan**', (route) => route.abort());
    await scanV2(page);
    await expect(form(page)).toBeVisible();
    expect(await chipIds(page)).toEqual(['consulting', 'agents']);
  });

  test('asks for a topic, and for at most four', async ({ page }) => {
    await mock(page, { report: 'clean' });
    await scanV2(page);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick at least one topic.');

    const { posted } = await mock(page);
    await scanV2(page);
    await chip(page, 'edge-seo').check();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick up to four topics.');
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick up to four topics.');
    expect(posted).toHaveLength(0);
    await chip(page, 'tollbooth').uncheck();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');
  });

  test('shows the sites field only for the SEO and legacy-site topics', async ({ page }) => {
    await mock(page);
    await scanV2(page);
    const sites = page.getByLabel('Sites you run (optional)');
    await expect(sites).toBeHidden();
    await chip(page, 'tollbooth').uncheck();
    await chip(page, 'edge-seo').check();
    await expect(sites).toBeVisible();
    await chip(page, 'edge-seo').uncheck();
    await expect(sites).toBeHidden();
    await chip(page, 'response-firewall').check();
    await expect(sites).toBeVisible();
  });

  test('sends one lead per topic, in order, with the right wedge, note and events', async ({
    page,
  }) => {
    const { posted } = await mock(page);
    await scanV2(page);
    await chip(page, 'compliancefix').uncheck();
    await chip(page, 'consent-enforcement').uncheck();
    await chip(page, 'edge-seo').check();
    await chip(page, 'consulting').check();
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByLabel('Sites you run (optional)').fill('12');
    await page.getByLabel('Anything we should know? (optional)').fill('WordPress');
    await page.getByRole('button', { name: 'Talk to us' }).click();

    await expect(page.getByText('Thanks — your answer is in.', { exact: true })).toBeVisible();
    await expect(page.getByText(/^Topics sent: /)).toContainText('SEO fixes at the server');
    await expect(form(page)).toBeHidden();
    expect(posted.map((p) => p.topic)).toEqual([
      'tollbooth',
      'agentpass',
      'edge-seo',
      'consulting',
    ]);
    expect(posted.map((p) => p.wedge)).toEqual(['tollbooth', 'agentpass', 'edge-seo', '']);
    expect(String(posted[2].message)).toContain(
      'Operator note / crawl volume: sites: 12; WordPress',
    );
    expect(String(posted[0].message)).toContain('Operator note / crawl volume: WordPress');
    expect(String(posted[0].message)).not.toContain('sites: 12');
    expect(posted[3]).toMatchObject({
      topic: 'consulting',
      wedge: '',
      name: '',
      email: 'ops@example.com',
      url: 'https://shop.example.com/',
    });
    expect(String(posted[3].message)).toContain('[consulting] request from the scan result');

    const events = await trackedEvents(page);
    const leads = events.filter((e) => e.name === 'lead_submit');
    expect(leads.map((e) => e.data)).toEqual(
      ['tollbooth', 'agentpass', 'edge-seo', 'consulting'].map((t) => ({
        channel: 'scan',
        topic: t,
        wedge: t === 'consulting' ? '' : t,
        source_path: '/ai-readability/',
      })),
    );
    const next = events.filter((e) => e.name.startsWith('airead-next-')).map((e) => e.name);
    expect(next).toEqual([
      'airead-next-tollbooth',
      'airead-next-agentpass',
      'airead-next-edge-seo',
    ]);
  });

  test('retries only the topics that failed, without counting twice', async ({ page }) => {
    const mocked = await mock(page, { failTopics: ['agentpass'] });
    await scanV2(page);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText(
      'We could not send: Signed-agent verification (prove which agent is which). Try again.',
    );
    await expect(form(page)).toBeVisible();
    expect(await checkedIds(page)).toEqual(['agentpass']);
    expect(mocked.posted.map((p) => p.topic)).toEqual([
      'tollbooth',
      'agentpass',
      'compliancefix',
      'consent-enforcement',
    ]);

    mocked.failTopics.length = 0;
    mocked.posted.length = 0;
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.getByText('Thanks — your answer is in.', { exact: true })).toBeVisible();
    expect(mocked.posted.map((p) => p.topic)).toEqual(['agentpass']);
    await expect(page.getByText(/^Topics sent: /)).toContainText('Origin controls');
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(4);
    expect(events.filter((e) => e.name === 'airead-next-agentpass')).toHaveLength(1);
  });

  test('adds the speed-help chip when the speed result arrives, keeping the visitor’s choices', async ({
    page,
  }) => {
    await mock(page, { report: 'clean' });
    let release!: () => void;
    const gate = new Promise<void>((r) => (release = r));
    await page.route('**/psi/v5/runPagespeed**', async (route) => {
      await gate;
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ lighthouseResult: { categories: { performance: { score: 0.2 } } } }),
      });
    });
    await stubUmami(page);
    await page.goto('/ai-readability/?ui=v2');
    await page.waitForLoadState('networkidle');
    await page.fill('#scan-url', 'shop.example.com');
    await page.click('#scan-submit');
    await expect(form(page)).toBeVisible({ timeout: 15000 });
    expect(await chipIds(page)).toEqual(['agents']);
    await chip(page, 'agents').check();
    release();
    await expect(chip(page, 'consulting')).toBeChecked();
    expect(await chipIds(page)).toEqual(['consulting', 'agents']);
    await expect(chip(page, 'agents')).toBeChecked();
  });
});

test.describe('next steps and share', () => {
  test('carry the surface’s events, the permalink and the extended report', async ({ page }) => {
    await mock(page, { report: JSON.parse(fixture('full')) });
    await scanV2(page);
    const next = page.locator('[data-scan-next]');
    await expect(next.getByRole('link', { name: /Download & run/ })).toHaveAttribute(
      'data-umami-event',
      'airead-cta-optimize',
    );
    await expect(next.getByRole('link', { name: /Support plans/ })).toHaveAttribute(
      'data-umami-event',
      'airead-cta-buy',
    );
    await expect(page.locator('[data-scan-permalink]')).toHaveText(
      'https://modpagespeed.com/ai-readability/api/r/a1b2c3d4e5f6',
    );
    await expect(page.locator('[data-scan-share]')).toContainText(
      'The link works for up to 24 hours.',
    );
    await expect(page.getByRole('button', { name: 'Copy link to this result' })).toBeVisible();

    const [download] = await Promise.all([
      page.waitForEvent('download'),
      page.getByRole('button', { name: 'Download this report' }).click(),
    ]);
    expect(download.suggestedFilename()).toBe('pagespeed-report-shop.example.com.md');
    const body = readFileSync((await download.path())!, 'utf8');
    expect(body).toContain('AI readability: grade C (64/100)');
    expect(body).toContain('- Pre-consent leak: attention');
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'analyze_report_download')).toHaveLength(1);
  });

  test('use the analyzer’s events on the analyzer page', async ({ page }) => {
    await mock(page);
    await scanV2(page, '/analyze/?ui=v2');
    const next = page.locator('[data-scan-next]');
    await expect(next.getByRole('link', { name: /Download & run/ })).toHaveAttribute(
      'data-umami-event',
      'psi-result-install-cta',
    );
    const support = next.getByRole('link', { name: /Support plans/ });
    await expect(support).toHaveAttribute('data-umami-event', 'cta_commercial');
    await expect(support).toHaveAttribute('data-umami-event-surface', 'analyze');
  });
});

test.describe('v1 and v2 send the same lead', () => {
  // Every wedge the original page offers, on the full report. Each case fills
  // the original form, then the new one with only that topic ticked, and the
  // two POST bodies must be identical.
  const cases: Array<{ wedge: string; sites?: string; note?: string }> = [
    { wedge: 'tollbooth' },
    { wedge: 'agentpass' },
    { wedge: 'compliancefix' },
    { wedge: 'consent-enforcement' },
    { wedge: 'page-integrity' },
    { wedge: 'third-party-freeze' },
    { wedge: 'edge-seo', sites: '12' },
    { wedge: 'edge-seo', sites: '12', note: 'WordPress' },
    { wedge: 'edge-seo', note: 'WordPress' },
  ];

  for (const c of cases) {
    const name = `${c.wedge}${c.sites ? ' with sites' : ''}${c.note ? ' with a note' : ' and an empty note'}`;
    test(`${name}: the same body in both`, async ({ page }) => {
      const v1 = await mock(page);
      await stubUmami(page);
      await page.goto('/ai-readability/?ui=v1');
      await page.fill('#ar-url', 'shop.example.com');
      await page.click('#ar-go');
      const email = 'operator@example.com';
      if (c.wedge === 'edge-seo') {
        await page.fill('#ar-seo-email', email);
        if (c.sites) await page.fill('#ar-seo-sites', c.sites);
        if (c.note) await page.fill('#ar-seo-note', c.note);
        await page.click('#ar-seo-form button[type="submit"]');
      } else if (c.wedge === 'consent-enforcement') {
        await page.fill('#ar-consent-email', email);
        await page.click('#ar-consent-form button[type="submit"]');
      } else if (c.wedge === 'page-integrity' || c.wedge === 'third-party-freeze') {
        await page.fill('#ar-scripts-email', email);
        await page.click(`#ar-scripts-form button[data-wedge="${c.wedge}"]`);
      } else {
        await page.fill('#ar-next-email', email);
        await page.check(`input[name="ar-topic"][value="${c.wedge}"]`);
        await page.click('#ar-next-form button[type="submit"]');
      }
      await expect.poll(() => v1.posted.length, { timeout: 15000 }).toBe(1);

      const p2 = await page.context().newPage();
      const v2 = await mock(p2);
      await scanV2(p2);
      for (const id of await checkedIds(p2)) await chip(p2, id).uncheck();
      await chip(p2, c.wedge).check();
      await p2.fill('input[name="email"]', email);
      if (c.sites) await p2.getByLabel('Sites you run (optional)').fill(c.sites);
      if (c.note) await p2.getByLabel('Anything we should know? (optional)').fill(c.note);
      await p2.getByRole('button', { name: 'Talk to us' }).click();
      await expect(p2.getByText('Thanks — your answer is in.', { exact: true })).toBeVisible();
      expect(v2.posted).toHaveLength(1);
      expect(v2.posted[0]).toEqual(v1.posted[0]);
    });
  }
});
