// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// /api/product.json must say the same thing as the copy: the private package
// repository for Priority and Enterprise is being set up, and there is no
// hardened-builds offer.

import { describe, expect, it } from 'vitest';
import { GET as productJsonGet } from '../src/pages/api/product.json.ts';

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
    expect(ent.includes.join(' ')).not.toMatch(/build targets/i);
    expect(ent.note).toContain('available on request, quoted separately');
  });
});
