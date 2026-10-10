// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { SCAN_UI_STORAGE_KEY, resolveScanUi } from './flag';

function memoryStorage(initial: Record<string, string> = {}) {
  const data = { ...initial };
  return {
    data,
    getItem: (k: string) => (k in data ? data[k] : null),
    setItem: (k: string, v: string) => {
      data[k] = v;
    },
  };
}

const boom = () => {
  throw new Error('blocked');
};

describe('resolveScanUi', () => {
  it('returns the default when nothing is asked and nothing is stored', () => {
    expect(resolveScanUi('', memoryStorage(), 'v1')).toBe('v1');
    expect(resolveScanUi('', memoryStorage(), 'v2')).toBe('v2');
    expect(resolveScanUi('?url=https://example.com', memoryStorage(), 'v1')).toBe('v1');
  });

  it('lets ?ui=v2 win over the default and over a stored v1, and stores it', () => {
    const s = memoryStorage({ [SCAN_UI_STORAGE_KEY]: 'v1' });
    expect(resolveScanUi('?ui=v2', s, 'v1')).toBe('v2');
    expect(s.data[SCAN_UI_STORAGE_KEY]).toBe('v2');
  });

  it('lets ?ui=v1 win over the default and over a stored v2, and stores it', () => {
    const s = memoryStorage({ [SCAN_UI_STORAGE_KEY]: 'v2' });
    expect(resolveScanUi('?ui=v1', s, 'v2')).toBe('v1');
    expect(s.data[SCAN_UI_STORAGE_KEY]).toBe('v1');
  });

  it('uses the stored value when the query is silent', () => {
    expect(resolveScanUi('', memoryStorage({ 'scan-ui': 'v2' }), 'v1')).toBe('v2');
    expect(resolveScanUi('', memoryStorage({ 'scan-ui': 'v1' }), 'v2')).toBe('v1');
  });

  it('falls back to the default for an invalid query value, leaving storage alone', () => {
    const s = memoryStorage();
    expect(resolveScanUi('?ui=v3', s, 'v1')).toBe('v1');
    expect(resolveScanUi('?ui=', s, 'v2')).toBe('v2');
    expect(s.data).toEqual({});
  });

  it('falls back to the default for an invalid stored value', () => {
    expect(resolveScanUi('', memoryStorage({ 'scan-ui': 'v9' }), 'v1')).toBe('v1');
    expect(resolveScanUi('', memoryStorage({ 'scan-ui': '' }), 'v2')).toBe('v2');
  });

  it('an invalid query value does not hide a valid stored one', () => {
    expect(resolveScanUi('?ui=nope', memoryStorage({ 'scan-ui': 'v2' }), 'v1')).toBe('v2');
  });

  it('survives a getItem that throws', () => {
    const s = { getItem: boom, setItem: () => undefined };
    expect(resolveScanUi('', s, 'v1')).toBe('v1');
    expect(resolveScanUi('', s, 'v2')).toBe('v2');
    expect(resolveScanUi('?ui=v2', s, 'v1')).toBe('v2');
  });

  it('survives a setItem that throws and still honours the query', () => {
    const s = { getItem: () => 'v1', setItem: boom };
    expect(resolveScanUi('?ui=v2', s, 'v1')).toBe('v2');
    expect(resolveScanUi('', s, 'v2')).toBe('v1');
  });

  it('works without any storage', () => {
    expect(resolveScanUi('?ui=v2', null, 'v1')).toBe('v2');
    expect(resolveScanUi('', undefined, 'v2')).toBe('v2');
  });
});
