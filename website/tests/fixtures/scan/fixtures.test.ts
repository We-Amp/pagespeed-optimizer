// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The scan fixtures drive the visual suite. They must stay faithful to what
// the gate predicates read: the full report fires all eight gates, the clean
// one fires none, and the blocked one fires none with its risk lenses marked
// as not measured.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  agentPassCtaApplies,
  complianceFixCtaApplies,
  consentEnforcementCtaApplies,
  edgeSeoCtaApplies,
  pageIntegrityCtaApplies,
  responseFirewallCtaApplies,
  thirdPartyFreezeCtaApplies,
  tollboothCtaApplies,
} from '../../../src/lib/scan/demand.mjs';

const load = (name: string) =>
  JSON.parse(readFileSync(new URL(`./${name}.json`, import.meta.url), 'utf8'));

function gates(report: Record<string, unknown>) {
  return {
    tollbooth: tollboothCtaApplies(report.agentVerifiability),
    agentpass: agentPassCtaApplies(report.signedAgentVerification),
    compliancefix: complianceFixCtaApplies(report.accessibility),
    consent: consentEnforcementCtaApplies(report.preConsentLeak),
    pageIntegrity: pageIntegrityCtaApplies(report.scriptInventory),
    thirdPartyFreeze: thirdPartyFreezeCtaApplies(report.scriptInventory),
    edgeSeo: edgeSeoCtaApplies(report.seoDefects),
    responseFirewall: responseFirewallCtaApplies(report.responseExposure),
  };
}

const RISK_LENSES = ['preConsentLeak', 'scriptInventory', 'seoDefects', 'responseExposure'];

describe('scan fixtures', () => {
  it('full report: grade C, every risk lens ok, all eight gates fire', () => {
    const data = load('report-full');
    const r = data.report;
    expect(r.grade).toBe('C');
    for (const k of RISK_LENSES) expect(r[k].status).toBe('ok');
    expect(r.metrics.rawHtmlTokens).toBeGreaterThan(0);
    expect(r.diff.renderedCleanTokens).toBeGreaterThan(r.diff.staticCleanTokens);
    expect(data.permalink).toMatch(
      /^https:\/\/modpagespeed\.com\/ai-readability\/api\/r\/[0-9a-f]{12}$/,
    );
    expect(Object.values(gates(r)).every((v) => v === true)).toBe(true);
    expect(Object.keys(gates(r))).toHaveLength(8);
  });

  it('clean report: grade A, every risk lens ok and clean, no gate fires', () => {
    const r = load('report-clean').report;
    expect(r.grade).toBe('A');
    for (const k of RISK_LENSES) {
      expect(r[k].status).toBe('ok');
      expect(r[k].verdict).toBe('clean');
    }
    expect(Object.values(gates(r)).some((v) => v === true)).toBe(false);
  });

  it('blocked report: grade present, risk lenses not measured, no gate fires', () => {
    const r = load('report-blocked').report;
    expect(r.grade).toMatch(/^[A-F]$/);
    expect(r.preConsentLeak.status).toBe('blocked');
    expect(r.scriptInventory.status).toBe('blocked');
    expect(r.seoDefects.status).toBe('blocked');
    expect(r.responseExposure.status).toBe('error');
    for (const k of ['preConsentLeak', 'scriptInventory', 'seoDefects']) {
      expect(r[k].verdict).toBe('unknown');
      expect(r[k].note).toMatch(/^Render blocked/);
    }
    expect(Object.values(gates(r)).some((v) => v === true)).toBe(false);
  });

  it('PSI fixtures carry the fields the analyzer reads', () => {
    for (const name of ['psi-mobile', 'psi-desktop']) {
      const lr = load(name).lighthouseResult;
      expect(typeof lr.categories.performance.score).toBe('number');
      const flagged = Object.values<{ details?: { overallSavingsMs?: number } }>(lr.audits).filter(
        (a) => (a.details?.overallSavingsMs ?? 0) > 0,
      );
      expect(flagged.length).toBeGreaterThan(3);
    }
    expect(readFileSync(new URL('./psi-429.html', import.meta.url), 'utf8')).toContain(
      '429 Too Many Requests',
    );
  });
});
