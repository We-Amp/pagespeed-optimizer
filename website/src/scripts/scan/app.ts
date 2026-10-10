// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Scan controller for the v2 interface. Loaded lazily, only when v2 is
// active. For now it starts the three requests at once (scanner, PageSpeed
// Insights mobile, PageSpeed Insights desktop) and sets each tile's state;
// the drill-down panels stay empty.

import {
  CHECKING,
  NOT_MEASURED,
  aireadStatus,
  healthLine,
  riskStatus,
  speedStatus,
  type TileStatus,
} from '../../lib/scan/status';

type Pillar = 'speed' | 'airead' | 'risk';
type Strategy = 'mobile' | 'desktop';

const PSI_ENDPOINT = '/psi/v5/runPagespeed';
const PSI_TIMEOUT_MS = 60_000;
const SCAN_TIMEOUT_MS = 45_000;
const PILLARS: Pillar[] = ['speed', 'airead', 'risk'];
const PILLAR_NAME: Record<Pillar, string> = {
  speed: 'Speed',
  airead: 'AI readability',
  risk: 'Risk & SEO',
};
const TONE: Record<TileStatus['state'], string> = {
  checking: 'badge-neutral',
  good: 'badge-success',
  'needs-work': 'badge-accent',
  poor: 'badge-accent',
  clean: 'badge-neutral',
  flagged: 'badge-accent',
  none: 'badge-neutral',
};

const SCAN_TIMEOUT_REASON = 'The scanner did not answer in time.';
const PSI_TIMEOUT_REASON = 'PageSpeed Insights did not answer within a minute.';

interface PsiBody {
  lighthouseResult?: { categories?: { performance?: { score?: number | null } } };
}
interface ScanBody {
  error?: string;
  report?: Record<string, unknown> & { grade?: string; score?: number };
}

class RequestError extends Error {
  constructor(
    message: string,
    readonly kind: 'timeout' | 'network' | 'http',
  ) {
    super(message);
  }
}

// Normalise what the visitor typed: a bare host gets https://, then it must
// parse as an http(s) URL with a public-looking hostname.
export function normalizeUrl(
  raw: string,
): { ok: true; url: string } | { ok: false; reason: string } {
  let text = (raw || '').trim();
  if (!text) return { ok: false, reason: 'Enter a URL to check.' };
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) text = 'https://' + text;
  let parsed: URL;
  try {
    parsed = new URL(text);
  } catch {
    return { ok: false, reason: 'That does not look like a valid URL (try https://example.com).' };
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    return { ok: false, reason: 'Only http:// and https:// URLs are supported.' };
  }
  if (!parsed.hostname || parsed.hostname === 'localhost') {
    return { ok: false, reason: 'We need a publicly reachable hostname. Localhost won’t work.' };
  }
  return { ok: true, url: parsed.toString() };
}

async function getJson<T>(url: string, timeoutMs: number, timeoutReason: string): Promise<T> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetch(url, { method: 'GET', cache: 'no-store', signal: ctl.signal });
    let body: unknown = null;
    try {
      body = await res.json();
    } catch {
      /* non-JSON error body */
    }
    if (!res.ok) {
      const detail = (body as { error?: unknown } | null)?.error;
      const message =
        typeof detail === 'string'
          ? detail
          : ((detail as { message?: string } | undefined)?.message ?? `HTTP ${res.status}`);
      throw new RequestError(message, 'http');
    }
    return body as T;
  } catch (err) {
    if (err instanceof RequestError) throw err;
    if (ctl.signal.aborted) throw new RequestError(timeoutReason, 'timeout');
    throw new RequestError('The request failed.', 'network');
  } finally {
    clearTimeout(timer);
  }
}

const perfScore = (r: PsiBody): number | null => {
  const s = r.lighthouseResult?.categories?.performance?.score;
  return typeof s === 'number' ? Math.round(s * 100) : null;
};

function track(name: string, data?: Record<string, unknown>) {
  try {
    (window as unknown as { umami?: { track: (n: string, d?: unknown) => void } }).umami?.track(
      name,
      data,
    );
  } catch {
    /* analytics must never break a run */
  }
}

