// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// mod_pagespeed 1.1-specific FAQ entries.
//
// Voice register: service / reassurance tier — visitors here are usually
// already running open-source mod_pagespeed and evaluating the maintained
// continuation. Answers favor stability cues over architectural pitch.
// Appended to faq-shared on /1.1/ marketing surfaces.

import type { FaqEntry } from './faq-shared';
import { LICENSE_CLAUSE } from './product-facts.mjs';

export const faq11: FaqEntry[] = [
  {
    q: 'Is mod_pagespeed still maintained?',
    a: `Yes. The last upstream release shipped in 2020 and the GitHub repository was archived in 2025. Active development continued at <a href="https://we-amp.com/" class="text-interactive hover:text-interactive-hover underline">We-Amp B.V.</a> — the Dutch company that helped build ngx_pagespeed, maintained mod_pagespeed, and drove the project's Apache incubation. mod_pagespeed 1.15 continued the line, and the converged <a href="/" class="text-interactive hover:text-interactive-hover underline">mod_pagespeed 2.1</a> — ${LICENSE_CLAUSE} — is its drop-in upgrade. See the <a href="/1.1/docs/upgrading-to-2-1/" class="text-interactive hover:text-interactive-hover underline">upgrade guide</a> to move.`,
  },
  {
    q: 'Are the known CVEs against the last upstream release patched?',
    a: 'Yes. mod_pagespeed 1.15 ships with patches for the known CVEs that accumulated against the archived upstream, and continues to receive security fixes. If your security team needs a current CVE statement for procurement, <a href="/contact/" class="text-interactive hover:text-interactive-hover underline">contact us</a>.',
  },
  {
    q: 'Will my existing pagespeed.conf keep working?',
    a: 'Yes. mod_pagespeed 1.15 is a drop-in continuation of the open-source project. All existing <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">pagespeed</code> directives work unchanged — swap the binary, keep your config.',
  },
  {
    q: 'How is 1.15 different from the last upstream open-source release?',
    a: "Same filters, same directives, same in-process architecture. What's added: ongoing CVE patches, a native IIS module, a GA nginx dynamic module (signed apt packages for Debian 11/12/13 and Ubuntu 22.04/24.04 on amd64 and arm64, plus AlmaLinux/RHEL/Rocky 9 on x86_64 + aarch64 and 10 on x86_64 via yum), the Cyclone shared-memory cache (replacing the old file cache, no config change), and direct email support. The last upstream release shipped in 2020 and the GitHub repository was archived in 2025; 1.15 is the actively maintained branch.",
  },
  {
    q: 'Which web servers does 1.15 support?',
    a: 'Apache (drop-in replacement), nginx (dynamic module), and IIS (native Windows Server module) all ship as GA packages today. The signed apt packages cover nginx and Apache on Debian 11, 12, and 13 and Ubuntu 22.04 and 24.04, on both amd64 and arm64; the yum packages cover AlmaLinux/RHEL/Rocky 9 on x86_64 and aarch64, and 10 on x86_64. Each module is built against its distro\'s stock nginx, so if you run a different nginx version — for example nginx.org\'s stable or mainline — <a href="/contact/" class="text-interactive hover:text-interactive-hover underline">contact us</a> for a matching build. Envoy (HTTP filter) is experimental.',
  },
  {
    q: 'Should I run 1.15 or the converged mod_pagespeed 2.1?',
    a: `On Apache and nginx, run <a href="/" class="text-interactive hover:text-interactive-hover underline">mod_pagespeed 2.1</a> — the drop-in upgrade with the same directives, plus the optimizer worker, ${LICENSE_CLAUSE}. Stay on 1.15 where the converged packages do not cover you yet: the IIS package ships from the 1.15 packaging channel. See the <a href="/1.1/docs/upgrading-to-2-1/" class="text-interactive hover:text-interactive-hover underline">upgrade guide</a> for details.`,
  },
  {
    q: 'Can I run mod_pagespeed 1.15 under ASP.NET Core?',
    a: 'Yes, on Linux. The <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">WeAmp.PageSpeed.Sidecar</code> NuGet package adds mod_pagespeed 1.15 to your Kestrel app via middleware, with a bundled nginx + ngx_pagespeed optimizer running on loopback behind it — <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">AddPageSpeed()</code> / <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">UsePageSpeed()</code> wire it in, and a single <code class="rounded bg-bg-elevated px-1 py-0.5 text-xs">dotnet add package</code> pulls the bundled native binaries. It is Linux-only (linux-x64, linux-arm64). See the <a href="/1.1/docs/aspnet-sidecar/" class="text-interactive hover:text-interactive-hover underline">ASP.NET Core sidecar guide</a>. For cross-platform, in-process optimization with SVG auto-vectorization and Jpegli, use the <a href="/docs/aspnet-getting-started/" class="text-interactive hover:text-interactive-hover underline">ModPageSpeed 2.0 middleware</a> instead.',
  },
];
