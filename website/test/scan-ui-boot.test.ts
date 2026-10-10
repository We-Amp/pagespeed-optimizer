// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import { describe, expect, it } from 'vitest';
import { SCAN_UI_STORAGE_KEY, resolveScanUi } from '../src/lib/scan/flag';

// The pre-paint script cannot import, so it carries its own copy of the
// resolution rules. Run that very file against the cases resolveScanUi
// answers and require the same result.
const bootSource = readFileSync('src/scripts/scan/boot.js', 'utf8');

function boot(search: string, stored: string | null, broken: boolean, fallback: 'v1' | 'v2') {
  const data: Record<string, string> = stored === null ? {} : { [SCAN_UI_STORAGE_KEY]: stored };
  const storage = {
    getItem: (k: string) => {
      if (broken) throw new Error('blocked');
      return data[k] ?? null;
    },
    setItem: (k: string, v: string) => {
      if (broken) throw new Error('blocked');
      data[k] = v;
    },
  };
  const attrs: Record<string, string> = { content: fallback, 'data-key': SCAN_UI_STORAGE_KEY };
  const html = { dataset: {} as Record<string, string> };
  runInNewContext(bootSource, {
    window: { localStorage: storage },
    location: { search },
    document: {
      documentElement: html,
      querySelector: () => ({ getAttribute: (n: string) => attrs[n] ?? null }),
    },
    URLSearchParams,
  });
  return { ui: html.dataset.scanUi, data };
}

describe.each(['v1', 'v2'] as const)('pre-paint scan interface script (default %s)', (fallback) => {
  const cases: Array<[string, string | null, boolean]> = [
    ['', null, false],
    ['?ui=v2', null, false],
    ['?ui=v1', null, false],
    ['?ui=v2', 'v1', false],
    ['?ui=v1', 'v2', false],
    ['?url=https://example.com', 'v2', false],
    ['?url=https://example.com', 'v1', false],
    ['?ui=nope', 'v2', false],
    ['?ui=v2', null, true],
    ['', 'v2', true],
  ];
  for (const [search, stored, broken] of cases) {
    it(`agrees with resolveScanUi for ${JSON.stringify({ search, stored, broken })}`, () => {
      const expected = resolveScanUi(
        search,
        broken
          ? {
              getItem: () => {
                throw new Error('blocked');
              },
              setItem: () => {
                throw new Error('blocked');
              },
            }
          : { getItem: () => stored, setItem: () => undefined },
        fallback,
      );
      expect(boot(search, stored, broken, fallback).ui).toBe(expected);
    });
  }

  it('remembers an explicit choice', () => {
    expect(boot('?ui=v2', null, false, fallback).data[SCAN_UI_STORAGE_KEY]).toBe('v2');
  });
});
