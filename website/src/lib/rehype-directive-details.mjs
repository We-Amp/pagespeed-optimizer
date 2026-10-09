// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// rehype-directive-details.mjs — collapse the directive reference.
//
// /docs/configuration/ lists every module directive as an `####` section under
// one of the `###` area headings. The rendered page is ~6,800 DOM nodes, and
// that size alone is its mobile FCP/LCP cost (the page has no blocking time).
// This rehype step, scoped to that one source file, wraps each directive
// section — the h4 and every node after it until the next h2/h3/h4 — in a
// native `<details class="directive-details">` whose `<summary>` holds the h4
// itself. The heading keeps its tag, level and id, so every anchor, the
// heading permalinks, the on-this-page rail and search keep working; the page
// just stops laying out ~5,000 nodes on first paint. Deep links are opened by
// the small inline script in DirectiveDetailsDeepLink.astro.
//
// Content between directives that is not part of a directive's own block (the
// area heading, the area's index paragraph of links) stays outside any
// details, so the collapsed page still reads as a browsable index.
//
// Scoped by source path, not frontmatter: the markdown content stays
// untouched, and the Markdown twin (/docs/configuration.md, rendered by
// src/lib/docs-markdown.mjs from the raw source) never sees this transform.

const CONFIGURATION_SOURCE = /[/\\]src[/\\]content[/\\]docs[/\\]configuration\.md$/;

/** A node that ends the current directive section. */
function endsSection(node) {
  return node.type === 'element' && ['h2', 'h3', 'h4'].includes(node.tagName);
}

/**
 * @returns {import('unified').Transformer}
 */
export function rehypeDirectiveDetails() {
  return (tree, file) => {
    const source = file.path ?? file.history?.[file.history.length - 1] ?? '';
    if (!CONFIGURATION_SOURCE.test(source)) return;

    const out = [];
    for (let i = 0; i < tree.children.length; i++) {
      const node = tree.children[i];
      if (node.type !== 'element' || node.tagName !== 'h4') {
        out.push(node);
        continue;
      }
      // The section: the heading plus everything until the next section
      // boundary. Whitespace text nodes between sections are absorbed.
      const body = [];
      let j = i + 1;
      while (j < tree.children.length && !endsSection(tree.children[j])) {
        body.push(tree.children[j]);
        j++;
      }
      out.push({
        type: 'element',
        tagName: 'div',
        properties: { className: ['directive-row'] },
        children: [
          {
            type: 'element',
            tagName: 'details',
            properties: { className: ['directive-details'] },
            children: [
              {
                type: 'element',
                tagName: 'summary',
                properties: {},
                // The heading element itself — not a copy of its text — so the
                // section has exactly one heading in the DOM and the a11y tree.
                children: [node],
              },
              ...body,
            ],
          },
        ],
      });
      i = j - 1;
    }
    tree.children = out;
  };
}
