// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Cost, ratio and period maths plus the plain-text summary. Cost is an
// estimate at the rate the visitor set; the words on the page say so.
import type { Aggregate, Rates } from './types';

export function parseRate(input: string): number | null {
  const n = Number(input.trim().replace(',', '.'));
  return input.trim() !== '' && Number.isFinite(n) && n >= 0 ? n : null;
}

export function estimateCost(bytes: number, requests: number, rates: Rates): number | null {
  const ok = (v: number) => Number.isFinite(v) && v >= 0;
  if (!ok(rates.perGb) || !ok(rates.perMillionRequests)) return null;
  return (bytes / 1e9) * rates.perGb + (requests / 1e6) * rates.perMillionRequests;
}

export function headlineRatio(a: Aggregate): number | null {
  return a.referredPageRequests === 0 ? null : a.crawlerPageRequests / a.referredPageRequests;
}

export function operatorRatios(a: Aggregate) {
  const pages = new Map<string, number>();
  for (const row of a.crawlers) {
    if (row.kind !== 'fetcher' && row.pageRequests > 0)
      pages.set(row.operator, (pages.get(row.operator) ?? 0) + row.pageRequests);
  }
  const operators = new Set([...pages.keys(), ...Object.keys(a.referredByOperator)]);
  return [...operators].sort().map((operator) => {
    const crawlerPageRequests = pages.get(operator) ?? 0;
    const referred = a.referredByOperator[operator] ?? 0;
    return {
      operator,
      crawlerPageRequests,
      referred,
      ratio: referred === 0 ? null : crawlerPageRequests / referred,
    };
  });
}

export function periodDays(a: Aggregate): number | null {
  if (a.firstTime === null || a.lastTime === null) return null;
  return (a.lastTime - a.firstTime) / 86_400_000;
}

export function projectTo30Days(value: number, days: number | null): number | null {
  return days === null || days < 1 ? null : (value / days) * 30;
}

export function formatMoney(n: number, currency: string): string {
  if (n > 0 && n < 0.01) return `less than 0.01 ${currency}`;
  return `${n.toFixed(2)} ${currency}`;
}

export function formatBytes(n: number): string {
  if (n < 1000) return `${n} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let v = n / 1000;
  let i = 0;
  while (v >= 1000 && i < units.length - 1) {
    v /= 1000;
    i += 1;
  }
  return `${v.toFixed(1)} ${units[i]}`;
}

export function formatRatio(r: number): string {
  return r >= 10 ? Math.round(r).toLocaleString('en-US') : r.toFixed(1);
}

function isoDay(ms: number): string {
  return new Date(ms).toISOString().slice(0, 10);
}

export function formatSummary(a: Aggregate, rates: Rates | null, pageUrl: string): string {
  const out: string[] = [];
  const period =
    a.firstTime !== null && a.lastTime !== null
      ? ` (${isoDay(a.firstTime)} to ${isoDay(a.lastTime)})`
      : '';
  out.push(`AI crawler cost, measured from my own access log${period}`);
  const ratio = headlineRatio(a);
  out.push(
    ratio === null
      ? `No visits from AI assistants in this log (${a.crawlerPageRequests} crawler page requests).`
      : `${formatRatio(ratio)} crawler page requests per visit an AI assistant sent back (${a.crawlerPageRequests} crawler page requests, ${a.referredPageRequests} visits)`,
  );
  const cost = rates ? estimateCost(a.crawlerBytes, a.crawlerRequests, rates) : null;
  const perReq =
    rates && rates.perMillionRequests > 0
      ? ` and ${rates.perMillionRequests} ${rates.currency} per million requests`
      : '';
  const head = `AI crawlers: ${a.crawlerRequests} requests, ${formatBytes(a.crawlerBytes)}, estimated cost`;
  out.push(
    rates && cost !== null
      ? `${head} ${formatMoney(cost, rates.currency)} at ${rates.perGb} ${rates.currency} per GB${perReq}`
      : `${head} not set (enter a rate)`,
  );
  for (const row of a.crawlers.slice(0, 5)) {
    const c = rates ? estimateCost(row.bytes, row.requests, rates) : null;
    out.push(
      `- ${row.token} (${row.operator}): ${row.requests} requests, ${formatBytes(row.bytes)}${rates && c !== null ? `, ${formatMoney(c, rates.currency)}` : ''}`,
    );
  }
  out.push(`Measured in the browser with ${pageUrl}; nothing from the log was uploaded.`);
  return out.join('\n');
}
