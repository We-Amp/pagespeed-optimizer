// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Typed accessors for the release manifests synced from corp/releases/*.yaml
// into src/content/releases-{1.1,2.0}/release.yaml by the corp workflow
// `.github/workflows/sync-manifests-to-mps2.yml` (ADR-062 §7).
//
// Astro pages import `getRelease(line)` to fetch the manifest and
// `artifactUrl(rel, channel, arch)` to render an absolute download URL with
// `%V` / `%R` / `%T` substituted from the manifest's release section.

import { getEntry } from 'astro:content';

type Line = '1.1' | '2.0';

export async function getRelease(line: Line) {
  const collection = `releases-${line}` as 'releases-1.1' | 'releases-2.0';
  const entry = await getEntry(collection, 'release');
  if (!entry) {
    throw new Error(
      `Missing release manifest for ${line} (expected src/content/releases-${line}/release.yaml)`,
    );
  }
  return entry.data;
}

export type Release = Awaited<ReturnType<typeof getRelease>>;

/**
 * Resolve an artifact URL from the manifest.
 *
 * Substitutions:
 *   %V → release.semver         (e.g. "1.1.0")
 *   %R → release.revision ?? 0  (e.g. 9; renders as "9")
 *   %T → release.tag            (e.g. "v1.1.0+r9")
 *
 * Throws when the channel/arch pair is absent so build-time failures surface
 * loudly instead of producing a broken href.
 */
export function artifactUrl(rel: Release, channel: string, arch: string): string {
  const channelTemplates = rel.artifacts[channel];
  if (!channelTemplates) {
    throw new Error(`No artifact channel "${channel}" in ${rel.product.line} manifest`);
  }
  const tmpl = channelTemplates[arch];
  if (typeof tmpl !== 'string') {
    throw new Error(
      `No artifact template for channel=${channel} arch=${arch} in ${rel.product.line} manifest`,
    );
  }
  const filename = tmpl
    .replaceAll('%V', rel.release.semver)
    .replaceAll('%R', String(rel.release.revision ?? 0))
    .replaceAll('%T', rel.release.tag);
  return rel.urls.archive_base + filename;
}

/**
 * Resolve a sibling URL under the same archive_base (e.g. SHA256SUMS).
 * Kept here so download pages don't hand-concatenate `rel.urls.archive_base`.
 */
export function archiveUrl(rel: Release, filename: string): string {
  return rel.urls.archive_base + filename;
}
