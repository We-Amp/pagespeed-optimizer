// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Validates the `from` / `redirect` query value on /contact/thanks/ before it
// becomes a link. Only a root-relative path is accepted: one leading slash, no
// protocol-relative `//`, no backslash, no scheme, no control characters.

const PLACEHOLDER_ORIGIN = 'https://placeholder.invalid';

export function safeReturnPath(value: string | null | undefined): string | null {
  if (typeof value !== 'string' || value.length === 0 || value.length > 512) return null;
  if (!value.startsWith('/') || value.startsWith('//')) return null;
  // eslint-disable-next-line no-control-regex
  if (/[\\\u0000-\u001f\u007f]/.test(value)) return null;
  try {
    const url = new URL(value, PLACEHOLDER_ORIGIN);
    if (url.origin !== PLACEHOLDER_ORIGIN) return null;
  } catch {
    return null;
  }
  return value;
}
