// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// filter-topics.mjs — the data behind the per-filter topic pages
// (/docs/filters/<name>/, src/pages/docs/filters/[name].astro).
//
// The prose has ONE source: the filter's section on its group page
// (src/content/docs/{image,css,javascript,html}-filters.md and
// cache-control.md). This module reads those markdown files and cuts each
// filter's section into the parts a topic page shows: the opening, the rest of
// the explanation, the risks and the configuration blocks. Nothing here writes
// prose about a filter; the facts it adds (CoreFilters and OptimizeForBandwidth
// membership, the risk rating, the enable/disable directives) come from the
// generated reference data in src/data/reference/.
//
// Consumers, all of which must agree on which pages are indexable:
//   - src/pages/docs/filters/[name].astro   renders the topic pages
//   - astro.config.mjs                      keeps thin pages out of the sitemap
//   - scripts/generate-llms.mjs             lists the indexable pages in llms.txt
//   - test/reference/filter-topics.test.ts  the gates
//
// Thin-content guard: a filter whose section carries fewer than THIN_WORDS
// words of its own prose gets a topic page with `noindex,follow`, stays out of
// the sitemap and out of llms.txt. Enriching the section on the group page
// lifts the guard on the next build; nothing else needs to change.

import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/** Fewer words of section prose than this and the topic page is noindex. */
export const THIN_WORDS = 120;

/** Topic page URL path for a filter name. */
export function topicPath(/** @type {string} */ name) {
  return `/docs/filters/${name}/`;
}

/** The group page that documents each category (mirrors src/data/filters.ts). */
export const PAGE_BY_CATEGORY = {
  Image: 'image-filters',
  CSS: 'css-filters',
  JavaScript: 'javascript-filters',
  HTML: 'html-filters',
  Caching: 'cache-control',
};

/**
 * Display names, sentence case, for the H1 and the title. A display label, not
 * prose: where the examples gallery names the same technique, the name follows
 * it ("Minify JavaScript" for rewrite_javascript), so a searcher's words are in
 * the heading. Every filter in filters.json needs one; the gate checks it.
 * @type {Record<string, string>}
 */
