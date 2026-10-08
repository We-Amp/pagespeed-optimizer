// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The /support/ page FAQ: what a subscription covers and how hardened builds
// work. Rendered visibly on the page and emitted as FAQPage JSON-LD from the
// same entries, so the structured data never says more than the page does.
// Answers may contain HTML; JSON-LD callers pipe them through stripHtml().
// No price amounts and no per-tier response-time targets (the quote states
// them).

import type { FaqEntry } from './faq-shared';
import { COMMERCIAL_EMAIL, SOURCE_PUBLICATION } from './product-facts.mjs';

const link = (href: string, text: string) =>
  `<a href="${href}" class="text-interactive hover:text-interactive-hover underline">${text}</a>`;

export const faqSupport: FaqEntry[] = [
  {
    q: 'Does a subscription change what the software does?',
    a: `No. Every install runs the same software under the ${SOURCE_PUBLICATION.license}, with every optimization enabled by configuration, not by a plan. A subscription changes who answers when you need help, and for Priority and Enterprise, which repository your packages come from.`,
  },
  {
    q: 'Who answers a ticket?',
    a: 'The engineers who build mod_pagespeed 2.1. There is no first-line desk in front of them: the person reading your ticket can read the code path behind it.',
  },
  {
    q: 'When do you answer?',
    a: 'On CET business days. There is no 24x7 desk. Response targets by severity, including any out-of-hours terms for Enterprise, are stated in your quote.',
  },
  {
    q: 'How do security updates reach me?',
    a: `Everyone gets security fixes as regular releases through the channel they installed from, listed under Security in the ${link('/docs/release-notes/', 'release notes')}. Subscribers get advance notice, with a delivery window stated in their quote. Priority and Enterprise subscribers receive the update through the subscriber repository before the public release.`,
  },
  {
    q: 'What counts as a production server?',
    a: 'A server that serves production traffic with the module or the optimizer worker. You declare the band (up to 5, up to 25, or more) when you ask for a quote; there is no metering and the software never reports a count.',
  },
  {
    q: 'What does "hardened" mean for the builds?',
    a: `The same source, built through a hardened build pipeline and delivered through the subscriber repository, with security updates ahead of the public release and targets rebuilt on request. The artifacts carry the same ${SOURCE_PUBLICATION.license} as the standard packages. See ${link('#hardened-builds', 'hardened builds')}.`,
  },
  {
    q: 'Can I verify the standard packages without a subscription?',
    a: `Yes. The apt and yum repositories are GPG-signed, every release asset is listed in SHA256SUMS, and the SPDX SBOM is published in the source tree. The ${link('#verify', 'four-line recipe')} on this page checks the key and the checksums.`,
  },
  {
    q: 'What happens if a subscription lapses?',
    a: `Nothing happens to your deployment. The software keeps running and you keep upgrading from the public channels. Write to ${link(`mailto:${COMMERCIAL_EMAIL}`, COMMERCIAL_EMAIL)} when you want to pick it up again.`,
  },
];
