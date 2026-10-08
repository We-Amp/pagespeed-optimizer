// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The /pricing/ FAQ: buying questions only (what is free, why there is no
// number, how a quote works, what happens to existing licenses). Eight
// entries, rendered visibly and as FAQPage JSON-LD from the same source.
// Technical questions live in the docs; support-scope questions on /support/.
// No price amounts and no per-tier response targets anywhere in here.

import type { FaqEntry } from './faq-shared';
import {
  COMMERCIAL_EMAIL,
  LICENSE_CLAUSE,
  LICENSING_TERMS_URL,
  PRICING_ON_REQUEST,
  SOURCE_PUBLICATION,
} from './product-facts.mjs';

const link = (href: string, text: string) =>
  `<a href="${href}" class="text-interactive hover:text-interactive-hover underline">${text}</a>`;

export const faqPricing: FaqEntry[] = [
  {
    q: 'Is the software really free?',
    a: `Yes. mod_pagespeed 2.1 is ${LICENSE_CLAUSE}: free to install and run, in development and in production, on any number of servers, with no registration. What We-Amp sells is support, with hardened builds included from the Priority tier up.`,
  },
  {
    q: 'Why is there no price on this page?',
    a: `${PRICING_ON_REQUEST} Ask for a quote and you get a written number for your fleet within three business days.`,
  },
  {
    q: 'What does the early-subscriber rate mean?',
    a: 'If you subscribe before prices are published, the rate in your quote holds for three years, whatever the published price turns out to be.',
  },
  {
    q: 'Which tier do I need?',
    a: `The server band decides most of it: Standard covers up to 5 production servers, Priority up to 25, Enterprise one organization without a limit. Choose Priority or Enterprise when your security team wants hardened builds. ${link('/support/', 'What each tier includes')}.`,
  },
  {
    q: 'What happens after I ask for a quote?',
    a: 'We reply within one business day, send a written quote within three, and schedule the onboarding call within five business days of signing. Response targets by severity are part of the quote.',
  },
  {
    q: 'What happens if my subscription lapses?',
    a: `Nothing happens to your deployment. The software is yours under the ${SOURCE_PUBLICATION.license}: it keeps running, keeps optimizing, and you keep upgrading from the public channels.`,
  },
  {
    q: 'I bought an IISpeed or mod_pagespeed 1.x license. What now?',
    a: `Your license moves to mod_pagespeed 2.1 at no cost and paid terms are honored to expiry. To transfer or renew into a support subscription, use the ${link('/contact/?topic=iispeed-transfer', 'contact form')} or write to ${link(`mailto:${COMMERCIAL_EMAIL}`, COMMERCIAL_EMAIL)}.`,
  },
  {
    q: 'Where do the support terms live?',
    a: `At ${link(LICENSING_TERMS_URL, 'we-amp.com/licensing/')}. Subscriptions are governed by those support terms together with the ${link('/terms/', 'terms of service')}.`,
  },
];