export const HUMAN_NAMES = {
  add_base_tag: 'Add base tag',
  add_head: 'Add head',
  add_ids: 'Add IDs',
  add_instrumentation: 'Add instrumentation',
  cache_partial_html: 'Cache partial HTML',
  canonicalize_javascript_libraries: 'Canonicalize JavaScript libraries',
  collapse_whitespace: 'Collapse whitespace',
  combine_css: 'Combine CSS',
  combine_heads: 'Combine heads',
  combine_javascript: 'Combine JavaScript',
  compute_critical_css: 'Compute critical CSS',
  compute_statistics: 'Compute HTML statistics',
  convert_gif_to_png: 'Convert GIF to PNG',
  convert_jpeg_to_avif: 'Convert JPEG to AVIF',
  convert_jpeg_to_progressive: 'Progressive JPEG',
  convert_jpeg_to_webp: 'Convert JPEG to WebP',
  convert_meta_tags: 'Convert meta tags',
  convert_png_to_jpeg: 'Convert PNG to JPEG',
  convert_to_avif_animated: 'Animated AVIF',
  convert_to_avif_lossless: 'Lossless AVIF',
  convert_to_webp_animated: 'Animated WebP',
  convert_to_webp_lossless: 'Lossless WebP',
  debug: 'Debug comments',
  decode_rewritten_urls: 'Decode rewritten URLs',
  dedup_inlined_images: 'Deduplicate inlined images',
  defer_iframe: 'Defer iframes',
  defer_javascript: 'Defer JavaScript',
  deterministic_js: 'Deterministic JavaScript',
  disable_javascript: 'Disable JavaScript',
  div_structure: 'Div structure',
  elide_attributes: 'Elide attributes',
  experiment_collect_mob_image_info: 'Collect mobile image info',
  experiment_http2: 'HTTP/2 experiment',
  explicit_close_tags: 'Explicit close tags',
  extend_cache: 'Extend cache',
  extend_cache_css: 'Extend cache for CSS',
  extend_cache_images: 'Extend cache for images',
  extend_cache_pdfs: 'Extend cache for PDFs',
  extend_cache_scripts: 'Extend cache for scripts',
  fallback_rewrite_css_urls: 'Fallback CSS URL rewriting',
  fix_reflows: 'Fix reflows',
  flatten_css_imports: 'Flatten CSS @imports',
  flush_subresources: 'Flush subresources',
  hint_preload_subresources: 'Preload subresources',
  in_place_optimize_for_browser: 'In-place browser optimization',
  include_js_source_maps: 'Include JS source maps',
  inline_css: 'Inline CSS',
  inline_google_font_css: 'Inline Google Fonts CSS',
  inline_images: 'Inline images',
  inline_import_to_link: 'Convert @import to link',
  inline_javascript: 'Inline JavaScript',
  inline_preview_images: 'Inline preview images',
  insert_amp_link: 'Insert AMP link',
  insert_dns_prefetch: 'Insert DNS prefetch',
  insert_ga: 'Insert Google Analytics',
  insert_image_dimensions: 'Insert image dimensions',
  insert_img_dimensions: 'Insert image dimensions',
  insert_speculation_rules: 'Insert speculation rules',
  jpeg_subsampling: 'JPEG chroma subsampling',
  lazyload_images: 'Lazy-load images',
  left_trim_urls: 'Trim URLs',
  local_storage_cache: 'Local storage cache',
  make_google_analytics_async: 'Async Google Analytics',
  make_show_ads_async: 'Async AdSense',
  mobilize: 'Mobilize',
  mobilize_precompute: 'Mobilize precompute',
  move_css_above_scripts: 'Move CSS above scripts',
  move_css_to_head: 'Move CSS to head',
  outline_css: 'Outline CSS',
  outline_javascript: 'Outline JavaScript',
  pedantic: 'Pedantic',
  prioritize_critical_css: 'Prioritize critical CSS',
  prioritize_critical_images: 'Prioritize critical images',
  recompress_avif: 'Recompress AVIF',
  recompress_images: 'Recompress images',
  recompress_jpeg: 'Recompress JPEG',
  recompress_png: 'Recompress PNG',
  recompress_webp: 'Recompress WebP',
  remove_comments: 'Remove comments',
  remove_quotes: 'Remove quotes',
  resize_images: 'Resize images',
  resize_mobile_images: 'Resize images for mobile',
  resize_rendered_image_dimensions: 'Resize to rendered dimensions',
  responsive_images: 'Responsive images',
  responsive_images_zoom: 'Responsive images zoom',
  rewrite_css: 'Minify CSS',
  rewrite_domains: 'Rewrite domains',
  rewrite_images: 'Optimize images',
  rewrite_javascript: 'Minify JavaScript',
  rewrite_javascript_external: 'Minify external JavaScript',
  rewrite_javascript_inline: 'Minify inline JavaScript',
  rewrite_style_attributes: 'Rewrite style attributes',
  rewrite_style_attributes_with_url: 'Rewrite style attributes with url()',
  split_html: 'Split HTML',
  split_html_helper: 'Split HTML helper',
  sprite_images: 'Sprite images',
  strip_image_color_profile: 'Strip image color profiles',
  strip_image_meta_data: 'Strip image metadata',
  strip_scripts: 'Strip scripts',
  trim_urls: 'Trim URLs',
};

// --- Root and inputs ---------------------------------------------------------

/**
 * The website root: this module's location in the source tree (the generator,
 * the tests, `astro dev`), else the working directory (the Astro build bundles
 * server code under dist/ and runs from the website root).
 * @returns {string}
 */
export function websiteRoot() {
  const fromModule = fileURLToPath(new URL('../../', import.meta.url));
  if (existsSync(resolve(fromModule, 'src/content/docs'))) return fromModule;
  return process.cwd();
}

const FRONTMATTER = /^---\r?\n[\s\S]*?\r?\n---\r?\n?/;

// --- Markdown blocks ---------------------------------------------------------

/**
 * @typedef {{ type: 'heading', depth: number, text: string, id: string | null }
 *   | { type: 'code', lang: string, text: string }
 *   | { type: 'table', text: string }
 *   | { type: 'item', text: string }
 *   | { type: 'para', text: string }
 *   | { type: 'anchors', ids: string[] }
 *   | { type: 'label', text: string }
 *   | { type: 'guide', text: string }
 *   | { type: 'other', text: string }} Block
 */

const LABEL =
  /^(?:\*\*(?:Apache|Nginx|nginx|IIS):\*\*|Enable:|Configuration for (?:nginx|Apache):)$/;
/** The "Full guide" link line the group pages carry under each section. */
export const GUIDE_LINE = /^(?:\[Full guide →\]|Full guides:)/;

