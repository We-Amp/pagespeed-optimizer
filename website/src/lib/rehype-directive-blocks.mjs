// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * rehype plugin: give the generated directive reference a structure a script
 * and a stylesheet can work with.
 *
 * The reference region of the configuration page is flat Markdown: an `h3` per
 * area, an `h4` per directive, and the directive's list and paragraphs as
 * siblings. This plugin groups that flat run at render time:
 *
 *   <div data-directive-reference>
 *     …the region's introduction…
 *     <div data-directive-finder>…label, input, status…</div>
 *     <section data-directive-area>
 *       <h3>…</h3>
 *       <p data-directive-index>…links to the area's directives…</p>
 *       <div data-directive="Name"><h4 id>…</h4>…</div>
 *       …
 *     </section>
 *     …
 *   </div>
 *
 * Only the rendered HTML changes. The Markdown source, the generated region
 * in it, the `.md` twin of the page and `llms-full.txt` are untouched, and
 * every heading keeps its id. A page without the region is left alone.
 *
 * It runs before rehype-raw, so HTML written in the Markdown (comments, the
 * `<a id>` anchors) is still `raw` nodes here.
 */

export const REGION = 'module-directives';

/** The finder's visible strings. */
export const FINDER_LABEL = 'Find a directive';

/** @param {import('hast').RootContent} node */
const isHeading = (node, tag) => node.type === 'element' && node.tagName === tag;

/** @param {import('hast').RootContent} node */
const isMarker = (node, edge) =>
  node.type === 'raw' && node.value.trim() === `<!-- generated:${edge} ${REGION} -->`;

/** @param {import('hast').Nodes} node @returns {string} */
function text(node) {
  if (node.type === 'text') return node.value;
  return 'children' in node ? node.children.map(text).join('') : '';
}

/**
 * @param {string} tagName
 * @param {Record<string, unknown>} properties
 * @param {import('hast').ElementContent[]} children
 * @returns {import('hast').Element}
 */
const el = (tagName, properties, children) => ({ type: 'element', tagName, properties, children });

const newline = () => /** @type {import('hast').Text} */ ({ type: 'text', value: '\n' });

/** The finder: a labelled input and the line that reports the result. */
function finder() {
  return el('div', { className: ['directive-finder'], dataDirectiveFinder: '' }, [
    el('label', { htmlFor: 'directive-filter' }, [{ type: 'text', value: FINDER_LABEL }]),
    el(
      'input',
      {
        id: 'directive-filter',
        type: 'search',
        autoComplete: 'off',
        autoCapitalize: 'off',
        spellCheck: 'false',
        ariaDescribedBy: 'directive-filter-status',
      },
      [],
    ),
    el('p', { id: 'directive-filter-status', role: 'status' }, []),
  ]);
}

export default function rehypeDirectiveBlocks() {
  return (/** @type {import('hast').Root} */ tree) => {
    const begin = tree.children.findIndex((n) => isMarker(n, 'begin'));
    if (begin === -1) return;
    const end = tree.children.findIndex((n, i) => i > begin && isMarker(n, 'end'));
    if (end === -1) return;

    /** @type {import('hast').ElementContent[]} */
    const intro = [];
    /** @type {import('hast').Element[]} */
    const areas = [];
    /** @type {import('hast').Element | null} */
    let area = null;
    /** @type {import('hast').Element | null} */
    let block = null;

    for (const node of /** @type {import('hast').ElementContent[]} */ (
      tree.children.slice(begin + 1, end)
    )) {
      if (isHeading(node, 'h3')) {
        block = null;
        area = el('section', { dataDirectiveArea: '' }, [node]);
        areas.push(area);
      } else if (area && isHeading(node, 'h4')) {
        block = el('div', { dataDirective: text(node).trim() }, [node]);
        area.children.push(block);
      } else if (block) {
        block.children.push(node);
      } else if (area) {
        // Between the area heading and its first directive: the links to the
        // area's directives.
        if (isHeading(node, 'p')) node.properties = { ...node.properties, dataDirectiveIndex: '' };
        area.children.push(node);
      } else {
        intro.push(node);
      }
    }
    if (areas.length === 0) return;

    const reference = el('div', { dataDirectiveReference: '' }, [
      ...intro,
      finder(),
      newline(),
      ...areas.flatMap((a) => [a, newline()]),
    ]);
    tree.children.splice(begin + 1, end - begin - 1, newline(), reference, newline());
  };
}
