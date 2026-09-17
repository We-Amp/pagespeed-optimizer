// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { LICENSE_CLAUSE, SOURCE_PUBLICATION } from './product-facts.mjs';

// Shared FAQ entries — the support-model questions that apply across the
// converged line and the predecessor lines.
//
// Rendered into the pricing/support page (with faq-2 appended) and into the
// 1.1 marketing FAQ (with faq-1-1 appended). Answers may contain HTML;
// JSON-LD callers should pipe through stripHtml() to get plain text
// for the Schema.org Answer.text field.

export interface FaqEntry {
  q: string;
  a: string;
}

export const faqShared: FaqEntry[] = [
  {
    q: 'Is the software really free?',
    a: `Yes. mod_pagespeed 2.1 is ${LICENSE_CLAUSE}: free to install and run, in development and in production, with no usage registration. The standard signed packages cost nothing; what We-Amp sells is support — and hardened, attested builds.`,
  },
  {
    q: 'What does a support subscription buy?',
    a: 'Backing from the people who build the product: a direct channel, agreed response times for configuration, upgrade and incident questions, and SLA-backed delivery of security updates. Hosting providers get the same subscription shaped for a fleet. The software is the same whether or not you hold a subscription.',
  },
  {
    q: 'What are hardened builds?',
    a: `Builds of the same code the standard packages are built from, produced through a hardened build pipeline with a signed SBOM and build provenance, delivered through the subscriber repository. They differ in how they are built and attested — never in features. Sold as a subscription; pricing is to be announced. The artifacts themselves carry the same ${SOURCE_PUBLICATION.license} as the standard packages; a subscription covers access to the subscriber repository.`,
  },
  {
    q: 'When can I buy a support plan or hardened builds?',
    a: 'Pricing is being finalized. Until it is published, talk to us directly at <a href="mailto:sales@we-amp.com" class="text-interactive hover:text-interactive-hover underline">sales@we-amp.com</a>.',
  },
  {
    q: 'What happens if my support plan lapses?',
    a: `Nothing happens to your deployment. The software is yours under the ${SOURCE_PUBLICATION.license} — it keeps running, keeps optimizing, and you can keep upgrading.`,
  },
  {
    q: 'Do containers, replicas, or autoscaling count against anything?',
    a: 'No. The software is free, with no per-server, per-container, or per-request metering of any kind.',
  },
  {
    q: 'The original mod_pagespeed costs nothing. What changed?',
    a: `Nothing, on that front: the maintained continuation is free too, under the same ${SOURCE_PUBLICATION.license} the original codebase carried. What changed is that it is maintained again — security fixes, a current toolchain, and the optimizer worker — and that the maintenance is funded by support plans and hardened-build subscriptions.`,
  },
  {
    q: 'Where do the support terms live?',
    a: 'At <a href="https://we-amp.com/licensing/" class="text-interactive hover:text-interactive-hover underline">we-amp.com/licensing/</a> — the We-Amp terms page.',
  },
];

export function stripHtml(html: string): string {
  return html.replace(/<[^>]+>/g, '');
}
