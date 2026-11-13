// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The version map (src/data/version-map.mjs) is hand-kept, one row per
// release. Its first row is the current release, so it must agree with the
// sources that already carry that release: the release manifest, the Helm
// chart and the module package version. A release that forgets to add its row
// fails here.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { parse as parseYaml } from 'yaml';
import { MODULE_PACKAGE_VERSION, SIDECAR_VERSION } from '../src/data/product-facts.mjs';
import { VERSION_MAP, versionMapMarkdown } from '../src/data/version-map.mjs';
import { releaseManifest } from './lib/release-manifest';
import { expectOrWarn } from './lib/release-window';

// Numeric x.y.z comparison: negative, zero or positive.
function compareVersions(a: string, b: string): number {
  const pa = a.split('.').map(Number);
  const pb = b.split('.').map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d !== 0) return d;
  }
  return 0;
}

const chart = parseYaml(
  readFileSync(new URL('../../deploy/helm/pagespeed/Chart.yaml', import.meta.url), 'utf8'),
);

describe('version map', () => {
  const current = VERSION_MAP[0];

  it('starts with the release in the manifest', () => {
    expect(current.release).toBe(releaseManifest.release.semver);
    expect(current.date).toBe(String(releaseManifest.release.released_on));
  });

  it('gives the current release the chart and package versions that ship it', () => {
    // The chart is bumped by the release lane's own pull request
    // (release/helm-*) before the site's version map follows, so in a release
    // window a mismatch here is a warning (test/lib/release-window.ts).
    expectOrWarn('version map row vs Helm chart', () => {
      expect(current.helmChart).toBe(String(chart.version));
      expect(String(chart.appVersion)).toBe(current.release);
    });
    // Two committed files, not live state: strict in a window too.
    expect(current.packages).toBe(MODULE_PACKAGE_VERSION);
  });

  it('lists releases newest first, each once, with ISO dates', () => {
    const releases = VERSION_MAP.map((r) => r.release);
    expect(new Set(releases).size).toBe(releases.length);
    const dates = VERSION_MAP.map((r) => r.date);
    for (const d of dates) expect(d).toMatch(/^\d{4}-\d{2}-\d{2}$/);
    expect([...dates].sort().reverse()).toEqual(dates);
  });

  it('carries one number on every artifact from 2.5.0 on, except the chart', () => {
    // A module-only update (2.8.2) publishes new module packages and the IIS
    // installer without new images, chart or NuGet packages, so the packages
    // may lead the release; they never lag it, and everything else stays on it.
    for (const r of VERSION_MAP.filter((row) => /^2\.(?:[5-9]|\d{2,})\./.test(row.release))) {
      expect([r.images, r.nuget], r.release).toEqual([r.release, r.release]);
      expect(
        compareVersions(r.packages, r.release),
        `${r.release} packages`,
      ).toBeGreaterThanOrEqual(0);
      expect(r.packages.split('.').slice(0, 2), `${r.release} packages stay on the line`).toEqual(
        r.release.split('.').slice(0, 2),
      );
      expect(r.iisInstaller, r.release).toBe(true);
    }
  });

  it('keeps the images and the NuGet packages of the current release on the release', () => {
    expect(current.images).toBe(releaseManifest.release.semver);
    expect(current.nuget).toBe(releaseManifest.release.semver);
    expect(SIDECAR_VERSION).toBe(releaseManifest.release.semver);
  });

  it('renders a Markdown table with one row per release', () => {
    const md = versionMapMarkdown();
    const rows = md.split('\n').filter((l) => l.startsWith('| '));
    expect(rows).toHaveLength(VERSION_MAP.length + 2);
    expect(md).toContain(`pagespeed-iis-${current.packages}-win-x64.msi`);
  });
});
