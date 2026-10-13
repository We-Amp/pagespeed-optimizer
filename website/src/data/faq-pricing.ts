// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The /pricing/ FAQ: buying questions (what is free, what support covers,
// which tier, how to buy, VAT, what happens after payment or a lapse, what
// happens to existing licenses). Rendered visibly and as FAQPage JSON-LD from
// the same source. Technical questions live in the docs; the detail of each
// tier lives on /support/.

import type { FaqEntry } from './faq-shared';
import {
  COMMERCIAL_EMAIL,
  LICENSE_CLAUSE,
  LICENSING_TERMS_URL,
  SOURCE_PUBLICATION,
} from './product-facts.mjs';

const link = (href: string, text: string) =>
  `<a href="${href}" class="text-interactive hover:text-interactive-hover underline">${text}</a>`;

export const faqPricing: FaqEntry[] = [
  {
    q: 'Is the software really free?',
    a: `Yes. mod_pagespeed 2.1 is ${LICENSE_CLAUSE}: free to install and run, in development and in production, on any number of servers. Support is optional.`,
  },
  {
    q: 'What if I never subscribe?',
    a: 'You run the same software from the same signed public repositories as everyone else. Running without a subscription is a first-class choice.',
  },
  {
    q: 'What does support cover?',
    a: "Configuration, upgrades and incidents on the servers in your tier's band, for mod_pagespeed 2.1 on Apache, nginx, IIS, Docker and ASP.NET Core. It does not cover changes to your own application code or site content; that is the audit or a consulting engagement.",
  },
  {
    q: 'Which tier do I need?',
    a: `Count production servers: up to 5 is Standard, up to 25 is Priority, more than that (or one organization without a limit) is Enterprise. Choose Priority when your security team wants the release SBOM and VEX. ${link('/support/', 'What each tier includes')}.`,
  },
  {
    q: 'How do I buy?',
    a: 'Standard and Priority are bought at checkout. FastSpring, our reseller and merchant of record, takes the payment, charges the VAT that applies and issues the invoice. There is no separate invoice by email from us. Enterprise runs on a master agreement with purchase order and invoice.',
  },
  {
    q: 'Do I pay VAT?',
    a: 'Prices on this page exclude VAT. FastSpring adds the VAT that applies to you at checkout.',
  },
  {
    q: 'What happens after I pay?',
    a: "A reply from an engineer within your tier's response target, with any questions about your setup. The onboarding call and your ticket channel follow within five business days.",
  },
  {
    q: 'What happens if my subscription lapses?',
    a: `Nothing happens to your deployment. The software is yours under the ${SOURCE_PUBLICATION.license}: it keeps running and optimizing, and you keep upgrading from the public repositories.`,
  },
  {
    q: 'Will it break my site?',
    a: 'Transforms are deliberately conservative, and the design falls back to your original content when an optimization step fails. Test on staging, then disable any path, URL pattern or transform type with one line of config.',
  },
  {
    q: 'What if We-Amp disappears?',
    a: `Your deployment keeps running. The software is ${LICENSE_CLAUSE} and there is no kill switch in the request path, so the code stays yours to build, patch and fork.`,
  },
  {
    q: 'I have an IISpeed or mod_pagespeed 1.x license. What now?',
    a: `Your license moves to mod_pagespeed 2.1 at no cost, and paid terms are honored to expiry. Existing customers keep their current rate when they move to a support subscription. Write to ${link(`mailto:${COMMERCIAL_EMAIL}`, COMMERCIAL_EMAIL)} or use the ${link('/contact/?topic=iispeed-transfer', 'contact form')}.`,
  },
  {
    q: 'Where do the support terms live?',
    a: `At ${link(LICENSING_TERMS_URL, 'we-amp.com/licensing/')}. Subscriptions are governed by those support terms together with the ${link('/terms/', 'terms of service')}.`,
  },
];
