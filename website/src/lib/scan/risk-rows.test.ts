// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { RX_BUILDING, riskRows } from './risk-rows';

const load = (name: string) =>
  JSON.parse(readFileSync(`tests/fixtures/scan/report-${name}.json`, 'utf8')).report;

const byKey = (rows: ReturnType<typeof riskRows>) =>
  Object.fromEntries(rows.map((r) => [r.key, r]));

describe('riskRows over the full report', () => {
  const rows = riskRows(load('full'));
  const m = byKey(rows);
  it('lists the four lenses in order', () => {
    expect(rows.map((r) => r.key)).toEqual([
      'preConsentLeak',
      'scriptInventory',
      'seoDefects',
      'responseExposure',
    ]);
    expect(rows.map((r) => r.name)).toEqual([
      'Pre-consent leak',
      'Script inventory',
      'SEO defects',
      'Response exposure',
    ]);
  });
  it('flags every lens and carries the gate-fired link with its event', () => {
    expect(rows.every((r) => r.state === 'attention' && r.chip === 'Attention')).toBe(true);
    expect(m.preConsentLeak.link).toMatchObject({
      href: '/platform/consent/',
      event: 'airead-cta-consent',
    });
    expect(m.scriptInventory.link).toMatchObject({
      href: '/platform/',
      event: 'airead-cta-page-integrity',
    });
    expect(m.seoDefects.link).toMatchObject({
      href: '/platform/edge-seo/',
      event: 'airead-cta-edge-seo',
    });
    expect(m.responseExposure.link).toMatchObject({
      href: '/platform/#pack-response-firewall',
      event: 'airead-cta-response-firewall',
    });
    expect(m.responseExposure.building).toEqual([RX_BUILDING]);
  });
  it('uses the approved verdict sentences', () => {
    expect(m.preConsentLeak.sentence).toMatch(
      /^\d+ tracker host\(s\) contacted before any interaction, in a fresh browser with no consent given: /,
    );
    expect(m.preConsentLeak.sentence).toMatch(/not legal advice or a compliance certificate\.$/);
    expect(m.scriptInventory.sentence).toMatch(
      /^27 script\(s\) · 13 from other hosts · 1 with integrity/,
    );
    expect(m.seoDefects.sentence).toMatch(
      /^3 issue\(s\): canonical 1, title 1, structured data 1\. /,
    );
    expect(m.responseExposure.sentence).toBe(
      '1 error-output pattern(s) · 2 version tell(s) · end of life: PHP 7.4 · missing: nosniff, framing. One page only; not a security assessment.',
    );
  });
  it('names duplicate libraries and versions', () => {
    expect(m.scriptInventory.sentence).toContain('jquery 1.12.4 + 3.6.0');
  });
  it('gives the script row both building sentences when both gates fire', () => {
    expect(m.scriptInventory.building).toHaveLength(2);
    expect(m.scriptInventory.building[0]).toBe(
      'We are building a server module that keeps a list of approved scripts at your origin.',
    );
  });
});

describe('riskRows over the clean report', () => {
  const rows = riskRows(load('clean'));
  it('shows clean rows with no building sentence and no link', () => {
    expect(rows).toHaveLength(4);
    for (const r of rows) {
      expect(r.state).toBe('clean');
      expect(r.chip).toBe('Clean');
      expect(r.building).toEqual([]);
      expect(r.link).toBeNull();
    }
  });
  it('uses the approved clean sentences', () => {
    const m = byKey(rows);
    expect(m.preConsentLeak.sentence).toMatch(
      /^No tracker requests observed before any interaction/,
    );
    expect(m.seoDefects.sentence).toMatch(/^No defects found: the canonical/);
  });
});

describe('riskRows over the blocked report', () => {
  const m = byKey(riskRows(load('blocked')));
  it('shows muted not-measured rows with the note, or the default', () => {
    for (const key of ['preConsentLeak', 'scriptInventory', 'seoDefects']) {
      expect(m[key].state).toBe('none');
      expect(m[key].chip).toBe('Not measured');
      expect(m[key].link).toBeNull();
      expect(m[key].building).toEqual([]);
    }
    // the error shape has a reason and no note
    expect(m.responseExposure.state).toBe('none');
    expect(m.responseExposure.sentence).toBe('Not measured: the site blocked the scanner.');
  });
});

