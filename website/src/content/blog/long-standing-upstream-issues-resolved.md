---
title: 'Long-standing upstream issues, resolved'
description: 'Dozens of open issues in the archived mod_pagespeed and ngx_pagespeed trackers are resolved in current mod_pagespeed. Each is listed with its fix.'
date: 2026-10-30
tags: ['history', 'ngx_pagespeed', 'nginx', 'apache', 'release']
draft: false
---

The Apache Incubator trackers for mod_pagespeed and ngx_pagespeed are archived.
Between them they still hold nearly 900 open issues (524 and 353 in October
2026), and an archived tracker cannot close any of them, so they stay open
even after the problem is gone. We went through every one of them, and for those that
looked fixed we checked the current code and the CHANGELOG. 41 issues are
resolved in current mod_pagespeed, each tied to a specific change and
release. The full list, with links, is on the new docs page
[Long-standing upstream issues resolved](/docs/upstream-issues-resolved/).

Some of them, briefly:

- **Building ngx_pagespeed against a current nginx.** Issues from 2016 to
  2022 report compile failures with newer nginx, newer glibc, or as a dynamic
  module. Resolved in mod_pagespeed 1.1.0: the stock nginx of each supported
  distribution gets a prebuilt, signed module, so there is nothing to compile,
  and the released source builds against the nginx versions we support, listed
  on the [worker page](/docs/worker-configuration/#which-nginx-version).
- **ARM64.** Reported in 2016 against ngx_pagespeed ("Supporting pagespeed on
  ARM devices?"); resolved in 1.1.0 with arm64/aarch64 builds for Apache and
  nginx on Linux.
- **The archived Google package repository.** Reported in 2020 and 2022 as GPG
  and "no longer signed" errors from apt; resolved by a maintained, signed
  repository, and since 2.5.0 `apt-get install mod-pagespeed` replaces the
  archived package in one step.
- **A deadlock in shared-memory statistics.** Reported in 2018: a server child
  that died while holding a shared-memory lock left every other process
  waiting until a restart. Resolved in 2.2.0: the next process takes the lock
  over.
- **Native lazy loading.** Requested in 2019 against ngx_pagespeed, and in
  2020 against mod_pagespeed by sites whose PageSpeed Insights report still
  flagged offscreen images. Resolved in 1.15.0+r18: `lazyload_images` can write
  `loading="lazy"` instead of swapping in a placeholder image.
- **The file cache.** Reports from 2016 to 2019 about the cache cleaner loading
  the disk, the cache growing past its limit and logs filling with lock-file
  errors. Resolved in 1.1.0, where Cyclone Cache, a fixed-size, memory-mapped
  cache with no cleanup passes, replaced the file cache.

The page links every row to the CHANGELOG entry, the release notes or the
source file that carries the change. We left out issues we could not tie to a
specific change, and the rows we could not confirm against the reporter's
setup are marked "likely". If one of the listed issues still happens to
you on a current release, or you hit another one from those trackers, open an
issue in [We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed/issues)
with your version and a reproduction.
