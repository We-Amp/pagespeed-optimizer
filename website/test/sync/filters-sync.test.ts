// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// filters-sync.test.ts — drift guard between the canonical filter table and the
// machine-consumable projection used by the /docs/filters/ hub.
//
//   source of truth : src/content/docs-1.1/filter-reference.md  ("All filters")
//   projection       : src/data/filters.ts  (FILTERS)
//
// The hub page (/docs/filters/) renders its flat reference table and ItemList
// JSON-LD from FILTERS. FILTERS is a hand-maintained projection of four columns
// (name, category, description, deep-link) out of the six-column markdown table.
// If a filter is added/removed/renamed or its description/link changes in the
// markdown without the same change in FILTERS (or vice-versa), CI turns red here
// — the markdown stays the single source, FILTERS can never silently drift from
// it. This guard parses the markdown, not a second copy, so it cannot itself rot.
//
// LINK TARGET: the markdown source still links into the legacy /1.1/docs/
// per-category pages, but four of those categories (Image, CSS, JavaScript,
// HTML) now also live at /docs/ and FILTERS deliberately points there
// instead — Caching does not, and stays on /1.1/docs/. CONVERGED_CATEGORIES
// below is the one place that split is declared; `expectedHref` re-derives
// the FILTERS-side link from the markdown-side link so the row-for-row
// comparison still catches any OTHER drift (name/category/description, or an
// unexpected href on top of the known category remap).

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, it, expect } from 'vitest';
import {
  FILTERS,
  CATEGORY_ORDER,
  REWRITE_JS_FALLBACK_HREF,
  type Filter,
  type FilterCategory,
} from '../../src/data/filters';

const ALLOWED_CATEGORIES = new Set<string>(CATEGORY_ORDER);

// Categories whose per-category page was copied into the main /docs/ tree.
// Their FILTERS hrefs point at /docs/<slug>/, not /1.1/docs/<slug>/; Caching
// has not moved and keeps its /1.1/docs/ links.
const CONVERGED_CATEGORIES = new Set<FilterCategory>(['Image', 'CSS', 'JavaScript', 'HTML']);

/**
 * The href FILTERS is expected to carry for a markdown-sourced row: the
 * markdown's own href, with the legacy /1.1/docs/ prefix swapped for /docs/
 * when the row's category is one of CONVERGED_CATEGORIES.
 */