/**
 * Split a markdown body into blocks. The group pages use a small subset of
 * markdown (headings, paragraphs, flat lists, tables, fenced code, raw anchor
 * tags and `:::` callouts), so a line-based reader is enough and keeps this
 * module free of a parser dependency the generator would also have to load.
 * @param {string} body
 * @returns {Block[]}
 */
export function parseBlocks(body) {
  const lines = body.split('\n');
  /** @type {Block[]} */
  const blocks = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (!line.trim()) {
      i++;
      continue;
    }
    const fence = /^\s*(```|~~~)\s*([\w-]*)/.exec(line);
    if (fence) {
      const body = [];
      i++;
      while (i < lines.length && !lines[i].trim().startsWith(fence[1])) body.push(lines[i++]);
      i++;
      blocks.push({ type: 'code', lang: fence[2] || 'text', text: body.join('\n') });
      continue;
    }
    const heading = /^(#{1,6})\s+(.*?)\s*$/.exec(line);
    if (heading) {
      const explicit = /\s*\{#([A-Za-z0-9_-]+)\}$/.exec(heading[2]);
      blocks.push({
        type: 'heading',
        depth: heading[1].length,
        text: explicit ? heading[2].slice(0, explicit.index).trim() : heading[2],
        id: explicit ? explicit[1] : null,
      });
      i++;
      continue;
    }
    if (/^:::/.test(line)) {
      const body = [line];
      i++;
      while (i < lines.length && !/^:::\s*$/.test(lines[i])) body.push(lines[i++]);
      body.push(lines[i] ?? '');
      i++;
      blocks.push({ type: 'other', text: body.join('\n') });
      continue;
    }
    if (/^\s*\|/.test(line)) {
      const body = [];
      while (i < lines.length && /^\s*\|/.test(lines[i])) body.push(lines[i++]);
      blocks.push({ type: 'table', text: body.join('\n') });
      continue;
    }
    if (/^(?:<a\s+(?:id|name)="[^"]+"><\/a>\s*)+$/.test(line.trim())) {
      blocks.push({
        type: 'anchors',
        ids: [...line.matchAll(/<a\s+(?:id|name)="([^"]+)"/g)].map((m) => m[1]),
      });
      i++;
      continue;
    }
    const item = /^(?:[-*]|\d+\.)\s+(.*)$/.exec(line);
    if (item) {
      const body = [item[1]];
      i++;
      while (
        i < lines.length &&
        /^\s{2,}\S/.test(lines[i]) &&
        !/^\s*(?:[-*]|\d+\.)\s/.test(lines[i])
      )
        body.push(lines[i++].trim());
      blocks.push({ type: 'item', text: body.join(' ') });
      continue;
    }
    const body = [];
    while (
      i < lines.length &&
      lines[i].trim() &&
      !/^(#{1,6}\s|\s*(```|~~~)|\s*\||(?:[-*]|\d+\.)\s|:::)/.test(lines[i])
    )
      body.push(lines[i++]);
    const text = body.join(' ').replace(/\s+/g, ' ').trim();
    if (LABEL.test(text)) blocks.push({ type: 'label', text });
    else if (GUIDE_LINE.test(text)) blocks.push({ type: 'guide', text });
    else blocks.push({ type: 'para', text });
  }
  return blocks;
}

// --- Text helpers ------------------------------------------------------------

/**
 * Markdown inline syntax to text, whitespace kept: code spans, links,
 * emphasis and HTML entities reduce to their text.
 * @param {string} md
 */
function inlineText(md) {
  return md
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/\*\*([^*]*)\*\*/g, '$1')
    .replace(/(^|[\s(])\*([^*\s][^*]*)\*/g, '$1$2')
    .replace(/(^|[\s(])_([^_\s][^_]*)_(?=[\s).,;:]|$)/g, '$1$2')
    .replace(/&mdash;/g, '—')
    .replace(/&rarr;/g, '→')
    .replace(/&amp;/g, '&');
}

/**
 * Markdown inline syntax to plain text, whitespace collapsed. The FAQPage
 * JSON-LD answers are this; the visible answers are faqHtml() of the same
 * string, whose text content is identical.
 * @param {string} md
 */
export function plainText(md) {
  return inlineText(md).replace(/\s+/g, ' ').trim();
}

/**
 * A FAQ answer as HTML: code spans become <code>, everything else is the same
 * text plainText() produces, escaped. No typographic substitutions, so the
 * page and the JSON-LD say exactly the same thing.
 * @param {string} md
 */
