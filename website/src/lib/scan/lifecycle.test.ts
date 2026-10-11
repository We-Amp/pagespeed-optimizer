import { describe, expect, it } from 'vitest';
import { lifecycleSentence, parseVersion } from './lifecycle';

describe('lifecycleSentence', () => {
  it('says a line below 1.15 no longer receives fixes', () => {
    for (const v of ['1.9.32.14', '1.13', '1.14.36.1', '0.10.22.7']) {
      expect(lifecycleSentence(v)).toBe(
        `mod_pagespeed ${v}: this line no longer receives fixes. Upgrade in place to the current release.`,
      );
    }
  });
  it('calls 1.15 up to 1.17.x a supported line', () => {
    for (const v of ['1.15.0.1', '1.16', '1.17.1']) {
      expect(lifecycleSentence(v)).toBe(
        `mod_pagespeed ${v}: supported line. Update to the current release for the latest fixes.`,
      );
    }
  });
  it('calls 2.0.x frozen, with the security-only date', () => {
    expect(lifecycleSentence('2.0.3')).toBe(
      'mod_pagespeed 2.0.3: frozen line, security fixes only until 2027-02-07. Upgrade in place to the current release.',
    );
  });
  it('has no sentence for 2.1 and later', () => {
    for (const v of ['2.1', '2.1.0', '2.2.0', '3.0']) expect(lifecycleSentence(v)).toBe('');
  });
  it('has no sentence for an unparsable version', () => {
    for (const v of ['', 'a', 'latest', '1', '.5', undefined, null, 7]) {
      expect(lifecycleSentence(v)).toBe('');
    }
    expect(parseVersion('x1.2')).toBeNull();
  });
  it('never names 2.1 as the target', () => {
    for (const v of ['1.13', '1.16', '2.0.1']) expect(lifecycleSentence(v)).not.toContain('2.1');
  });
});
