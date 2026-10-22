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
import { MODULE_PACKAGE_VERSION } from '../src/data/product-facts.mjs';
import { VERSION_MAP, versionMapMarkdown } from '../src/data/version-map.mjs';
import { releaseManifest } from './lib/release-manifest';

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
    expect(current.helmChart).toBe(String(chart.version));
    expect(String(chart.appVersion)).toBe(current.release);
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
    for (const r of VERSION_MAP.filter((row) => /^2\.(?:[5-9]|\d{2,})\./.test(row.release))) {
      expect([r.packages, r.images, r.nuget], r.release).toEqual([r.release, r.release, r.release]);
      expect(r.iisInstaller, r.release).toBe(true);
    }
  });

  it('renders a Markdown table with one row per release', () => {
    const md = versionMapMarkdown();
    const rows = md.split('\n').filter((l) => l.startsWith('| '));
    expect(rows).toHaveLength(VERSION_MAP.length + 2);
    expect(md).toContain(`pagespeed-iis-${current.release}-win-x64.msi`);
  });
});
