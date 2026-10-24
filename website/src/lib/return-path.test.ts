// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import { safeReturnPath } from './return-path';

describe('safeReturnPath', () => {
  it('accepts root-relative paths', () => {
    expect(safeReturnPath('/')).toBe('/');
    expect(safeReturnPath('/pricing/')).toBe('/pricing/');
    expect(safeReturnPath('/ai-readability/?u=a#x')).toBe('/ai-readability/?u=a#x');
  });

  it('rejects everything else', () => {
    for (const bad of [
      '',
      null,
      undefined,
      'pricing/',
      '//evil.example/',
      '///evil.example',
      '/\\evil.example',
      '\\\\evil.example',
      'https://evil.example/',
      'javascript:alert(1)',
      '/ok\nhttps://evil.example',
      '/a\tb',
      '/' + 'a'.repeat(600),
    ]) {
      expect(safeReturnPath(bad as string | null | undefined)).toBeNull();
    }
  });
});
