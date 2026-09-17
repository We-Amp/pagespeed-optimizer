// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Single source of truth for the schema.org offer node emitted across the site.
//
// Converged model: the standard signed packages are free, so the node is a
// single $0 Offer. Google
// requires a Product node to carry offers/review/aggregateRating — silently
// dropping offers would invalidate the Product markup on every importing page,
// and a $0 Offer is accurate (the standard packages cost nothing) and
// rich-result-valid. (The pre-GA AggregateOffer spanned the paid per-site
// license rungs; the license ladder is retired.) When hardened-build prices
// are published, graduate to an AggregateOffer with lowPrice 0 — never emit a
// price-less Offer.
//
// Centralizes `priceValidUntil` (recommended by Google for offer eligibility)
// so it doesn't drift across pages, and keeps the description derived from
// product-facts.mjs — no price, license or support literals in this file.

import { LICENSE_CLAUSE_CAP, SUPPORT_URL, PRICE_VALID_UNTIL } from './product-facts.mjs';

// Re-exported from product-facts.mjs. Google treats a stale priceValidUntil as
// expired, so the canonical value is bumped ~yearly in product-facts.mjs.
export { PRICE_VALID_UNTIL };

// The one sentence every offer node carries. The license clause comes from
// product-facts.mjs (publication-derived: "licensed under" today, "open source
// under" once the source is published); hardened builds and support plans are
// sold separately and are deliberately not part of this offer (the standard
// packages cost $0).
export const DEFAULT_OFFER_DESCRIPTION =
  `${LICENSE_CLAUSE_CAP} — the standard signed packages are free ` +
  `to install and run. Hardened builds and support plans are sold separately: ${SUPPORT_URL}`;

// Return policy intentionally omitted from structured data — /terms/ grants no
// unconditional money-back guarantee; the EU 14-day withdrawal right is
// conditional and extinguishable on use (Art 16(m)). Re-add a
// MerchantReturnPolicy ONLY if a real 14-day money-back guarantee is adopted and
// stated on /terms/ + /pricing/ (owner decision).

export interface OfferOptions {
  /** Canonical URL for this offer (defaults to the pricing/support page). */
  url?: string;
  /** Human-readable offer description (defaults to the license line). */
  description?: string;
}

/**
 * Build the canonical schema.org Offer node: the standard packages are free ($0).
 * Pass `url`/`description` to keep a page's existing values; everything else
 * (price, currency, priceValidUntil, availability) comes from the shared
 * constants above.
 */
export function offerJsonLd(opts: OfferOptions = {}) {
  return {
    '@type': 'Offer',
    availability: 'https://schema.org/InStock',
    price: 0,
    priceCurrency: 'USD',
    priceValidUntil: PRICE_VALID_UNTIL,
    url: opts.url ?? 'https://modpagespeed.com/pricing/',
    description: opts.description ?? DEFAULT_OFFER_DESCRIPTION,
  };
}
