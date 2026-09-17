// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Single source of truth for firing share analytics. The declarative
// `data-umami-event="share"` attributes on the in-page share buttons cover every
// click that lands on a tracked element — but a path that *bypasses* those
// buttons (the OS native share sheet via `navigator.share`) is invisible to that
// mechanism. So any code that invokes the native share sheet MUST go through
// `nativeShare` here, which records the umami event *first*. Keeping the share
// and its tracking coupled in one place is what stops the "mobile share is
// invisible to analytics" bug from recurring as new share entry points are added.

interface ShareData {
  url: string;
  title: string;
}

type UmamiGlobal = {
  umami?: { track?: (event: string, data: Record<string, unknown>) => void };
};

/** Record a umami `share` event. Safe to call before umami has loaded. */
export function trackShare(network: string, surface: string): void {
  (globalThis as UmamiGlobal).umami?.track?.('share', { network, surface });
}

/**
 * Open the OS native share sheet, recording the share intent first so the mobile
 * share path is never analytics-invisible. The Web Share API never reports which
 * target the user picks, so the network is "native". Fire-and-forget — a
 * user-cancelled share (AbortError) is swallowed.
 */
export function nativeShare(surface: string, data: ShareData): void {
  trackShare('native', surface);
  void navigator.share({ title: data.title, url: data.url }).catch(() => {});
}
