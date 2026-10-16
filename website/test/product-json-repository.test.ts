// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// /api/product.json must say the same thing as the copy: the private package
// repository for Priority and Enterprise is being set up, and there is no
// hardened-builds offer.

import { describe, expect, it, vi } from 'vitest';
import { MODULE_PACKAGE_VERSION } from '../src/data/product-facts.mjs';
import { GET as productJsonGet } from '../src/pages/api/product.json.ts';

// product.json.ts reads the release manifest through astro:content, which only
// exists inside an Astro build; stand in a manifest with the fields it reads.
vi.mock('../src/lib/release', () => ({
  getRelease: async () => ({
    release: { semver: '2.4.0', tag: 'v2.4.0', released_on: '2026-10-14' },
  }),
}));

describe('/api/product.json private repository status', () => {
  it('states the repository as being set up and offers no hardened builds', async () => {
    const res = (productJsonGet as (ctx?: unknown) => Response)({});
    const data = JSON.parse(await res.text());
    expect(data.artifacts.hardened_builds).toBe('not-offered');
    expect(data.artifacts.hardened_pricing).toBe('not-offered');
    expect(data.artifacts.private_repository).toBe('being-set-up');
    for (const tier of data.support.tiers) {
      expect(tier.kind).toBe('support');
      expect(tier.hardened_builds).toBe(false);
    }
    const priority = data.support.tiers.find((t: { id: string }) => t.id === 'priority');
    expect(priority.note).toContain('private package repository (being set up)');
    const ent = data.support.tiers.find((t: { id: string }) => t.id === 'enterprise');
    expect(ent.includes.join(' ')).toContain('Custom build targets');
  });

  it('carries a release block with the line, version and module package version', async () => {
    const res = (productJsonGet as (ctx?: unknown) => Response)({});
    const data = JSON.parse(await res.text());
    expect(data.release).toEqual({
      line: '2.1',
      version: '2.4.0',
      tag: 'v2.4.0',
      released_on: '2026-10-14',
      module_package_version: MODULE_PACKAGE_VERSION,
    });
  });
});
