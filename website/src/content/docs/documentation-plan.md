---
title: 'Where the Documentation Lives'
description: 'Where the mod_pagespeed 2.1 documentation lives, and what happened to the pages written before the 2.0 re-architecture converged into it.'
order: 62
group: 'Reference'
lastUpdated: 2026-09-19
---

All mod_pagespeed 2.1 documentation lives in this tree. This page explains
what that means for the pages you may have bookmarked.

## One tree under /docs

The mod_pagespeed 2.1 documentation lives here, under [/docs](/docs/). Pages
written before the 2.0 re-architecture converged are reworded into 2.1
documentation **in place, at the same URLs** — the addresses do not carry a
version number, so the bookmark you already have keeps working.

Two audiences keep their own dedicated pages:

- **Docker and Helm users on the 2.0 stack** — the
  [migration guide](/docs/migrating-to-2-1/) is the path over.
- **ASP.NET Core middleware users** — the aspnet pages document
  `WeAmp.PageSpeed.AspNetCore`, the ASP.NET Core form of mod_pagespeed 2.1; that
  audience has no migration to make.

## The 1.15 docs are part of this tree now

The mod_pagespeed 1.15 reference material has been folded into the pages
above — installation, configuration, caching, and the filter reference now
cover both the module and the worker in one place. The
[1.15 history section](/docs/release-notes/#115-history) of the release
notes covers what changed release by release, with a pointer to the
archived version history.

## Reporting gaps

A page that has not yet been reworded may lag the product. If a page
contradicts what your install does, that is a bug —
[tell us](/contact/).
