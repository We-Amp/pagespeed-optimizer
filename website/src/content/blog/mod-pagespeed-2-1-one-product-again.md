---
title: 'mod_pagespeed 2.1: one product again'
description: 'The mod_pagespeed lineage and the ModPageSpeed 2.0 engine converge into one open-source product: the module you know, plus a separate optimizer worker. Apache-2.0, drop-in for 1.14/1.15 configs.'
date: 2026-09-17
author: 'Otto van der Schaaf'
tags: ['announcement', 'release']
draft: false
---

For the past year we shipped two products: mod_pagespeed 1.15, the maintained
continuation of the classic module, and ModPageSpeed 2.0, a ground-up rewrite
of the optimization engine. Today they are one product again.

**mod_pagespeed 2.1** is the converged line: the native web-server module for
Apache and nginx, with the 2.0 engine running alongside it as a separate
optimizer worker. It is open source under the Apache-2.0 license — the same
license the original codebase carried — and it is a drop-in for your existing
configuration: 1.14 and 1.15 directives and filter names carry over.

## What actually converged

The module stays. If you run mod_pagespeed today, 2.1 keeps the
`pagespeed`-directive surface and the 1.14/1.15 filter names on its Core
surface — swap the packages and your `pagespeed.conf` carries over. On the
supported Linux deb/rpm channels the module and the `pagespeed-optimizer`
worker install from the same signed repository at packages.modpagespeed.com.

The optimizer moved out. The heavy work — image transcoding and the rest of
the rewrite pipeline — now runs in the worker, off the request path, writing
optimized variants back to the shared cache the module serves from. That
worker _is_ the ModPageSpeed 2.0 engine: the image pipeline with ML-predicted
quality and SSIMULACRA2 verification, variant-aware caching, and the
optimizations the rewrite was built for, now behind the module everyone
already runs.

## Why the architecture is worth having

A web optimizer decodes untrusted bytes: every image it recompresses and
every page it parses arrived from somewhere else. In mod_pagespeed 2.1 that
optimization work runs in a dedicated `pagespeed-optimizer` process, under
its own unprivileged system user with an empty capability set, and the
module reaches its results through the shared cache. Browser-based analysis
runs in that same worker process, with the Chrome sandbox required by
default.

## Browser analysis and the agentic web, for everyone

Browser-based analysis (critical-CSS validation, waterfall capture, visual
comparison) and the agent-readability tooling that makes your pages legible
to AI assistants as well as browsers ship in 2.1 for everyone — one product,
one feature set, on every platform the converged packages cover.

## Open source, Apache-2.0

mod_pagespeed 2.1 is open source under the Apache-2.0 license, like the
original codebase — and the relicensing is retroactive, covering the
predecessor lines as well. The source lives at
[github.com/We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed).
There are no editions and no usage registration, and the standard signed
packages are free. What's for sale is support and hardened, attested builds: [SLA-backed plans, enterprise attestation with hardened builds, and a
hoster partner program](/pricing/) from the people who build it.

## What this means for you

- **Running mod_pagespeed 1.15?** The line continues — 2.1 is its next
  release, and the upgrade is drop-in. 1.15 now receives security fixes only,
  for per-platform transition windows: Linux deb/rpm users have at least six
  months, and IIS stays covered until a converged Windows package ships. The
  [upgrade guide](/1.1/docs/upgrading-to-2-1/) has the details.
- **Running ModPageSpeed 2.0?** The Docker/Helm line is feature-frozen and
  supported until February 7, 2027; the
  [migration guide](/docs/migrating-to-2-1/) walks the move. The ASP.NET Core
  middleware is supported until the same date.
- **New here?** [Install it](/docs/getting-started/) — two packages, your
  existing web server, free in development and in production.

---

mod_pagespeed is an open-source project originally developed at Google.
We-Amp helped build ngx_pagespeed, maintained mod_pagespeed, and drove the
project's Apache incubation throughout the Google era; mod_pagespeed 2.1 is
developed by We-Amp B.V. and is not affiliated with or endorsed by Google.
