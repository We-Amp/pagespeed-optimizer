// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The build marker: a short id that names THIS checkout. astro.config.mjs
// bakes it into every page as <meta name="build-id">, and playwright.config.ts
// asserts it on the served home page before any test runs, so a run that is
// pointed at another checkout's server (a sibling worktree, a stale preview)
// fails loudly instead of passing or failing for the wrong reasons.
// SITE_BUILD_ID overrides the derived value (CI, or a deliberate shared server).

import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const websiteRoot = fileURLToPath(new URL('../..', import.meta.url));

export const BUILD_ID =
  process.env.SITE_BUILD_ID || createHash('sha1').update(websiteRoot).digest('hex').slice(0, 12);
