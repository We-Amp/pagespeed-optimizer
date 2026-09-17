// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ModPageSpeed 2.0-specific FAQ entries.
//
// Architectural questions whose answers reference the 2.0 reverse-proxy
// or ASP.NET Core middleware integrations. Appended to faq-shared on the
// pricing page and other 2.0 surfaces.

import type { FaqEntry } from './faq-shared';

export const faq2: FaqEntry[] = [
  {
    q: 'Which integrations are supported?',
    a: 'Two first-class integrations: a Docker / nginx caching reverse proxy in front of any HTTP origin (Apache, Node.js, Caddy, IIS, your CDN\'s origin), and an ASP.NET Core middleware NuGet package. Same C++ optimization pipeline in both. (For an in-process nginx or Apache module on bare metal, that\'s <a href="/1.1/" class="text-interactive hover:text-interactive-hover underline">mod_pagespeed 1.15</a>.)',
  },
  {
    q: 'How does the nginx integration work?',
    a: 'ModPageSpeed 2.0 runs in front of nginx as a Docker reverse proxy. The prebuilt nginx module ships inside the Docker image — there is no separate bare-metal module to install. Point the reverse proxy at your origin and all optimizations apply automatically.',
  },
  {
    q: 'Does it work with Apache?',
    a: 'Yes — as a reverse proxy. Deploy ModPageSpeed 2.0 in front of your Apache server via the Docker Compose setup, point <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">BACKEND_HOST</code> at your origin, and all optimizations apply automatically. If you need an in-process Apache module instead, that\'s <a href="/1.1/" class="text-interactive hover:text-interactive-hover underline">mod_pagespeed 1.15</a>.',
  },
  {
    q: 'Does it work with Kubernetes?',
    a: 'Yes. The Docker distribution uses separate nginx and worker containers, designed for Kubernetes pod deployments. Configuration is passed as environment variables, making it easy to manage via ConfigMaps and Secrets. A Helm chart is included.',
  },
  {
    q: 'Does it add latency?',
    a: 'On cache hit, serving is sub-millisecond — just a hash lookup and an mmap pointer. On cache miss, the original content is served immediately while the worker generates optimized variants in the background. There is no synchronous processing in the request path.',
  },
  {
    q: 'Can it break my site?',
    a: 'ModPageSpeed 2.0 optimizes outside the request path, and its transforms are deliberately conservative: JS minification strips whitespace and comments — no variable renaming — and every image re-encode is checked against a perceptual quality metric. The design falls back to serving your original content when an optimization step fails. No software that rewrites live responses is risk-free, so test on staging before production. You stay in control either way: <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">pagespeed_disallow</code> skips a URL pattern, <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">pagespeed off</code> disables a location, worker flags like <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">--disable-js</code> turn off a transform type, and a PURGE drops cached variants immediately. See <a href="/docs/configuration/" class="text-interactive hover:text-interactive-hover underline">configuration</a>.',
  },
  {
    q: 'Why not just use Cloudflare or another CDN?',
    a: "CDN-based optimization requires routing your traffic through a third-party proxy. ModPageSpeed runs on your servers — your visitors' content stays on your infrastructure, and the software sends nothing to us. Self-hosted by design; helps your GDPR posture and keeps you in control of caching, configuration, and data.",
  },
];
