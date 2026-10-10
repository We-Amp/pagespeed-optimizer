// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The /support/ page FAQ: what a subscription covers and how the signed
// artifacts are verified. Rendered visibly on the page and emitted as FAQPage JSON-LD from the
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
    a: `No. Every install runs the same software under the ${SOURCE_PUBLICATION.license}, with every optimization enabled by configuration, not by a plan. A subscription changes who answers when you need help, and for Priority and Enterprise, which repository your packages come from once the private package repository (being set up) exists.`,
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
    a: `Everyone gets security fixes as regular releases through the channel they installed from, listed under Security in the ${link('/docs/release-notes/', 'release notes')}. Subscribers get advance notice, with a delivery window stated in their quote. Everyone receives the update through the same public channels. A private package repository for Priority and Enterprise subscribers, with security fixes delivered before the public release, is being set up. Until then, subscribers receive the release SBOM and VEX on request and advance notice of security releases.`,
  },
  {
    q: 'What counts as a production server?',
    a: 'A server that serves production traffic with the module or the optimizer worker. You declare the band (up to 5, up to 25, or more) when you ask for a quote; there is no metering and the software never reports a count.',
  },
  {
    q: 'What is the private package repository?',
    a: 'A private package repository for Priority and Enterprise subscribers, with security fixes delivered before the public release, is being set up. Until then, subscribers receive the release SBOM and VEX on request and advance notice of security releases. The packages in it carry the same license as the standard packages.',
  },
  {
    q: 'What is signed today?',
    a: `The apt and yum repositories are GPG-signed, release packages carry .asc signatures, and the Windows installer is Authenticode-signed. The container images are cosign-signed and carry an SBOM attestation and build provenance. Release SBOM and VEX documents are available on request. Everything is the same software under the ${SOURCE_PUBLICATION.license}. See ${link('#hardened-builds', 'signed packages and images')}.`,
  },
  {
    q: 'Can I verify the standard packages without a subscription?',
    a: `Yes. The apt and yum repositories are GPG-signed, every release asset is listed in SHA256SUMS, and the SPDX SBOM and OpenVEX file are published in the source tree. The ${link('#verify', 'four-line recipe')} on this page checks the key and the checksums.`,
  },
  {
    q: 'What happens if a subscription lapses?',
    a: `Nothing happens to your deployment. The software keeps running and you keep upgrading from the public channels. Write to ${link(`mailto:${COMMERCIAL_EMAIL}`, COMMERCIAL_EMAIL)} when you want to pick it up again.`,
  },
];
