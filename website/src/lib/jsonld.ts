// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Structured data that cannot close its own script tag.
 *
 * Every JSON-LD block the site emits is inlined into the page as
 * `<script type="application/ld+json" set:html={...}>`, and several of them
 * carry content-authored strings: FAQ answers come from frontmatter, filter
 * summaries from the docs collection. A string containing `</script>` would
 * otherwise end the script element early and inject the rest of the string as
 * markup — the classic script-context injection.
 *
 * `safeJsonLd(value)` serializes like `JSON.stringify` and then replaces every
 * `<` with its JSON unicode escape `\u003c`. The escape is valid JSON anywhere
 * a character can appear, so `JSON.parse` of the output yields exactly the
 * same object, while the bytes `</script` can never occur in the page. Only
 * `<` is escaped: JSON syntax has no other character that HTML tokenization
 * treats specially inside a script element.
 */
export function safeJsonLd(value: unknown): string {
  return JSON.stringify(value).replace(/</g, '\\u003c');
}
