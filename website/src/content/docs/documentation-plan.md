---
title: 'Where the Documentation Lives'
description: 'Where the mod_pagespeed 2.1 documentation lives, and what happens to the ModPageSpeed 2.0 and mod_pagespeed 1.15 pages.'
order: 62
group: 'Reference'
lastUpdated: 2026-09-18
---

All mod_pagespeed 2.1 documentation lives in this tree. This page explains
what that means for the pages you may have bookmarked.

## One tree under /docs

The mod_pagespeed 2.1 documentation lives here, under [/docs](/docs/). Pages
written for ModPageSpeed 2.0 are reworded into 2.1 documentation **in place,
at the same URLs** — the addresses do not carry a version number, so the
bookmark you already have keeps working.

Two audiences keep their own dedicated pages:

- **ModPageSpeed 2.0 Docker/Helm users** — the
  [migration guide](/docs/migrating-to-2-1/) is the path over.
- **ASP.NET Core middleware users** — the aspnet pages remain ModPageSpeed 2.0
  documentation; that audience has no migration to make.

## The 1.15 docs stay online

The [mod_pagespeed 1.15 documentation](/1.1/docs/) is not merged into this
tree. It stays online at its current URLs. Because 2.1 keeps the 1.15
directives and filter names, much of the configuration reference there still
applies — but check the current pages here first for a new deployment. New
documentation lands only here.

## Reporting gaps

A page that has not yet been reworded may lag the product. If a page
contradicts what your install does, that is a bug —
[tell us](/contact/).
