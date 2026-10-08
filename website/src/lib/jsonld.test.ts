// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { safeJsonLd } from './jsonld';

describe('safeJsonLd', () => {
  it('serializes like JSON.stringify when nothing needs escaping', () => {
    const obj = { '@type': 'WebSite', name: 'modpagespeed.com' };
    expect(safeJsonLd(obj)).toBe(JSON.stringify(obj));
  });

  it('cannot be closed by a FAQ answer containing </script>', () => {
    // The shape BaseLayout inlines: a FAQPage whose acceptedAnswer text is
    // content-authored frontmatter. An answer that names the closing tag must
    // not be able to end the script element it lives in.
    const faq = {
      '@context': 'https://schema.org',
      '@type': 'FAQPage',
      mainEntity: [
        {
          '@type': 'Question',
          name: 'Is it safe to write </script> in an answer?',
          acceptedAnswer: {
            '@type': 'Answer',
            text: 'Yes — the string </script> stays inside the JSON block.',
          },
        },
      ],
    };
    const out = safeJsonLd(faq);
    expect(out).not.toContain('</script');
    expect(out).not.toContain('<');
    // Still one JSON value, and the same one: the answer round-trips intact.
    expect(JSON.parse(out)).toEqual(faq);
  });

  it('escapes < everywhere it appears, in keys and nested values alike', () => {
    const out = safeJsonLd({ 'a<b': ['<img', { c: '1<2' }] });
    expect(out).not.toContain('<');
    expect(JSON.parse(out)).toEqual({ 'a<b': ['<img', { c: '1<2' }] });
  });
});
