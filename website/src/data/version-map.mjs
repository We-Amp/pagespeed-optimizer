// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// version-map.mjs — which version number each artifact of a release carries.
//
// One row per release since 2.1.0, newest first. From 2.5.0 on, every
// artifact carries the product version except the Helm chart, which has its
// own chart version (0.3.x) and carries the product version as its
// application version. Before 2.5.0 the apt and yum packages had their own
// numbers.
//
// The first row is the current release: test/version-map.test.ts holds it to
// the release manifest (src/content/releases-2.1/release.yaml), the Helm
// chart (deploy/helm/pagespeed/Chart.yaml) and MODULE_PACKAGE_VERSION, so a
// release that forgets this file fails the tests. A new release adds a row
// at the top.
//
// Plain ESM with no imports beyond product-facts: the <VersionMap /> component
// renders it as HTML, and src/lib/docs-markdown.mjs renders the same rows as a
// Markdown table for the agent files.

import { PKG_ASPNETCORE, PKG_SIDECAR, SIDECAR_VERSION } from './product-facts.mjs';

/**
 * @typedef {{
 *   release: string,
 *   date: string,
 *   packages: string,
 *   images: string | null,
 *   helmChart: string | null,
 *   nuget: string,
 *   iisInstaller: boolean,
 * }} VersionRow
 */

/** @type {VersionRow[]} */
export const VERSION_MAP = [
  {
    release: '2.5.2',
    date: '2026-10-22',
    packages: '2.5.2',
    images: '2.5.2',
    helmChart: '0.3.11',
    nuget: '2.5.2',
    iisInstaller: true,
  },
  {
    release: '2.5.0',
    date: '2026-10-19',
    packages: '2.5.0',
    images: '2.5.0',
    helmChart: '0.3.9',
    nuget: '2.5.0',
    iisInstaller: true,
  },
  {
    release: '2.4.0',
    date: '2026-10-14',
    packages: '1.19.0',
    images: '2.4.0',
    helmChart: '0.3.8',
    nuget: '2.4.0',
    iisInstaller: false,
  },
  {
    release: '2.3.0',
    date: '2026-10-12',
    packages: '1.18.0',
    images: '2.3.0',
    helmChart: '0.3.7',
    nuget: '2.3.0',
    iisInstaller: false,
  },
  {
    release: '2.2.0',
    date: '2026-10-06',
    packages: '1.17.0',
    images: '2.2.0',
    helmChart: '0.3.6',
    nuget: '2.2.0',
    iisInstaller: false,
  },
  {
    release: '2.1.0',
    date: '2026-09-17',
    packages: '1.16.0',
    images: null,
    helmChart: null,
    nuget: '2.1.0',
    iisInstaller: false,
  },
];

export const VERSION_MAP_COLUMNS = [
  'Release',
  'Date',
  'apt/yum packages (module + optimizer worker)',
  'Container images',
  'Helm chart (app version)',
  `NuGet ${PKG_ASPNETCORE}`,
  'IIS installer',
];

/** The IIS installer file name of a release (the module's GitHub release). */
export function iisInstallerName(/** @type {string} */ release) {
  return `pagespeed-iis-${release}-win-x64.msi`;
}

/**
 * The cells of one row, as plain text, in VERSION_MAP_COLUMNS order.
 * @param {VersionRow} row
 * @returns {string[]}
 */
export function versionRowCells(row) {
  return [
    row.release,
    row.date,
    row.packages,
    row.images ?? 'none',
    row.helmChart ? `${row.helmChart} (${row.release})` : 'none',
    row.nuget,
    row.iisInstaller ? iisInstallerName(row.release) : 'not on a public release',
  ];
}

export const VERSION_MAP_NOTE = `From 2.5.0 on, one number names the release on every artifact; only the Helm chart keeps its own chart version and carries the release as its application version. The ASP.NET Core Sidecar (${PKG_SIDECAR}) is versioned on its own; the current package is ${SIDECAR_VERSION}.`;

/**
 * The map as a Markdown table plus its note, for the agent files.
 * @returns {string}
 */
export function versionMapMarkdown() {
  const row = (/** @type {string[]} */ cells) => `| ${cells.join(' | ')} |`;
  return [
    row(VERSION_MAP_COLUMNS),
    row(VERSION_MAP_COLUMNS.map(() => '---')),
    ...VERSION_MAP.map((r) => row(versionRowCells(r))),
    '',
    VERSION_MAP_NOTE,
  ].join('\n');
}
