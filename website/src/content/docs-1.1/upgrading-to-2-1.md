---
title: 'Upgrading to mod_pagespeed 2.1'
description: 'mod_pagespeed 2.1 is the next release of the line: open source, and a drop-in upgrade for 1.15 configurations. How to move.'
order: 7
group: 'Getting Started'
lastUpdated: 2026-09-18
---

The mod_pagespeed line continues: its next release is **mod_pagespeed 2.1**,
the converged line — open source (Apache-2.0, like the original codebase)
and functionally 1.15-equivalent on its Core surface. Upgrading from 1.15 is
drop-in — 2.1 keeps the 1.14/1.15 directives and filter names, so existing
configuration carries over.

Platform notes:

- **IIS / Windows Server** — the package ships from the 1.15 packaging channel.
- **Debian 11 (bullseye)** — runs the 1.15-line module.
- **cPanel EasyApache 4 (EL8)** — the module runs without the optimizer
  worker.

## Upgrading

The upgrade is drop-in. mod_pagespeed 2.1 keeps the same directives and the
same filter names you run today — 1.14 and 1.15 configurations carry
over — and installs from the same signed package repository. See
[downloads](/1.1/docs/downloads/) for packages, and the
[getting started guide](/1.1/docs/getting-started/) for a fresh install.

If you run ModPageSpeed 2.0 (the Docker/Helm line), your path is the
[2.0 migration guide](/docs/migrating-to-2-1/) instead.

## Why converge

mod_pagespeed 2.1 is licensed under Apache-2.0 (like the original codebase) and
does everything 1.15 did — plus the optimizer worker. The standard signed
packages are free; what We-Amp sells is support — and hardened, attested
builds. If you want backing beyond the packages, see [support](/pricing/).

These docs stay online for reference. New documentation lives in the [mod_pagespeed 2.1 docs](/docs/).