function expectedHref(category: FilterCategory, mdHref: string): string {
  if (!CONVERGED_CATEGORIES.has(category)) return mdHref;
  return mdHref.replace(/^\/1\.1\/docs\//, '/docs/');
}

// Expected per-category counts (verified against filter-reference.md). The total
// (68) and these counts are asserted so an accidental row add/drop is caught even
// if the projection is edited to match by mistake.
const EXPECTED_COUNTS: Record<FilterCategory, number> = {
  Image: 30,
  CSS: 13,
  JavaScript: 8,
  HTML: 13,
  Caching: 4,
};

function read(rel: string): string {
  return readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8');
}

/**
 * Parse the "All filters" markdown table into the same four-field shape as
 * FILTERS. Only rows beginning with `| <a id="..."` are table rows; the platform
 * syntax table and the deprecated-filters prose list are excluded by that anchor.
 */
function parseReferenceTable(md: string): Filter[] {
  const rows: Filter[] = [];
  for (const line of md.split('\n')) {
    if (!line.startsWith('| <a id="')) continue;
    const cells = line.split('|').map((c) => c.trim());
    // cells[0] = '' ; cells[1] = name cell ; cells[2] = category ; cells[5] = description
    const nameCell = cells[1] ?? '';
    const nameMatch = nameCell.match(/id="([^"]+)"/);
    if (!nameMatch) {
      throw new Error(`filters-sync: could not parse filter name from row: ${line}`);
    }
    const name = nameMatch[1];
    const category = cells[2] as FilterCategory;
    // strip markdown code-ticks so it matches the plain-text description in FILTERS
    const description = (cells[5] ?? '').replace(/`/g, '');
    const hrefMatch = nameCell.match(/\]\(([^)]+)\)/);
    // Only the rewrite_javascript_* sub-filters legitimately lack a markdown
    // link in the source table; any OTHER unlinked row is a markdown defect that
    // should fail loudly rather than silently inherit the JS fallback href.
    let href: string;
    if (hrefMatch) {
      href = hrefMatch[1];
    } else if (name.startsWith('rewrite_javascript_')) {
      href = REWRITE_JS_FALLBACK_HREF;
    } else {
      throw new Error(
        `filters-sync: row "${name}" has no markdown link and is not a known ` +
          `rewrite_javascript_* sub-filter — fix the link in filter-reference.md.`,
      );
    }
    rows.push({ name, category, description, href });
  }
  return rows;
}

/**
 * Count EVERY data row in the "All filters" table section — anchored or not —
 * independent of parseReferenceTable's `| <a id=` filter. This closes the blind
 * spot where a filter row added WITHOUT an <a id> anchor (precedent:
 * rewrite_javascript_external/inline) would be silently skipped by the parser
 * yet leave the hardcoded counts unchanged, so the page would miss a filter.
 */
function countAllFilterTableRows(md: string): number {
  const lines = md.split('\n');
  const start = lines.findIndex((l) => l.trim() === '## All filters');
  if (start === -1) {
    throw new Error('filters-sync: could not find the "## All filters" section heading');
  }
  let count = 0;
  for (let i = start + 1; i < lines.length; i++) {
    const line = lines[i];
    if (line.startsWith('## ')) break; // next section
    if (!line.startsWith('|')) continue; // not a table row
    if (line.startsWith('| Filter')) continue; // header row
    if (/^\|\s*-/.test(line)) continue; // separator row
    count++;
  }
  return count;
}

describe('filters.ts is in lockstep with filter-reference.md', () => {
  const parsed = parseReferenceTable(read('../../src/content/docs-1.1/filter-reference.md'));

  it('parses a non-trivial number of rows from the markdown source', () => {
    expect(parsed.length).toBeGreaterThanOrEqual(60);
  });

  it('has exactly 68 filters in both the markdown table and the projection', () => {
    expect(parsed.length).toBe(68);
    expect(FILTERS.length).toBe(68);
  });

  it('projects EVERY data row in the markdown table (catches un-anchored row churn)', () => {
    // Total rows in the section, counted independently of the parser's anchor
    // filter, must equal what we projected — so adding/removing a row (anchored
    // or not) without updating FILTERS turns CI red.
    const totalRows = countAllFilterTableRows(
      read('../../src/content/docs-1.1/filter-reference.md'),
    );
    expect(totalRows).toBe(FILTERS.length);
    expect(totalRows).toBe(68);
  });

  it('uses only known categories', () => {
    for (const f of FILTERS) {
      expect(ALLOWED_CATEGORIES.has(f.category)).toBe(true);
    }
  });

  it('matches the expected per-category breakdown', () => {
    for (const category of CATEGORY_ORDER) {
      const projCount = FILTERS.filter((f) => f.category === category).length;
      const mdCount = parsed.filter((f) => f.category === category).length;
      expect(projCount, `projection count for ${category}`).toBe(EXPECTED_COUNTS[category]);
      expect(mdCount, `markdown count for ${category}`).toBe(EXPECTED_COUNTS[category]);
    }
  });

  it('every filter deep-links into the converged /docs/ or legacy /1.1/docs/ per-category docs', () => {
    for (const f of FILTERS) {
      const prefix = CONVERGED_CATEGORIES.has(f.category) ? '/docs/' : '/1.1/docs/';
      expect(f.href.startsWith(prefix), `${f.name} href should start with ${prefix}`).toBe(true);
    }
  });

  it('matches the markdown source field-for-field, row-for-row (converged categories re-pointed)', () => {
    // Both arrays are in the same (alphabetical) source order, so compare index-by-index.
    // href is re-derived via expectedHref() to reflect the deliberate /docs/ re-point for
    // CONVERGED_CATEGORIES; every other field (name, category, description) must still
    // match the markdown byte-for-byte.
    const expected = parsed.map((f) => ({ ...f, href: expectedHref(f.category, f.href) }));
    expect(FILTERS).toEqual(expected);
  });
});
