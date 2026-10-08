// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The measured impact of one /examples/<slug>/ demo, read from
// src/data/examples-data.json (written by tools/examples-gallery/generate.mjs).
// Shared by the example page (src/pages/examples/[slug].astro) and the filter
// topic pages (src/pages/docs/filters/[name].astro), so both show the same
// numbers from the same rules.

import { EXAMPLES, MECHANISM, type Example, type Mechanism } from '../data/examples';
import examplesData from '../data/examples-data.json';

export interface ImpactTile {
  label: string;
  before: string;
  after: string;
  pill: string;
}

export interface ExampleImpact {
  ex: Example;
  /** How the filter shows when it is not a plain HTML rewrite. */
  mechanism: Mechanism | undefined;
  /** The raw generator entry, when there is one. */
  measured: any;
  /** The generator has an entry without an error. */
  present: boolean;
  /** ...and it captured a stable before/after. */
  hasData: boolean;
  /** A beacon-gated capture that settled byte-identical: nothing real to show. */
  nonSubstantive: boolean;
  substantive: boolean;
  diff: { t: string; s: string }[] | undefined;
  savings: any;
  /** The byte and request tiles worth showing. */
  tiles: ImpactTile[];
}

export function formatBytes(bytes: number): string {
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return bytes + ' B';
}

export function exampleImpact(slug: string): ExampleImpact | null {
  const ex = EXAMPLES.find((e) => e.slug === slug);
  if (!ex) return null;
  const mechanism = MECHANISM[ex.slug];
  const data = examplesData as { examples: Record<string, any> };
  const measured = data.examples[ex.slug];
  // "Settled" entries captured a stable before/after the generator could diff.
  // Unsettled-but-present entries are header-/beacon-/config-gated filters that
  // leave no HTML source diff — `mechanism` explains which.
  const present = !!measured && !measured.error;
  const hasData = present && !!measured.settled;
  const diff: { t: string; s: string }[] | undefined = hasData ? measured.diff : undefined;
  const savings = hasData ? measured.savings : undefined;

  // A beacon-gated filter can "settle" on a byte-identical capture — a dedicated
  // demo page whose single-line tags leave no reflow — so the data is present but
  // shows no real change. Treat such a card as non-substantive: prefer the beacon
  // explanation over an empty/cosmetic diff and suppress the "Measured impact"
  // block. Cards with actual +/- diff lines (prioritize_critical_css,
  // inline_preview_images) keep their diff and are unaffected.
  const hasRealDiff = !!diff && diff.some((line) => line.t === '+' || line.t === '-');
  const nonSubstantive =
    mechanism === 'beacon' &&
    hasData &&
    !hasRealDiff &&
    (!savings || (savings.requests === 0 && savings.resourceBytesPct === 0));
  const substantive = !nonSubstantive;

  const tiles: ImpactTile[] = [];
  if (savings && substantive) {
    if (savings.requests > 0) {
      tiles.push({
        label: 'HTTP requests',
        before: String(measured.before.requestCount),
        after: String(measured.after.requestCount),
        pill: `−${savings.requests}`,
      });
    }
    if (savings.totalBytesPct > 0 && measured.before.resourcesMeasured > 0) {
      const b = measured.before.htmlBytes + measured.before.resourceBytes;
      const a = measured.after.htmlBytes + measured.after.resourceBytes;
      tiles.push({
        label: 'Total bytes',
        before: formatBytes(b),
        after: formatBytes(a),
        pill: `−${savings.totalBytesPct}%`,
      });
    }
    if (savings.htmlBytesPct > 0) {
      tiles.push({
        label: 'HTML size',
        before: formatBytes(measured.before.htmlBytes),
        after: formatBytes(measured.after.htmlBytes),
        pill: `−${savings.htmlBytesPct}%`,
      });
    }
  }

  return {
    ex,
    mechanism,
    measured,
    present,
    hasData,
    nonSubstantive,
    substantive,
    diff,
    savings,
    tiles,
  };
}
