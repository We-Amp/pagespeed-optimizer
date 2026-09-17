---
title: 'Upgrading to mod_pagespeed 2.1'
description: 'mod_pagespeed 2.1 is the next release of the line: open source, and a drop-in upgrade for 1.15 configurations. How long 1.15 security support continues per platform, and how to move.'
order: 7
group: 'Getting Started'
lastUpdated: 2026-09-17
---

The mod_pagespeed line continues: its next release is **mod_pagespeed 2.1**,
the converged line — open source (Apache-2.0, like the original codebase)
and functionally 1.15-equivalent on its Core surface. Upgrading from 1.15 is
drop-in — 2.1 keeps the 1.14/1.15 directives and filter names, so existing
configuration carries over.

Feature work happens on mod_pagespeed 2.1. For 1.15 there are security
fixes only — no feature or behaviour changes — for the per-platform
transition windows below.

## Security-support windows, per platform

The general rule: a platform's 1.15 security support ends **no earlier than
six months after the converged line covers that platform**. For the Linux
Apache and nginx packages (deb and rpm), that is at least six months after
the mod_pagespeed 2.1 release.

Where a platform is not covered by the converged packages, 1.15 security
support continues until the condition named below is met — the clock only
starts once you have somewhere to go:

| Platform                           | 1.15 security support                                                                                                                                                          |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Linux — Apache and nginx (deb/rpm) | Ends no earlier than six months after the mod_pagespeed 2.1 release                                                                                                            |
| IIS / Windows Server               | Continues until a converged Windows package ships; the six-month window starts when that package ships                                                                         |
| Debian 11 (bullseye)               | Stays on 1.15-line content; support tracks Debian 11's own long-term support and ends with it — no converged package is planned                                                |
| cPanel EasyApache 4 on EL8         | The converged module runs there without the optimizer worker (optimization runs in-process) until the worker dependency reaches that channel; the six-month window starts then |

The windows above are floors, not targets: support does not end earlier
than stated.

## Upgrading

The upgrade is drop-in. mod_pagespeed 2.1 keeps the same directives and the
same filter names you run today — 1.14 and 1.15 configurations carry
over — and installs from the same signed package repository. See
[downloads](/1.1/docs/downloads/) for packages, and the
[getting started guide](/1.1/docs/getting-started/) for a fresh install.

If you run ModPageSpeed 2.0 (the Docker/Helm line), your path is the
[2.0 migration guide](/docs/migrating-to-2-1/) instead; 2.0 remains supported
until February 7, 2027.

## Why converge

mod_pagespeed 2.1 is licensed under Apache-2.0 (like the original codebase) and
does everything 1.15 did — plus the optimizer worker. The standard signed
packages are free; what We-Amp sells is support — and hardened, attested
builds. If you want backing beyond the packages, see [support](/pricing/).

These docs stay online for reference. New documentation lives in the [mod_pagespeed 2.1 docs](/docs/).
