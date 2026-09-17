// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Flat reference of every PageSpeed optimization filter, rendered as the
// scannable table on /docs/filters/ (the canonical "PageSpeed filters" hub)
// and mirrored into that page's ItemList JSON-LD.
//
// SINGLE SOURCE: the canonical, human-authored store is the markdown table in
// src/content/docs-1.1/filter-reference.md ("All filters"). This array is a
// machine-consumable PROJECTION of the four fields the hub page needs — name,
// category, plain-text description, and the per-filter deep-link. Core/OFB/Safe
// columns are deliberately omitted so the hub stays a 3-column overview, visibly
// distinct from the 6-column 1.15 directive reference (anti-cannibalization).
//
// ANTI-DRIFT: test/sync/filters-sync.test.ts parses filter-reference.md and
// asserts this array matches it field-for-field and row-for-row. Editing one
// file without the other turns CI red. Keep entries in the SAME order as the
// markdown table (alphabetical by filter name) so the drift check compares
// index-by-index.

export type FilterCategory = 'Image' | 'CSS' | 'JavaScript' | 'HTML' | 'Caching';

export interface Filter {
  /** Filter name, exactly as passed to EnableFilters/DisableFilters. */
  name: string;
  /** Content type the filter transforms. */
  category: FilterCategory;
  /** One-line "what it does", plain text (markdown code-ticks stripped). */
  description: string;
  /** Deep-link to the per-category directive reference, verbatim from filter-reference.md. */
  href: string;
}

/** Display order for grouping the flat table and JSON-LD. */
export const CATEGORY_ORDER: FilterCategory[] = ['Image', 'CSS', 'JavaScript', 'HTML', 'Caching'];

/**
 * Deep-link used for the two `rewrite_javascript` sub-filters, which have no
 * standalone anchor in the source table — they share the parent filter's page.
 */
export const REWRITE_JS_FALLBACK_HREF = '/1.1/docs/javascript-filters/#rewrite_javascript';