export function faqHtml(md) {
  const esc = (/** @type {string} */ s) =>
    s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  return md
    .replace(/\s+/g, ' ')
    .trim()
    .split(/(`[^`]*`)/)
    .map((part) =>
      /^`[^`]*`$/.test(part) ? `<code>${esc(part.slice(1, -1))}</code>` : esc(inlineText(part)),
    )
    .join('');
}

/** @param {string} text */
export function wordCount(text) {
  const t = plainText(text);
  return t ? t.split(/\s+/).filter((w) => /[A-Za-z0-9]/.test(w)).length : 0;
}

/**
 * Sentences of a markdown paragraph. Splits after `.`, `!` or `?` followed by
 * whitespace and a capital, a code span, a link or an emphasis marker, so
 * "e.g., `document.write`" and version strings stay whole.
 * @param {string} md
 */
export function sentences(md) {
  return md.split(/(?<=[.!?])\s+(?=[A-Z`[*(])/).filter((s) => s.trim());
}

/** Does this markdown text name the filter as a code span? */
function mentions(/** @type {string} */ md, /** @type {string} */ name) {
  return new RegExp('`' + name + '`').test(md);
}

// Status sentences at the start of a section ("Core filter.", "Not a
// CoreFilter. Test before deploying.") repeat what the data says; the opening
// starts with what the filter does instead.
const STATUS =
  /^(?:\*\*)?(?:Core filter|Not a core filter|Not a CoreFilter|Test before deploying|Test thoroughly before enabling|Experimental)\.(?:\*\*)?\s*/i;

// --- Sections ----------------------------------------------------------------

/**
 * @typedef {object} Section
 * @property {string} page        group page slug
 * @property {string} anchor      the id that lands on the section's heading
 * @property {boolean} shared     the filter is one of several on the section
 * @property {string} heading     the section heading's text
 * @property {string} lede        the opening (markdown, at most three sentences)
 * @property {Array<{ heading: string | null, md: string }>} body  the rest of the
 *   explanation, grouped under the section's own subheadings
 * @property {string[]} risks     risk paragraphs and list items (markdown)
 * @property {Array<{ md: string } | { lang: string, code: string }>} config
 *   configuration prose, tables and code blocks, in page order
 * @property {number} words       words of section prose the topic page shows
 * @property {string[]} siblings  the other filters documented on the same section
 */

/**
 * Cut a filter's section out of its group page.
 * @param {Block[]} blocks   the group page's blocks
 * @param {string} page      the group page slug
 * @param {string} name      the filter name
 * @param {Set<string>} allNames every filter name, to tell siblings apart
 * @returns {Section | null}
 */
export function extractSection(blocks, page, name, allNames) {
  let start = blocks.findIndex((b) => b.type === 'heading' && b.id === name);
  let shared = false;
  if (start < 0) {
    const at = blocks.findIndex((b) => b.type === 'anchors' && b.ids.includes(name));
    if (at < 0) return null;
    for (start = at; start >= 0 && blocks[start].type !== 'heading'; start--);
    if (start < 0) return null;
    shared = true;
  }
  const head = /** @type {Extract<Block, { type: 'heading' }>} */ (blocks[start]);
  let end = start + 1;
  while (
    end < blocks.length &&
    !(blocks[end].type === 'heading' && /** @type {any} */ (blocks[end]).depth <= head.depth)
  )
    end++;
  const inner = blocks.slice(start + 1, end);

  /** @type {Set<string>} */
  const siblings = new Set();
  if (head.id && head.id !== name && allNames.has(head.id)) siblings.add(head.id);
  for (const b of inner) {
    if (b.type === 'anchors') for (const id of b.ids) if (id !== name) siblings.add(id);
  }

  // A block on a shared section belongs to this filter's page when it names
  // the filter, or names none of the section's other filters (shared context).
  const keep = (/** @type {string} */ md) =>
    !shared || mentions(md, name) || ![...siblings].some((s) => mentions(md, s));

  /** @type {Section['body']} */
  const body = [];
  /** @type {string[]} */
  const risks = [];
  /** @type {Section['config']} */
  const config = [];
  /** @type {string | null} */
  let sub = null;
  let mode = /** @type {'body' | 'risks' | 'config'} */ ('body');
  for (const b of inner) {
    if (b.type === 'heading') {
      sub = b.text;
      mode = /^risks?$/i.test(b.text)
        ? 'risks'
        : /^(?:directives|configuration)$/i.test(b.text)
          ? 'config'
          : 'body';
      if (/^what (?:it does|they do)$/i.test(b.text)) sub = null;
      continue;
    }
    if (b.type === 'code') {
      if (!shared && mode !== 'risks') config.push({ lang: b.lang, code: b.text });
      continue;
    }
    if (b.type === 'table') {
      if (mode === 'config' || (!shared && mode === 'body'))
        config.push({ md: b.text.replace(/\]\(#/g, `](/docs/${page}/#`) });
      continue;
    }
    if (b.type !== 'para' && b.type !== 'item') continue;
    // In-page fragment links point at the group page from the topic page.
    const text = b.text.replace(/\]\(#/g, `](/docs/${page}/#`);
    const md = b.type === 'item' ? `- ${text}` : text;
    if (!keep(b.text)) continue;
    if (mode === 'risks') risks.push(md);
    else if (mode === 'config') config.push({ md });
    else {
      const last = body[body.length - 1];
      if (last && last.heading === sub)
        last.md +=
          (b.type === 'item' && /^- /m.test(last.md.split('\n').pop() ?? '') ? '\n' : '\n\n') + md;
      else body.push({ heading: sub, md });
    }
  }

  // The opening: the first explanation block that names the filter on a shared
  // section, else the first one; status sentences dropped, three sentences max.
  let ledeIdx = 0;
  if (shared) {
    const i = body.findIndex((g) => mentions(g.md, name));
    if (i >= 0) ledeIdx = i;
  }
  let lede = '';
  if (body.length) {
    const group = body[ledeIdx];
    const parts = group.md.split(/\n\n|\n(?=- )/);
    let pi = shared
      ? Math.max(
          0,
          parts.findIndex((p) => mentions(p, name)),
        )
      : 0;
    let first = parts[pi].replace(/^- /, '');
    while (STATUS.test(first)) first = first.replace(STATUS, '');
    const s = sentences(first);
    lede = s.slice(0, 3).join(' ');
    const rest = s.slice(3).join(' ');
    const remaining = [...parts.slice(0, pi), ...(rest ? [rest] : []), ...parts.slice(pi + 1)];
    if (remaining.length) group.md = remaining.join('\n\n');
    else body.splice(ledeIdx, 1);
  }

  // A sentence that introduces something this page shows elsewhere or not at
  // all ("Enable it by name:" before a code block that moved to the
  // configuration, or "Format conversion filters serve ...:" before a list
  // filtered down to this filter's item) has nothing left to introduce.
  for (const g of body) {
    const parts = g.md.split('\n\n');
    for (let i = parts.length - 1; i >= 0; i--) {
      // A colon introduces what follows; keep it only when a list follows.
      if (!/:$/.test(parts[i]) || /^- /.test(parts[i + 1] ?? '')) continue;
      const s = sentences(parts[i]);
      s.pop();
      if (s.length) parts[i] = s.join(' ');
      else parts.splice(i, 1);
    }
    g.md = parts.join('\n\n');
  }
  for (let i = body.length - 1; i >= 0; i--) if (!body[i].md) body.splice(i, 1);

  // The guard counts this filter's own words: on a shared section only the
  // blocks that name it, so sibling pages do not qualify on the same prose.
  const own = (/** @type {string} */ md) => (!shared || mentions(md, name) ? wordCount(md) : 0);
  const words =
    own(lede) +
    body.reduce(
      (n, g) => n + g.md.split(/\n\n|\n(?=- )/).reduce((m, part) => m + own(part), 0),
      0,
    ) +
    risks.reduce((n, r) => n + own(r), 0);

  return {
    page,
    anchor: head.id ?? name,
    shared,
    heading: head.text,
    lede,
    body,
    risks,
    config,
    words,
    siblings: [...siblings].filter((s) => allNames.has(s)).sort(),
  };
}

// --- Worker equivalents ------------------------------------------------------

/**
 * @typedef {object} WorkerTransform
 * @property {string} id       anchor on /docs/worker-configuration/
 * @property {string} title    the transform's heading
 * @property {string} control  how it is switched (markdown, links made absolute)
 */

/**
 * The worker transforms that name a module filter as their equivalent, read
 * from the "Transforms" section of /docs/worker-configuration/.
 * @param {string} body  worker-configuration.md without frontmatter
 * @returns {Map<string, WorkerTransform>}  filter name -> transform
 */
export function workerEquivalents(body) {
  /** @type {Map<string, WorkerTransform>} */
  const map = new Map();
  const blocks = parseBlocks(body);
  const start = blocks.findIndex(
    (b) => b.type === 'heading' && b.depth === 2 && b.text === 'Transforms',
  );
  if (start < 0) return map;
  for (let i = start + 1; i < blocks.length; i++) {
    const b = blocks[i];
    if (b.type === 'heading' && b.depth <= 2) break;
    if (b.type !== 'heading' || b.depth !== 3 || !b.id) continue;
    const paras = [];
    for (let j = i + 1; j < blocks.length && blocks[j].type !== 'heading'; j++) {
      const p = blocks[j];
      if (p.type === 'para') paras.push(p.text);
    }
    const text = paras.join(' ');
    const eq = /\(module\s+equivalent:\s*([^)]*)\)/.exec(text);
    if (!eq) continue;
    const names = [...eq[1].matchAll(/`([a-z0-9_]+)`/g)].map((m) => m[1]);
    const control = sentences(text.replace(eq[0], '').trim())
      .filter((s) => /Toggleable via|Always-on|no flag of its own/.test(s))
      .join(' ')
      .replace(/\]\(#/g, '](/docs/worker-configuration/#');
    for (const n of names) {
      if (!map.has(n)) map.set(n, { id: b.id, title: b.text, control });
    }
  }
  return map;
}

// --- Related filters --------------------------------------------------------

/**
 * Hand-picked neighbours for the filters people search for most: the
 * techniques a reader weighing this one compares it with. Pages without an
 * entry list the filters on the same section, then the rest of the category.
 * @type {Record<string, string[]>}
 */
export const RELATED = {
  trim_urls: ['left_trim_urls', 'rewrite_domains', 'extend_cache'],
  lazyload_images: [
    'defer_iframe',
    'inline_preview_images',
    'resize_images',
    'prioritize_critical_images',
  ],
  inline_javascript: ['rewrite_javascript', 'combine_javascript', 'defer_javascript', 'inline_css'],
  rewrite_javascript: ['combine_javascript', 'inline_javascript', 'defer_javascript'],
  combine_javascript: ['rewrite_javascript', 'inline_javascript', 'defer_javascript'],
  defer_javascript: ['inline_javascript', 'combine_javascript', 'lazyload_images'],
  combine_css: ['rewrite_css', 'inline_css', 'prioritize_critical_css'],
  rewrite_css: ['combine_css', 'inline_css', 'prioritize_critical_css'],
  inline_css: ['prioritize_critical_css', 'combine_css', 'rewrite_css', 'inline_javascript'],
  prioritize_critical_css: ['inline_css', 'combine_css', 'move_css_to_head'],
  extend_cache: ['extend_cache_css', 'extend_cache_images', 'extend_cache_scripts', 'trim_urls'],
  rewrite_images: [
    'recompress_images',
    'convert_jpeg_to_webp',
    'convert_jpeg_to_avif',
    'resize_images',
    'lazyload_images',
  ],
  recompress_images: ['rewrite_images', 'convert_jpeg_to_webp', 'jpeg_subsampling'],
  convert_jpeg_to_webp: ['convert_jpeg_to_avif', 'rewrite_images', 'recompress_jpeg'],
  convert_jpeg_to_avif: ['convert_jpeg_to_webp', 'convert_to_avif_lossless', 'rewrite_images'],
  resize_images: ['responsive_images', 'insert_image_dimensions', 'rewrite_images'],
  responsive_images: ['resize_images', 'lazyload_images', 'prioritize_critical_images'],
  inline_images: ['dedup_inlined_images', 'inline_preview_images', 'rewrite_images'],
};

// --- Topics ------------------------------------------------------------------

/**
 * @typedef {object} Topic
 * @property {string} name
 * @property {string} human
 * @property {string} category
 * @property {string} summary
 * @property {string} path            /docs/filters/<name>/
 * @property {string} groupPath       /docs/<page>/#<name>
 * @property {boolean | 'partial'} core
 * @property {boolean | 'partial'} optimizeForBandwidth
 * @property {string | null} risk
 * @property {string | null} note
 * @property {boolean} alias
 * @property {string[]} members
 * @property {string[]} memberOf      aliases that enable this filter
 * @property {string | null} alternateSpellingOf
 * @property {boolean} deprecated
 * @property {boolean} dangerous
 * @property {string | null} example  /examples/<slug>/
 * @property {Section} section
 * @property {WorkerTransform | null} worker
 * @property {{ apache: string, nginx: string, iis: string } | null} enable
 * @property {{ apache: string, nginx: string, iis: string } | null} disable
 * @property {boolean} indexable
 * @property {string} title
 * @property {string} description
 * @property {Array<{ q: string, a: string }>} faq  answers in markdown (code spans only)
 */

/**
 * @param {string} syntax  "ModPagespeedEnableFilters filter[,filter...]"
 * @param {string} name
 */
function directiveLine(syntax, name) {
  return syntax.replace('filter[,filter...]', name);
}

/** Title: the longest consistent form that fits 60 characters. */
export function topicTitle(/** @type {string} */ human, /** @type {string} */ name) {
  const forms = [
    `${human}: the ${name} filter for Apache, nginx and IIS`,
    `${human}: the ${name} filter`,
    `${human} (${name})`,
    `The ${name} filter`,
    name,
  ];
  return forms.find((t) => t.length <= 60) ?? name.slice(0, 60);
}

/** Description: the longest consistent form that fits 155 characters. */
export function topicDescription(/** @type {string} */ summary, /** @type {string} */ name) {
  const s = summary.replace(/\.$/, '').replace(/[<>]/g, '');
  const forms = [
    `${s}. How the mod_pagespeed ${name} filter works, when to use it, its risks and the Apache, nginx and IIS directives.`,
    `${s}. The ${name} filter: what it does, its risks and the Apache, nginx and IIS directives.`,
    `${s}. The ${name} filter: what it does, its risks and the directives.`,
    `${s}. The ${name} filter: what it does and its risks.`,
    `${s}.`,
  ];
  const fit = forms.find((d) => d.length <= 155);
  if (fit) return fit;
  return s.slice(0, 154).replace(/\s+\S*$/, '') + '…';
}

/**
 * Two to three questions answered from the section and the data. Answers are
 * markdown with code spans only, so the visible text and the FAQPage JSON-LD
 * (plainText of the same string) are one and the same.
 * @param {Omit<Topic, 'faq' | 'title' | 'description' | 'indexable'>} t
 */
function buildFaq(t) {
  const n = '`' + t.name + '`';
  /** @type {Array<{ q: string, a: string }>} */
  const faq = [];
  const lede = t.section.lede
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/\*\*([^*]*)\*\*/g, '$1');
  if (t.alternateSpellingOf) {
    faq.push({
      q: `What is the ${t.name} filter?`,
      a: `${n} is an alternate spelling of \`${t.alternateSpellingOf}\` that the module accepts. Both names enable the same filter.`,
    });
  } else if (lede) {
    faq.push({ q: `What does the ${t.name} filter do?`, a: lede });
  }

  let byDefault;
  if (t.deprecated) {
    byDefault = `No. The module accepts the name so that existing configurations load, but the filter does nothing.`;
  } else if (t.dangerous) {
    byDefault = `No. ${n} is in the dangerous set, which no RewriteLevel enables, not even \`AllFilters\`. Enable it by name, for testing and measurement only.`;
  } else if (t.core === true) {
    byDefault = `Yes. ${n} is in CoreFilters, the default RewriteLevel, so it runs unless you turn it off with \`DisableFilters\`.`;
  } else if (t.core === 'partial') {
    byDefault = `Partly. Some of the filters ${n} enables are in CoreFilters, the default RewriteLevel; the others run only when you enable them.`;
  } else {
    byDefault = `No. ${n} is not in CoreFilters, the default RewriteLevel; it runs only when you enable it by name with \`EnableFilters\`.`;
  }
  if (!t.deprecated && !t.dangerous) {
    if (t.optimizeForBandwidth === true)
      byDefault += ` It is also part of the OptimizeForBandwidth level.`;
    else if (t.optimizeForBandwidth === 'partial')
      byDefault += ` Some of its filters are also part of the OptimizeForBandwidth level.`;
  }
  faq.push({ q: `Is ${t.name} enabled by default?`, a: byDefault });

  if (t.deprecated) {
    faq.push({
      q: `Should I remove ${t.name} from my configuration?`,
      a: `Yes. The name is kept only so that an existing configuration still loads; removing it changes nothing in the output.`,
    });
  } else if (t.enable && t.disable) {
    if (t.core === true) {
      faq.push({
        q: `How do I turn off ${t.name}?`,
        a: `Add \`${t.disable.apache}\` on Apache or \`${t.disable.nginx}\` on nginx. On IIS, add \`${t.disable.iis}\` to pagespeed.config.`,
      });
    } else {
      faq.push({
        q: `How do I enable ${t.name} on Apache and nginx?`,
        a: `Add \`${t.enable.apache}\` on Apache or \`${t.enable.nginx}\` on nginx. On IIS, add \`${t.enable.iis}\` to pagespeed.config.`,
      });
    }
  }
  return faq;
}

/**
 * Every filter's topic, in filters.json order (filters, then compound names).
 * @param {string} [root]
 * @returns {Topic[]}
 */
export function loadFilterTopics(root = websiteRoot()) {
  const read = (/** @type {string} */ p) => readFileSync(resolve(root, p), 'utf8');
  const data = JSON.parse(read('src/data/reference/filters.json'));
  const directives = JSON.parse(read('src/data/reference/module-directives.json'));
  const list = Array.isArray(directives.directives) ? directives.directives : directives;
  const syntax = (/** @type {string} */ d) => {
    const entry = list.find((/** @type {any} */ x) => x.name === d);
    if (!entry) throw new Error(`module-directives.json: no ${d}`);
    return entry.syntax;
  };
  const enableSyntax = syntax('EnableFilters');
  const disableSyntax = syntax('DisableFilters');

  /** @type {Map<string, Block[]>} */
  const pages = new Map();
  for (const slug of new Set(Object.values(PAGE_BY_CATEGORY))) {
    pages.set(slug, parseBlocks(read(`src/content/docs/${slug}.md`).replace(FRONTMATTER, '')));
  }
  const workers = workerEquivalents(
    read('src/content/docs/worker-configuration.md').replace(FRONTMATTER, ''),
  );

  const entries = [
    ...data.filters.map((/** @type {any} */ f) => ({ ...f, alias: false, members: [] })),
    ...data.aliases.map((/** @type {any} */ a) => ({
      ...a,
      alias: true,
      deprecated: false,
      dangerous: false,
      alternateSpellingOf: null,
    })),
  ];
  const allNames = new Set(entries.map((e) => /** @type {string} */ (e.name)));

  return entries.map((e) => {
    const human = HUMAN_NAMES[e.name];
    if (!human) throw new Error(`filter-topics.mjs: no HUMAN_NAMES entry for ${e.name}`);
    const page = /** @type {Record<string, string>} */ (PAGE_BY_CATEGORY)[e.category];
    const section = extractSection(
      /** @type {Block[]} */ (pages.get(page)),
      page,
      e.name,
      allNames,
    );
    if (!section) throw new Error(`filter-topics.mjs: ${e.name} has no section on /docs/${page}/`);
    const iis = (/** @type {string} */ nginx) => nginx.replace(/;$/, '');
    const enable = e.deprecated
      ? null
      : {
          apache: directiveLine(enableSyntax.apache, e.name),
          nginx: directiveLine(enableSyntax.nginx, e.name),
          iis: iis(directiveLine(enableSyntax.nginx, e.name)),
        };
    const disable = e.deprecated
      ? null
      : {
          apache: directiveLine(disableSyntax.apache, e.name),
          nginx: directiveLine(disableSyntax.nginx, e.name),
          iis: iis(directiveLine(disableSyntax.nginx, e.name)),
        };
    // The table's one-line summary; one that leans on the row above it
    // ("Same, only for ...") reads wrong on its own, so the section's first
    // sentence stands in.
    let summary = e.summary ?? e.label ?? e.name;
    if (/^Same\b/.test(summary)) summary = plainText(sentences(section.lede)[0] ?? summary);
    const base = {
      name: e.name,
      human,
      category: e.category,
      summary,
      path: topicPath(e.name),
      groupPath: `/docs/${page}/#${e.name}`,
      core: e.core,
      optimizeForBandwidth: e.optimizeForBandwidth,
      risk: e.risk ?? null,
      note: e.note ?? null,
      alias: e.alias,
      members: e.members,
      memberOf: data.aliases
        .filter((/** @type {any} */ a) => a.members.includes(e.name))
        .map((/** @type {any} */ a) => a.name),
      alternateSpellingOf: e.alternateSpellingOf ?? null,
      deprecated: !!e.deprecated,
      dangerous: !!e.dangerous,
      example: e.example ?? null,
      section,
      worker: workers.get(e.name) ?? null,
      enable,
      disable,
    };
    return {
      ...base,
      indexable: !base.alternateSpellingOf && section.words >= THIN_WORDS,
      title: topicTitle(human, e.name),
      description: topicDescription(summary, e.name),
      faq: buildFaq(base),
    };
  });
}

/** Names of the topic pages that are indexable (sitemap, llms.txt). */
export function indexableTopicNames(root = websiteRoot()) {
  return new Set(
    loadFilterTopics(root)
      .filter((t) => t.indexable)
      .map((t) => t.name),
  );
}
