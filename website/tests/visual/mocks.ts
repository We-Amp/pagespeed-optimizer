// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Deterministic network for the visual suite: the scan API, both PageSpeed
// Insights strategies and the contact endpoint are answered from the fixtures
// in tests/fixtures/scan, with a per-source delay or a manual gate so the
// loading and partial states are captured on purpose.

import { readFileSync } from 'node:fs';
import type { Page } from '@playwright/test';

const FIXTURES = new URL('../fixtures/scan/', import.meta.url);
const read = (name: string): string => readFileSync(new URL(name, FIXTURES), 'utf8');

export type ScanFixture = 'full' | 'clean' | 'blocked';

/** What one mocked source answers. */
export interface SourceMock {
  /** HTTP status; default 200. */
  status?: number;
  /** Response body as an object (serialised) or a raw string; default is the fixture for the source. */
  body?: unknown;
  /** Milliseconds to wait before answering; default 0. */
  delayMs?: number;
  /** Hold the response until `mocks.release(source)` is called (overrides delayMs). */
  gate?: boolean;
  /** Fail the request at the network level (no response). */
  abort?: boolean;
}

export interface MockOptions {
  scan?: SourceMock & { fixture?: ScanFixture };
  psiMobile?: SourceMock;
  psiDesktop?: SourceMock;
  contact?: SourceMock;
}

export type Source = 'scan' | 'psiMobile' | 'psiDesktop' | 'contact';

export interface Mocks {
  /** Answer a gated source now. */
  release: (source: Source) => void;
  /** How many requests each source has seen. */
  calls: Record<Source, number>;
  /** Contact POST bodies, in arrival order. */
  contactBodies: unknown[];
}

const FIXED_TIME = new Date('2026-10-10T12:00:00.000Z');

/**
 * Freeze Date, hide the dev-server toolbar, refuse every request that is not
 * to the site under test, and await fonts. Call before page.goto().
 */
export async function makeDeterministic(page: Page): Promise<void> {
  await page.clock.setFixedTime(FIXED_TIME);
  await page.route(
    (url) => !['localhost', '127.0.0.1'].includes(url.hostname),
    (route) => route.abort(),
  );
  await page.addInitScript(() => {
    const style = document.createElement('style');
    style.textContent =
      'astro-dev-toolbar{display:none!important}*,*::before,*::after{caret-color:transparent!important}';
    document.addEventListener('DOMContentLoaded', () => document.head.appendChild(style));
  });
}

/** Wait for web fonts and a settled layout before a screenshot. */
export async function settle(page: Page): Promise<void> {
  await page.evaluate(() => document.fonts.ready);
  await page.evaluate(() => new Promise<void>((r) => requestAnimationFrame(() => r())));
}

/** Install the scan, PSI and contact mocks. Later calls replace earlier ones. */
export async function installMocks(page: Page, options: MockOptions = {}): Promise<Mocks> {
  const gates = new Map<Source, () => void>();
  const calls: Record<Source, number> = { scan: 0, psiMobile: 0, psiDesktop: 0, contact: 0 };
  const contactBodies: unknown[] = [];

  const wait = async (source: Source, mock: SourceMock) => {
    if (mock.gate) {
      await new Promise<void>((resolve) => gates.set(source, resolve));
    } else if (mock.delayMs) {
      await new Promise((resolve) => setTimeout(resolve, mock.delayMs));
    }
  };
  const bodyOf = (mock: SourceMock, fallback: string): string =>
    mock.body === undefined
      ? fallback
      : typeof mock.body === 'string'
        ? mock.body
        : JSON.stringify(mock.body);

  await page.route('**/ai-readability/api/scan**', async (route) => {
    calls.scan++;
    const mock = options.scan ?? {};
    await wait('scan', mock);
    if (mock.abort) return route.abort();
    await route.fulfill({
      status: mock.status ?? 200,
      contentType: 'application/json',
      body: bodyOf(mock, read(`report-${mock.fixture ?? 'full'}.json`)),
    });
  });

  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    const strategy = new URL(route.request().url()).searchParams.get('strategy');
    const source: Source = strategy === 'desktop' ? 'psiDesktop' : 'psiMobile';
    calls[source]++;
    const mock = options[source] ?? {};
    await wait(source, mock);
    if (mock.abort) return route.abort();
    const status = mock.status ?? 200;
    const fallback = read(
      status === 429
        ? 'psi-429.json'
        : strategy === 'desktop'
          ? 'psi-desktop.json'
          : 'psi-mobile.json',
    );
    await route.fulfill({
      status,
      contentType: 'application/json',
      body: bodyOf(mock, fallback),
    });
  });

  await page.route('**/ai-readability/api/contact**', async (route) => {
    calls.contact++;
    const mock = options.contact ?? {};
    try {
      contactBodies.push(route.request().postDataJSON());
    } catch {
      contactBodies.push(route.request().postData());
    }
    await wait('contact', mock);
    if (mock.abort) return route.abort();
    await route.fulfill({
      status: mock.status ?? 200,
      contentType: 'application/json',
      body: bodyOf(mock, '{"ok":true}'),
    });
  });

  return {
    release: (source) => gates.get(source)?.(),
    calls,
    contactBodies,
  };
}