describe('riskRows edge cases', () => {
  const rx = (extra: object) => ({ responseExposure: { status: 'ok', ...extra } });
  it('omits disabled and missing lenses', () => {
    const rows = riskRows({
      preConsentLeak: { status: 'disabled', verdict: 'unknown' },
      scriptInventory: {
        status: 'ok',
        verdict: 'clean',
        totals: { thirdPartyExternal: 0, thirdPartyWithIntegrity: 0 },
      },
    });
    expect(rows.map((r) => r.key)).toEqual(['scriptInventory']);
    expect(riskRows(null)).toEqual([]);
  });
  it('uses the could-not-render sentence for an unknown verdict', () => {
    const m = byKey(
      riskRows({
        preConsentLeak: { status: 'ok', verdict: 'unknown' },
        scriptInventory: { status: 'ok', verdict: 'unknown' },
        seoDefects: { status: 'ok', verdict: 'unknown', staticCompared: true, notMeasured: [] },
        ...rx({ verdict: 'unknown' }),
      }),
    );
    expect(m.preConsentLeak.sentence).toBe(
      'We could not render this page, so we could not observe which third parties it contacts before consent.',
    );
    expect(m.scriptInventory.sentence).toBe(
      'We could not render this page, so we could not inventory its scripts.',
    );
    expect(m.seoDefects.sentence).toBe(
      'We could not render this page, so we could not check its head.',
    );
    expect(m.responseExposure.sentence).toBe('We could not read enough of this response to say.');
    for (const r of Object.values(m)) {
      expect(r.state).toBe('none');
      expect(r.link).toBeNull();
    }
  });
  it('shows the clean response row as its facts line, and what was not checked', () => {
    const clean = riskRows(
      rx({ verdict: 'clean', leaks: [], tells: [], endOfLife: [], headers: { missing: [] } }),
    )[0];
    expect(clean.sentence).toBe(
      '0 error-output pattern(s) · 0 version tell(s) · end of life: none seen · missing: none. One page only; not a security assessment.',
    );
    const unknown = riskRows(
      rx({ verdict: 'unknown', notMeasured: ['static-html', 'headers'] }),
    )[0];
    expect(unknown.sentence).toBe(
      'We could not read enough of this response to say. Not checked: the HTML as served without JavaScript, the response headers.',
    );
  });
  it('lists the mod_pagespeed phrase once', () => {
    const row = riskRows(
      rx({
        verdict: 'attention',
        reasons: ['end-of-life'],
        endOfLife: [
          { product: 'mod_pagespeed', line: 'a', basis: 'no-fixes' },
          { product: 'mod_pagespeed', line: 'b', basis: 'no-fixes' },
        ],
      }),
    )[0];
    expect(row.sentence.match(/no longer receives fixes/g)).toHaveLength(1);
  });
  it('shows the response row without a link for version tells and missing headers only', () => {
    const row = riskRows(
      rx({
        verdict: 'attention',
        reasons: ['version-tell', 'missing-headers'],
        leaks: [],
        tells: [{ product: 'apache', source: 'server', version: '2.4.41' }],
        endOfLife: [],
        headers: { missing: ['hsts'] },
      }),
    )[0];
    expect(row.state).toBe('attention');
    expect(row.link).toBeNull();
    expect(row.building).toEqual([]);
    expect(row.sentence).toContain('end of life: none seen');
  });
  it('fires the response link for end-of-life alone, and names a mod_pagespeed build', () => {
    const row = riskRows(
      rx({
        verdict: 'attention',
        reasons: ['end-of-life'],
        endOfLife: [{ product: 'mod_pagespeed', line: '1.13', basis: 'no-fixes' }],
      }),
    )[0];
    expect(row.link?.event).toBe('airead-cta-response-firewall');
    expect(row.sentence).toContain('mod_pagespeed (no longer receives fixes)');
  });
  it('does not fire the response link for a hostile blocked fixture', () => {
    const row = riskRows({
      responseExposure: {
        status: 'blocked',
        verdict: 'attention',
        reasons: ['debug-output'],
        note: 'Blocked by a challenge page.',
      },
    })[0];
    expect(row.state).toBe('none');
    expect(row.link).toBeNull();
    expect(row.sentence).toBe('Blocked by a challenge page.');
  });
  it('does not render a script link for a clean inventory with only pinned scripts', () => {
    const row = riskRows({
      scriptInventory: {
        status: 'ok',
        verdict: 'clean',
        totals: { scripts: 2, thirdPartyExternal: 1, thirdPartyWithIntegrity: 1 },
      },
    })[0];
    expect(row.link).toBeNull();
  });
});
