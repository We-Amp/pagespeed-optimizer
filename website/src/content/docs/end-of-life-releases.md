---
title: 'End-of-life releases: 1.14.36.1 and earlier'
description: 'Advisory for the archived mod_pagespeed and ngx_pagespeed releases, 1.14.36.1 and earlier: what is known, how to check a server, what to do.'
order: 17
group: 'Upgrade and migrate'
lastUpdated: 2026-11-17
datePublished: 2026-11-17
---

The archived mod_pagespeed and ngx_pagespeed releases, 1.14.36.1 and earlier, have
had no release from the original project since 2020 and receive no security fixes.
They compile their own copies of several image libraries into the module, so an
operating-system update does not replace those copies. If a server still runs one of
these releases, remove the module or move to a build that still receives security
fixes.

mod_pagespeed is an open-source project originally developed at Google. The current
mod_pagespeed is developed by We-Amp B.V. and is not affiliated with or endorsed by
Google.

## Which releases this covers {#scope}

- Every release of the original project. The last two are 1.13.35.2 (February 2018)
  and 1.14.36.1 (2020); all earlier releases are covered too.
- Builds made from the original project's unreleased source after 1.14.36.1. They
  identify themselves as `1.15.0.0-` followed by a number.

It does not cover the current mod_pagespeed. For that, see the
[security updates](/docs/release-notes/#security-updates) in the release notes.

## How to check a server {#check}

The module adds a response header to the pages it serves: `X-Mod-Pagespeed` on Apache
and `X-Page-Speed` on nginx.

```bash
curl -s -o /dev/null -D - https://www.example.com/ | grep -i -E 'x-mod-pagespeed|x-page-speed'
```

| Header value                                                              | What it means                                                                                 |
| ------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| Four numbers up to `1.14.36.1`, such as `1.13.35.2-0` or `1.14.36.1-0`    | A release of the original project. End of life.                                               |
| `1.15.0.0-` and a number                                                  | A build of the original project's unreleased source. End of life.                             |
| Three numbers, such as `2.8.2`                                            | The current mod_pagespeed. Not covered by this page.                                          |
| `on`, `1`, or other text                                                  | The version is hidden. Check the installed package or ask whoever built the server.           |

The header shows which release a build calls itself. It does not show what is inside.
A third party can rebuild an old release with newer libraries and keep the version
number, and an operator can set the header to any text. If your module came from a
packager other than the original project, ask the packager which library versions it
contains.

## What is known {#known}

- **The image libraries are frozen at release time.** 1.13.35.2 builds in libwebp
  0.5.1 and 1.14.36.1 builds in libwebp 0.6.1. The copies of libpng, libjpeg-turbo,
  giflib and zlib in these releases also date from 2016 and 2017.
- **CVE-2023-4863 applies to those libwebp versions.** The CVE record lists every
  libwebp version before 1.3.2 as affected.
- **A system update does not change the module.** The original build compiles libwebp
  from its own source tree into the module and has no option to use a system copy.
- **WebP decoding is on with default settings.** With the default filter set, the
  module decodes the WebP images it optimizes.
- **Other fixes have been published since.** The current mod_pagespeed has fixed
  further defects inherited from these releases: memory-safety issues in image
  processing, and denial-of-service and cross-site-scripting issues in the module.
  They are listed by impact and affected version under
  [security updates](/docs/release-notes/#security-updates).

## What is not known {#not-known}

- **Whether CVE-2023-4863 can be exploited through the module.** We have not
  demonstrated it and know of no report of it. An attacker would need a crafted image
  to be served from a location the module optimizes, for example on a site that
  accepts image uploads.
- **Whether a given site is exposed.** That depends on its configuration and on where
  its images come from. Only the operator can judge it.
- **What third-party builds contain.** We have not assessed builds by other packagers.

## What to do {#what-to-do}

1. **Remove the module if nothing depends on it.** On Apache, remove the
   `mod-pagespeed-stable` or `mod-pagespeed-beta` package and restart. On nginx,
   remove the `pagespeed` directives and install an nginx build without the module.
2. **Or move to a build that still receives security fixes.** The current
   mod_pagespeed keeps the directives and the filter names. See
   [Migrate from the archived Google module](/docs/migrate-from-google-mod-pagespeed/)
   and [Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/). Test in
   staging first.

## About this advisory {#about}

We-Amp develops the current mod_pagespeed and earns its income from support for it,
so we are not a neutral party. This page makes no offer. If something on it is wrong
or incomplete, tell us at
[security@modpagespeed.com](mailto:security@modpagespeed.com) and we will correct it.
