// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Architecture FAQ entries for the converged line.
//
// Questions whose answers reference the reverse-proxy and ASP.NET Core
// integrations of the optimizer worker. Appended to faq-shared on the pricing
// page and the other product surfaces.

import type { FaqEntry } from './faq-shared';

export const faq2: FaqEntry[] = [
  {
    q: 'Which integrations are supported?',
    a: 'Three ways to run it: the native in-process module for Apache and nginx from the signed apt/yum repository (see <a href="/docs/installation-module/" class="text-interactive hover:text-interactive-hover underline">installation</a>); a Docker / nginx reverse proxy in front of any HTTP origin (Apache, Node.js, Caddy, IIS, your CDN\'s origin); and the ASP.NET Core middleware NuGet package (WeAmp.PageSpeed.AspNetCore). The same C++ optimization pipeline in all three. The IIS package ships from the 1.15 packaging channel.',
  },
  {
    q: 'How does the nginx integration work?',
    a: 'In reverse-proxy mode the module runs in front of your origin from the Docker image; the prebuilt nginx module ships inside it — there is no separate bare-metal module to install. Point the reverse proxy at your origin and all optimizations apply automatically.',
  },
  {
    q: 'Does it work with Apache?',
    a: 'Yes — as a reverse proxy. Put mod_pagespeed 2.1 in front of your Apache server with the Docker Compose setup, point <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">BACKEND_HOST</code> at your origin, and all optimizations apply automatically. If you want the in-process Apache module instead, it ships from the signed apt/yum repository — see <a href="/docs/installation-module/" class="text-interactive hover:text-interactive-hover underline">installation</a>.',
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
    a: 'The optimizer worker optimizes outside the request path, and its transforms are deliberately conservative: JS minification strips whitespace and comments — no variable renaming — and every image re-encode is checked against a perceptual quality metric. The design falls back to serving your original content when an optimization step fails. No software that rewrites live responses is risk-free, so test on staging before production. You stay in control either way: in the module, <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">pagespeed_disallow</code> skips a URL pattern and <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">pagespeed off</code> disables a location; on the worker, flags like <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">--disable-js</code> turn off a transform type, and a PURGE drops cached variants immediately. See <a href="/docs/configuration/" class="text-interactive hover:text-interactive-hover underline">configuration</a>.',
  },
  {
    q: 'Why not just use Cloudflare or another CDN?',
    a: "CDN-based optimization requires routing your traffic through a third-party proxy. mod_pagespeed runs on your servers — your visitors' content stays on your infrastructure, and the software sends nothing to us. Self-hosted by design; helps your GDPR posture and keeps you in control of caching, configuration, and data.",
  },
];
