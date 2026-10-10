// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Which scan interface a visitor sees on /analyze/ and /ai-readability/.
// `SCAN_UI_DEFAULT` is the switch: changing this literal changes the default
// for everyone. A visitor can always pick for themselves with `?ui=v1` or
// `?ui=v2`; the choice is remembered in localStorage.

export type ScanUi = 'v1' | 'v2';

export const SCAN_UI_DEFAULT: ScanUi = 'v1';

export const SCAN_UI_STORAGE_KEY = 'scan-ui';

export interface ScanUiStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

const isScanUi = (value: unknown): value is ScanUi => value === 'v1' || value === 'v2';

// Pure apart from the storage it is handed. A query value wins and is
// remembered; otherwise the remembered value; otherwise the default. Storage
// can be missing or throw (private windows, blocked site data): that never
// changes the answer, only whether it sticks.
export function resolveScanUi(
  search: string,
  storage: ScanUiStorage | null | undefined,
  fallback: ScanUi = SCAN_UI_DEFAULT,
): ScanUi {
  let fromQuery: string | null = null;
  try {
    fromQuery = new URLSearchParams(search).get('ui');
  } catch {
    fromQuery = null;
  }
  if (isScanUi(fromQuery)) {
    try {
      storage?.setItem(SCAN_UI_STORAGE_KEY, fromQuery);
    } catch {
      /* a failed write only means the choice is not remembered */
    }
    return fromQuery;
  }
  try {
    const stored = storage?.getItem(SCAN_UI_STORAGE_KEY);
    if (isScanUi(stored)) return stored;
  } catch {
    /* fall through to the default */
  }
  return fallback;
}