// Ordered alphabetically to match the filter-reference.md "All filters" table.
export const FILTERS: Filter[] = [
  {
    name: 'add_head',
    category: 'HTML',
    description: 'Adds <head> element if missing',
    href: '/1.1/docs/html-filters/#add_head',
  },
  {
    name: 'add_instrumentation',
    category: 'HTML',
    description: 'Injects JavaScript to measure page load time',
    href: '/1.1/docs/html-filters/#add_instrumentation',
  },
  {
    name: 'collapse_whitespace',
    category: 'HTML',
    description: 'Removes excess whitespace from HTML',
    href: '/1.1/docs/html-filters/#collapse_whitespace',
  },
  {
    name: 'combine_css',
    category: 'CSS',
    description: 'Combines multiple CSS files into one',
    href: '/1.1/docs/css-filters/#combine_css',
  },
  {
    name: 'combine_heads',
    category: 'HTML',
    description: 'Merges multiple <head> elements',
    href: '/1.1/docs/html-filters/#combine_heads',
  },
  {
    name: 'combine_javascript',
    category: 'JavaScript',
    description: 'Combines multiple JS files into one',
    href: '/1.1/docs/javascript-filters/#combine_javascript',
  },
  {
    name: 'convert_gif_to_png',
    category: 'Image',
    description: 'Converts GIF to PNG',
    href: '/1.1/docs/image-filters/#convert_formats',
  },
  {
    name: 'convert_jpeg_to_avif',
    category: 'Image',
    description: 'Converts photographic JPEG to AVIF for capable browsers',
    href: '/1.1/docs/image-filters/#avif',
  },
  {
    name: 'convert_jpeg_to_progressive',
    category: 'Image',
    description: 'Converts large JPEGs to progressive format',
    href: '/1.1/docs/image-filters/#recompress_images',
  },
  {
    name: 'convert_jpeg_to_webp',
    category: 'Image',
    description: 'Converts JPEG to WebP for capable browsers',
    href: '/1.1/docs/image-filters/#convert_formats',
  },
  {
    name: 'convert_meta_tags',
    category: 'HTML',
    description: 'Adds HTTP headers from <meta http-equiv> tags',
    href: '/1.1/docs/html-filters/#convert_meta_tags',
  },
  {
    name: 'convert_png_to_jpeg',
    category: 'Image',
    description: 'Converts PNG to JPEG when no transparency',
    href: '/1.1/docs/image-filters/#convert_formats',
  },
  {
    name: 'convert_to_avif_animated',
    category: 'Image',
    description: 'Converts animated images to AVIF',
    href: '/1.1/docs/image-filters/#avif',
  },
  {
    name: 'convert_to_avif_lossless',
    category: 'Image',
    description: 'Converts PNG/GIF to lossless AVIF',
    href: '/1.1/docs/image-filters/#avif',
  },
  {
    name: 'convert_to_webp_animated',
    category: 'Image',
    description: 'Converts animated GIF to WebP',
    href: '/1.1/docs/image-filters/#convert_formats',
  },
  {
    name: 'convert_to_webp_lossless',
    category: 'Image',
    description: 'Converts PNG/GIF to lossless WebP',
    href: '/1.1/docs/image-filters/#convert_formats',
  },
  {
    name: 'dedup_inlined_images',
    category: 'Image',
    description: 'Replaces repeated inlined images with JS reference',
    href: '/1.1/docs/image-filters/#inline_images',
  },
  {
    name: 'defer_javascript',
    category: 'JavaScript',
    description: 'Defers JS execution until after page load',
    href: '/1.1/docs/javascript-filters/#defer_javascript',
  },
  {
    name: 'elide_attributes',
    category: 'HTML',
    description: 'Removes default-value HTML attributes',
    href: '/1.1/docs/html-filters/#elide_attributes',
  },
  {
    name: 'extend_cache',
    category: 'Caching',
    description: 'Content-hashed URLs with 1-year browser cache',
    href: '/1.1/docs/caching-url-filters/#extend_cache',
  },
  {
    name: 'extend_cache_pdfs',
    category: 'Caching',
    description: 'Cache extension for PDF links',
    href: '/1.1/docs/caching-url-filters/#extend_cache_pdfs',
  },
  {
    name: 'fallback_rewrite_css_urls',
    category: 'CSS',
    description: 'Rewrites resource URLs in unparseable CSS',
    href: '/1.1/docs/css-filters/#fallback_rewrite_css_urls',
  },
  {
    name: 'flatten_css_imports',
    category: 'CSS',
    description: 'Inlines CSS @import rules',
    href: '/1.1/docs/css-filters/#flatten_css_imports',
  },
  {
    name: 'hint_preload_subresources',
    category: 'HTML',
    description: 'Adds Link: rel=preload headers',
    href: '/1.1/docs/html-filters/#hint_preload_subresources',
  },
  {
    name: 'in_place_optimize_for_browser',
    category: 'Image',
    description: 'Browser-specific in-place optimization',
    href: '/1.1/docs/image-filters/#in_place_optimize_for_browser',
  },
  {
    name: 'include_js_source_maps',
    category: 'JavaScript',
    description: 'Preserves JavaScript source maps',
    href: '/1.1/docs/javascript-filters/#include_js_source_maps',
  },
  {
    name: 'inline_css',
    category: 'CSS',
    description: 'Inlines small external CSS into HTML',
    href: '/1.1/docs/css-filters/#inline_css',
  },
  {
    name: 'inline_google_font_css',
    category: 'CSS',
    description: 'Inlines Google Fonts CSS',
    href: '/1.1/docs/css-filters/#inline_google_font_css',
  },
  {
    name: 'inline_images',
    category: 'Image',
    description: 'Inlines small images as data: URIs',
    href: '/1.1/docs/image-filters/#inline_images',
  },
  {
    name: 'inline_import_to_link',
    category: 'CSS',
    description: 'Converts <style>@import</style> to <link>',
    href: '/1.1/docs/css-filters/#inline_import_to_link',
  },
  {
    name: 'inline_javascript',
    category: 'JavaScript',
    description: 'Inlines small external JS into HTML',
    href: '/1.1/docs/javascript-filters/#inline_javascript',
  },
  {
    name: 'inline_preview_images',
    category: 'Image',
    description: 'Inserts low-quality image placeholders',
    href: '/1.1/docs/image-filters/#inline_images',
  },
  {
    name: 'insert_dns_prefetch',
    category: 'HTML',
    description: 'Adds <link rel=dns-prefetch> for third-party domains',
    href: '/1.1/docs/html-filters/#insert_dns_prefetch',
  },
  {
    name: 'insert_image_dimensions',
    category: 'Image',
    description: 'Adds width and height attributes to <img> tags',
    href: '/1.1/docs/image-filters/#resize_images',
  },
  {
    name: 'insert_speculation_rules',
    category: 'HTML',
    description: 'Injects a same-origin prefetch speculation-rules script',
    href: '/1.1/docs/html-filters/#insert_speculation_rules',
  },
  {
    name: 'jpeg_subsampling',
    category: 'Image',
    description: 'Reduces chroma sampling to 4:2:0',
    href: '/1.1/docs/image-filters/#recompress_images',
  },
  {
    name: 'lazyload_images',
    category: 'Image',
    description: 'Defers offscreen image loading',
    href: '/1.1/docs/image-filters/#lazyload_images',
  },
  {
    name: 'local_storage_cache',
    category: 'Caching',
    description: 'Caches inlined resources in localStorage',
    href: '/1.1/docs/caching-url-filters/#local_storage_cache',
  },
  {
    name: 'move_css_above_scripts',
    category: 'CSS',
    description: 'Moves CSS <link> above <script> tags',
    href: '/1.1/docs/css-filters/#move_css_above_scripts',
  },
  {
    name: 'move_css_to_head',
    category: 'CSS',
    description: 'Moves CSS <link> into <head>',
    href: '/1.1/docs/css-filters/#move_css_to_head',
  },
  {
    name: 'outline_css',
    category: 'CSS',
    description: 'Externalizes large inline CSS blocks',
    href: '/1.1/docs/css-filters/#outline_css',
  },
  {
    name: 'outline_javascript',
    category: 'JavaScript',
    description: 'Externalizes large inline JS blocks',
    href: '/1.1/docs/javascript-filters/#outline_javascript',
  },
  {
    name: 'pedantic',
    category: 'HTML',
    description: 'Adds type attributes for HTML4 validation',
    href: '/1.1/docs/html-filters/#pedantic',
  },
  {
    name: 'prioritize_critical_css',
    category: 'CSS',
    description: 'Inlines above-fold CSS, defers the rest',
    href: '/1.1/docs/css-filters/#prioritize_critical_css',
  },
  {
    name: 'prioritize_critical_images',
    category: 'Image',
    description: 'Sets fetchpriority=high on the LCP image',
    href: '/1.1/docs/image-filters/#prioritize_critical_images',
  },
  {
    name: 'recompress_avif',
    category: 'Image',
    description: 'AVIF-specific recompression',
    href: '/1.1/docs/image-filters/#avif',
  },
  {
    name: 'recompress_images',
    category: 'Image',
    description: 'Recompresses and converts images (lossy re-encode)',
    href: '/1.1/docs/image-filters/#recompress_images',
  },
  {
    name: 'recompress_jpeg',
    category: 'Image',
    description: 'JPEG-specific recompression',
    href: '/1.1/docs/image-filters/#recompress_images',
  },
  {
    name: 'recompress_png',
    category: 'Image',
    description: 'PNG-specific recompression',
    href: '/1.1/docs/image-filters/#recompress_images',
  },
  {
    name: 'recompress_webp',
    category: 'Image',
    description: 'WebP-specific recompression',
    href: '/1.1/docs/image-filters/#recompress_images',
  },
  {
    name: 'remove_comments',
    category: 'HTML',
    description: 'Strips HTML comments',
    href: '/1.1/docs/html-filters/#remove_comments',
  },
  {
    name: 'remove_quotes',
    category: 'HTML',
    description: 'Removes unnecessary attribute quotes',
    href: '/1.1/docs/html-filters/#remove_quotes',
  },
  {
    name: 'resize_images',
    category: 'Image',
    description: 'Resizes images to match <img> dimensions',
    href: '/1.1/docs/image-filters/#resize_images',
  },
  {
    name: 'resize_mobile_images',
    category: 'Image',
    description: 'Smaller placeholders for mobile',
    href: '/1.1/docs/image-filters/#resize_images',
  },
  {
    name: 'resize_rendered_image_dimensions',
    category: 'Image',
    description: 'Resizes to rendered dimensions',
    href: '/1.1/docs/image-filters/#resize_images',
  },
  {
    name: 'responsive_images',
    category: 'Image',
    description: 'Generates srcset for multiple resolutions',
    href: '/1.1/docs/image-filters/#responsive_images',
  },
  {
    name: 'rewrite_css',
    category: 'CSS',
    description: 'Minifies CSS, rewrites embedded URLs',
    href: '/1.1/docs/css-filters/#rewrite_css',
  },
  {
    name: 'rewrite_domains',
    category: 'Caching',
    description: 'Applies domain mappings to original resources',
    href: '/1.1/docs/caching-url-filters/#rewrite_domains',
  },
  {
    name: 'rewrite_images',
    category: 'Image',
    description: 'Master image optimization (enables sub-filters)',
    href: '/1.1/docs/image-filters/#rewrite_images',
  },
  {
    name: 'rewrite_javascript',
    category: 'JavaScript',
    description: 'Minifies JavaScript',
    href: '/1.1/docs/javascript-filters/#rewrite_javascript',
  },
  {
    name: 'rewrite_javascript_external',
    category: 'JavaScript',
    description: 'Minifies external JavaScript files',
    href: REWRITE_JS_FALLBACK_HREF,
  },
  {
    name: 'rewrite_javascript_inline',
    category: 'JavaScript',
    description: 'Minifies inline JavaScript',
    href: REWRITE_JS_FALLBACK_HREF,
  },
  {
    name: 'rewrite_style_attributes',
    category: 'CSS',
    description: 'Applies CSS rewriting to inline style attributes',
    href: '/1.1/docs/css-filters/#rewrite_style_attributes',
  },
  {
    name: 'rewrite_style_attributes_with_url',
    category: 'CSS',
    description: 'Same, only for styles containing url()',
    href: '/1.1/docs/css-filters/#rewrite_style_attributes',
  },
  {
    name: 'sprite_images',
    category: 'Image',
    description: 'Combines CSS background images into sprites',
    href: '/1.1/docs/image-filters/#sprite_images',
  },
  {
    name: 'strip_image_color_profile',
    category: 'Image',
    description: 'Removes ICC color profiles',
    href: '/1.1/docs/image-filters/#strip_metadata',
  },
  {
    name: 'strip_image_meta_data',
    category: 'Image',
    description: 'Removes EXIF and other metadata',
    href: '/1.1/docs/image-filters/#strip_metadata',
  },
  {
    name: 'trim_urls',
    category: 'HTML',
    description: 'Shortens URLs relative to base URL',
    href: '/1.1/docs/html-filters/#trim_urls',
  },
];

/** Filters grouped by category in display order, for the table and JSON-LD. */
export function filtersByCategory(): { category: FilterCategory; filters: Filter[] }[] {
  return CATEGORY_ORDER.map((category) => ({
    category,
    filters: FILTERS.filter((f) => f.category === category),
  }));
}
