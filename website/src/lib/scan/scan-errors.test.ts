// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { scanErrorCopy } from './scan-errors';

describe('scanErrorCopy', () => {
  it.each([
    ['rate limit — slow down a moment', 'Rate limit reached. Try again in a minute.'],
    ['busy — try again in a moment', 'The scanner is busy. Try again in a moment.'],
    [
      'that URL can’t be scanned — it must be a public http(s) website',
      'That address cannot be scanned. It must be a public website address starting with http or https.',
    ],
    [
      "that URL can't be scanned — it must be a public http(s) website",
      'That address cannot be scanned. It must be a public website address starting with http or https.',
    ],
    ['scan failed', 'The scan did not complete. Try again in a moment.'],
    ['internal', 'The scan did not complete. Try again in a moment.'],
    ['forbidden', 'The scan was refused.'],
    ['missing ?url', 'No address was given.'],
    ['  Scan Failed ', 'The scan did not complete. Try again in a moment.'],
  ])('maps %j', (raw, copy) => {
    expect(scanErrorCopy(raw)).toBe(copy);
  });
  it('falls back for unknown and non-string input without echoing it', () => {
    expect(scanErrorCopy('database on fire')).toBe('The scan did not complete.');
    expect(scanErrorCopy(undefined)).toBe('The scan did not complete.');
    expect(scanErrorCopy({ message: 'x' })).toBe('The scan did not complete.');
  });
});