export function init() {
  const root = document.querySelector<HTMLElement>('[data-scan-results]');
  const form = document.querySelector<HTMLFormElement>('form[data-scan-box]');
  if (!root || !form) return;
  const surface = form.dataset.scanSurface === 'airead' ? 'airead' : 'analyze';
  const input = form.querySelector<HTMLInputElement>('input[name="url"]')!;
  const submit = form.querySelector<HTMLButtonElement>('button[type="submit"]')!;
  const submitLabel = form.querySelector<HTMLElement>('[data-scan-submit-label]')!;
  const errorEl = form.querySelector<HTMLElement>('[role="alert"]')!;
  const live = root.querySelector<HTMLElement>('[data-scan-live]')!;
  const heading = root.querySelector<HTMLElement>('[data-scan-heading]')!;
  const hostEl = root.querySelector<HTMLElement>('[data-scan-host]')!;
  const healthEl = root.querySelector<HTMLElement>('[data-scan-health]')!;
  const tiles = new Map<Pillar, HTMLButtonElement>();
  const panels = new Map<Pillar, HTMLElement>();
  for (const p of PILLARS) {
    tiles.set(p, root.querySelector<HTMLButtonElement>(`[data-scan-tile="${p}"]`)!);
    panels.set(p, root.querySelector<HTMLElement>(`[data-scan-panel="${p}"]`)!);
  }

  const statuses: Record<Pillar, TileStatus> = {
    speed: CHECKING,
    airead: CHECKING,
    risk: CHECKING,
  };
  let runId = 0;
  let running = false;
  let announcedFirst = false;

  track('scan-ui', { variant: 'v2' });

  function announce(text: string) {
    live.textContent = text;
  }

  function paintTile(pillar: Pillar, status: TileStatus, reason = '') {
    statuses[pillar] = status;
    const tile = tiles.get(pillar)!;
    tile.dataset.state = status.state;
    const word = tile.querySelector<HTMLElement>('[data-scan-status]')!;
    word.textContent = status.word;
    word.className = `scan-tile-status ${TONE[status.state]}`;
    const value = tile.querySelector<HTMLElement>('[data-scan-value]')!;
    if (status.state === 'checking') {
      const bar = document.createElement('span');
      bar.className = 'scan-skeleton';
      bar.setAttribute('aria-hidden', 'true');
      value.replaceChildren(bar);
    } else {
      value.textContent = status.value || reason;
      if (!status.value && reason) value.style.fontWeight = '400';
      else value.style.removeProperty('font-weight');
    }
    healthEl.textContent = healthLine(PILLARS.map((p) => statuses[p]));
  }

  function setOpen(open: Pillar | null) {
    for (const p of PILLARS) {
      const isOpen = p === open;
      const tile = tiles.get(p)!;
      tile.setAttribute('aria-expanded', String(isOpen));
      tile.setAttribute('aria-label', `${isOpen ? 'Hide' : 'Show'} ${PILLAR_NAME[p]} details`);
      panels.get(p)!.hidden = !isOpen;
    }
    if (open) track('scan-tile-open', { pillar: open });
  }

  for (const p of PILLARS) {
    tiles.get(p)!.addEventListener('click', () => {
      const wasOpen = tiles.get(p)!.getAttribute('aria-expanded') === 'true';
      setOpen(wasOpen ? null : p);
    });
  }

  function arrival(text: string) {
    announce(text);
    if (!announcedFirst) {
      announcedFirst = true;
      heading.focus({ preventScroll: true });
    }
  }

  async function runSpeed(id: number, url: string) {
    const [m, d] = await Promise.allSettled([
      getJson<PsiBody>(psiUrl(url, 'mobile'), PSI_TIMEOUT_MS, PSI_TIMEOUT_REASON),
      getJson<PsiBody>(psiUrl(url, 'desktop'), PSI_TIMEOUT_MS, PSI_TIMEOUT_REASON),
    ]);
    if (id !== runId) return;
    const mobile = m.status === 'fulfilled' ? perfScore(m.value) : null;
    const desktop = d.status === 'fulfilled' ? perfScore(d.value) : null;
    if (mobile !== null || desktop !== null) {
      let hostname = '';
      try {
        hostname = new URL(url).hostname;
      } catch {
        /* url was validated */
      }
      track('psi-analyze-result', {
        url,
        hostname,
        mobile_score: mobile,
        desktop_score: desktop,
      });
    }
    const status = speedStatus(mobile, desktop);
    if (status.state === 'none') {
      const failed = m.status === 'rejected' ? m : d.status === 'rejected' ? d : null;
      const err = failed?.reason;
      const reason =
        err instanceof RequestError
          ? err.kind === 'network'
            ? 'Couldn’t reach PSI.'
            : err.message
          : 'No result.';
      paintTile('speed', status, reason);
      arrival(`Speed not measured. ${reason}`);
    } else {
      paintTile('speed', status);
      arrival(`Speed ready: ${status.word.toLowerCase()}. ${status.value}.`);
    }
  }

  async function runScan(id: number, url: string) {
    let report: ScanBody['report'] | null = null;
    let reason = '';
    try {
      const body = await getJson<ScanBody>(
        `${apiBase()}/scan?url=${encodeURIComponent(url)}`,
        SCAN_TIMEOUT_MS,
        SCAN_TIMEOUT_REASON,
      );
      if (body.error) reason = body.error;
      else report = body.report ?? null;
    } catch (err) {
      reason =
        err instanceof RequestError && err.kind !== 'network'
          ? err.message
          : 'Could not reach the scanner service. Check the URL and try again.';
    }
    if (id !== runId) return;
    if (report) {
      track('ai-scan', {
        domain: hostOf(url),
        grade: report.grade,
        score: report.score,
        csr_gap:
          (report.categories as Record<string, { flag?: string }> | undefined)?.['Content fidelity']
            ?.flag === 'csr-gap',
      });
    }
    const ai = report ? aireadStatus(report) : NOT_MEASURED;
    const risk = report ? riskStatus(report) : NOT_MEASURED;
    const why = reason || 'The scanner returned no result.';
    paintTile('airead', ai, why);
    paintTile('risk', risk, why);
    arrival(
      ai.state === 'none'
        ? `AI readability not measured. ${why}`
        : `AI readability ready: grade ${report!.grade}, ${report!.score} of 100.`,
    );
  }

  function hostOf(url: string) {
    try {
      return new URL(url).hostname;
    } catch {
      return url;
    }
  }

  function psiUrl(url: string, strategy: Strategy) {
    return `${PSI_ENDPOINT}?${new URLSearchParams({ url, strategy }).toString()}`;
  }

  function apiBase() {
    return new URLSearchParams(location.search).get('api') || '/ai-readability/api';
  }

  function setBusy(busy: boolean) {
    running = busy;
    submit.setAttribute('aria-disabled', String(busy));
    submit.setAttribute('aria-busy', String(busy));
    submitLabel.textContent = busy ? 'Checking…' : 'Check page';
  }

  function showError(reason: string | null) {
    errorEl.textContent = reason ?? '';
    errorEl.classList.toggle('hidden', !reason);
    input.setAttribute('aria-invalid', reason ? 'true' : 'false');
  }

  function start(url: string) {
    const id = ++runId;
    announcedFirst = false;
    setBusy(true);
    hostEl.textContent = hostOf(url);
    root!.hidden = false;
    for (const p of PILLARS) paintTile(p, CHECKING);
    setOpen(null);
    track('psi-analyze-submit');
    const done = Promise.allSettled([runSpeed(id, url), runScan(id, url)]);
    void done.then(() => {
      if (id === runId) setBusy(false);
    });
    root!.scrollIntoView({
      behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth',
      block: 'start',
    });
  }

  form.addEventListener('submit', (ev) => {
    ev.preventDefault();
    if (running) return;
    const result = normalizeUrl(input.value);
    if (!result.ok) {
      showError(result.reason);
      input.focus();
      return;
    }
    showError(null);
    start(result.url);
  });

  // ?url= deep links run on both pages.
  const preset = new URLSearchParams(location.search).get('url');
  if (preset) {
    input.value = preset;
    const result = normalizeUrl(preset);
    if (result.ok) start(result.url);
    else showError(result.reason);
  }

  // Development only: ?fixture=full paints every tile from canned data so the
  // layout can be reviewed without a network.
  if (import.meta.env.DEV && new URLSearchParams(location.search).get('fixture') === 'full') {
    showFixture();
  }

  function showFixture() {
    hostEl.textContent = 'example.com';
    root!.hidden = false;
    paintTile('speed', speedStatus(58, 91));
    paintTile('airead', aireadStatus({ grade: 'C', score: 64 }));
    paintTile(
      'risk',
      riskStatus({
        preConsentLeak: { status: 'ok', verdict: 'leaks' },
        scriptInventory: { status: 'ok', verdict: 'clean' },
        seoDefects: { status: 'ok', verdict: 'attention' },
        responseExposure: { status: 'blocked' },
      }),
    );
  }
}
