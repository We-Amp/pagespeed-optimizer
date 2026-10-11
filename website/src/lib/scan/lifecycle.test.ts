// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { MODULE_PACKAGE_VERSION } from '../../data/product-facts.mjs';
import { lifecycleSentence, parseVersion } from './lifecycle';

const GOOGLE = (v: string) =>
  `mod_pagespeed ${v}: this line no longer receives fixes. Move to the current release; configuration carries over.`;
const NEWER = (v: string) =>
  `mod_pagespeed ${v}: a newer release is available. Update recommended.`;

describe('lifecycleSentence', () => {
  it('says the archived upstream line no longer receives fixes', () => {
    for (const v of ['1.13.35.2', '1.14.36.1', '1.9.32.14']) {
      expect(lifecycleSentence(v)).toBe(GOOGLE(v));
    }
  });
  it('bands 1.1.x as our renumbered 1.15 line, not by naive minor comparison', () => {
    for (const v of ['1.1.0+r24', 'v1.1.0+r24', '1.1.3', '1.1']) {
      expect(lifecycleSentence(v)).toBe(NEWER(v.trim()));
    }
    expect(parseVersion('1.1.0+r24')).toMatchObject({ major: 1, minor: 1 });
    expect(parseVersion('1.14.36.1')).toMatchObject({ major: 1, minor: 14 });
  });
  it('says a newer release is available from 1.15 up to below current', () => {
    for (const v of ['1.15.0', '1.16.0']) expect(lifecycleSentence(v)).toBe(NEWER(v));
  });
  it('has no sentence at or past the current package version, suffixes included', () => {
    const cur = MODULE_PACKAGE_VERSION;
    for (const v of [cur, `${cur}-rc1`, `${cur}-dev`, `${cur}+r2`, '1.99.0']) {
      expect(lifecycleSentence(v)).toBe('');
    }
  });
  it('reads current from product facts, not a literal', () => {
    expect(lifecycleSentence('1.16.0', '1.16.0')).toBe('');
    expect(lifecycleSentence('1.16.0', '1.18.0')).toBe(NEWER('1.16.0'));
  });
  it('points 2.0.x at the migration guide, labelled ModPageSpeed, with no support claim', () => {
    const s = lifecycleSentence('2.0.3');
    expect(s).toBe(
      'ModPageSpeed 2.0.3: Migrate from ModPageSpeed 2.0 explains whether your setup moves to 2.1 today.',
    );
    expect(s).not.toMatch(/frozen|security fixes|in place|supported/i);
  });
  it('has no sentence for 2.1 and later', () => {
    for (const v of ['2.1.0', '2.2.0', '3.0']) expect(lifecycleSentence(v)).toBe('');
  });
  it('has no sentence for an unparsable version', () => {
    for (const v of ['', 'garbage', 'latest', '1', '.5', 'x1.2', undefined, null, 7]) {
      expect(lifecycleSentence(v)).toBe('');
    }
  });
  it('never says supported line, frozen or in place', () => {
    for (const v of ['1.1.0+r24', '1.13.35.2', '1.15.0', '1.16.0', '2.0.3']) {
      expect(lifecycleSentence(v)).not.toMatch(/supported line|frozen|in place/i);
    }
  });
});
