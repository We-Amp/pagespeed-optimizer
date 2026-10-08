// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Unit tests for the rule functions of scripts/check-built-content.mjs (the
// built-output content lint wired into the Website workflow as
// `npm run check:content`). The fixtures here are synthetic HTML snippets;
// the real build is checked by running the script over dist/, not here.

import { describe, it, expect } from 'vitest';
import {
  decodeEntities,
  collapseWhitespace,
  titleInfo,
  descriptionInfo,
  isNoindex,
  visibleText,
  h1Count,
  canonicalHrefs,
  canonicalProblem,
  lintPage,
  lintPages,
  urlOf,
} from '../scripts/check-built-content.mjs';

describe('decodeEntities', () => {
  it('decodes the named entities titles and descriptions carry', () => {
    expect(decodeEntities('a &amp; b &#39;c&#39; &mdash; d')).toBe("a & b 'c' — d");
  });

  it('decodes decimal and hex numeric references', () => {
    expect(decodeEntities('&#8212; &#x27;')).toBe('— \'');
  });

  it('leaves unknown entities alone', () => {
    expect(decodeEntities('&nosuch; 5 &amp; 6')).toBe('&nosuch; 5 & 6');
  });
});

describe('collapseWhitespace', () => {
  it('collapses runs and trims', () => {
    expect(collapseWhitespace('  a\n\t b   c ')).toBe('a b c');
  });
});

describe('titleInfo', () => {
  it('reads and decodes the document title from head', () => {
    const html = '<html><head><title>Fine &amp; dandy</title></head><body><svg><title>icon</title></svg></body></html>';
    const t = titleInfo(html);
    expect(t.count).toBe(1);
    expect(t.text).toBe('Fine & dandy');
  });

  it('reports a missing title', () => {
    expect(titleInfo('<html><head></head><body></body></html>').count).toBe(0);
  });

  it('reports duplicated titles', () => {
    expect(titleInfo('<head><title>a</title><title>b</title></head>').count).toBe(2);
  });
});

describe('descriptionInfo', () => {
  it('keeps a literal apostrophe inside a double-quoted value', () => {
    const html = '<meta name="description" content="Why the worker won&#39;t and can&#39;t stop">';
    // Astro leaves ' unescaped in double-quoted attributes; the value must
    // not be cut at it (this bug read "PSOL was Google's…" as 15 chars).
    const html2 = `<meta name="description" content="PSOL was Google's shared library">`;
    expect(descriptionInfo(html).text).toBe("Why the worker won't and can't stop");
    expect(descriptionInfo(html2).text).toBe("PSOL was Google's shared library");
  });

  it('accepts reversed attribute order', () => {
    const html = '<meta content="A description long enough to matter here" name="description">';
    expect(descriptionInfo(html).text).toBe('A description long enough to matter here');
  });

  it('reports absence', () => {
    expect(descriptionInfo('<head></head>').count).toBe(0);
    expect(descriptionInfo('<head></head>').text).toBe('');
  });
});

describe('isNoindex', () => {
  it('detects noindex in any robots value order', () => {
    expect(isNoindex('<meta name="robots" content="noindex, follow">')).toBe(true);
    expect(isNoindex('<meta name="robots" content="noindex,nofollow">')).toBe(true);
    expect(isNoindex('<meta content="index, follow" name="robots">')).toBe(false);
    expect(isNoindex('<meta name="robots" content="index, follow">')).toBe(false);
    expect(isNoindex('<head></head>')).toBe(false);
  });
});

describe('visible text extraction', () => {
  const page = (body: string) => `<html><head><title>t</title></head><body>${body}</body></html>`;

  it('strips script, style, code, pre and comments', () => {
    const html = page(
      '<script>var daemon = 1;</script><style>.x { content: "daemon" }</style>' +
        '<p>The worker runs.</p><code>daemon flag</code><pre>daemon config</pre><!-- daemon -->',
    );
    expect(visibleText(html)).toBe('The worker runs.');
  });

  it('decodes entities in text', () => {
    expect(visibleText(page('<p>It&#39;s the worker</p>'))).toBe("It's the worker");
  });
});

describe('h1Count', () => {
  it('counts only h1 opening tags outside script and comments', () => {
    const html = '<h1>One</h1><script>const s = "<h1>no</h1>";</script><!-- <h1>no</h1> --><h2>Two</h2>';
    expect(h1Count(html)).toBe(1);
  });

  it('does not match h1-prefixed tags like a hypothetical h10', () => {
    expect(h1Count('<h10>not a heading</h10>')).toBe(0);
  });

  it('counts zero and two', () => {
    expect(h1Count('<p>none</p>')).toBe(0);
    expect(h1Count('<h1>a</h1><h1>b</h1>')).toBe(2);
  });
});

describe('canonicalHrefs / canonicalProblem', () => {
  it('finds canonical links regardless of attribute order', () => {
    expect(canonicalHrefs('<link rel="canonical" href="https://modpagespeed.com/a/">')).toEqual([
      'https://modpagespeed.com/a/',
    ]);
    expect(canonicalHrefs('<link href="https://modpagespeed.com/a/" rel="canonical">')).toEqual([
      'https://modpagespeed.com/a/',
    ]);
    expect(canonicalHrefs('<link rel="stylesheet" href="/x.css">')).toEqual([]);
  });

  it('accepts absolute https URLs ending in / or an extension', () => {
    expect(canonicalProblem('https://modpagespeed.com/docs/')).toBeNull();
    expect(canonicalProblem('https://modpagespeed.com/1.0/doc/system.html')).toBeNull();
  });

  it('rejects relative, off-site and bare paths', () => {
    expect(canonicalProblem('/docs/')).toMatch(/not an absolute URL/);
    expect(canonicalProblem('http://modpagespeed.com/docs/')).toMatch(/not on/);
    expect(canonicalProblem('https://example.com/docs/')).toMatch(/not on/);
    expect(canonicalProblem('https://modpagespeed.com/docs')).toMatch(/does not end/);
  });
});

describe('lintPage', () => {
  const goodHead =
    '<title>A title of usable length</title>' +
    '<meta name="description" content="A description that is comfortably longer than fifty characters for the lint.">' +
    '<link rel="canonical" href="https://modpagespeed.com/x/">';

  it('passes a well-formed indexable page', () => {
    const html = `<html><head>${goodHead}</head><body><h1>Heading</h1><p>The worker serves.</p></body></html>`;
    expect(lintPage(html)).toEqual([]);
  });

  it('flags a short title and a short description separately', () => {
    const html =
      '<head><title>Short</title>' +
      '<meta name="description" content="too short">' +
      '<link rel="canonical" href="https://modpagespeed.com/x/"></head>' +
      '<body><h1>H</h1></body>';
    const rules = lintPage(html).map((f) => f.rule);
    expect(rules).toContain('title-length');
    expect(rules).toContain('description-length');
  });

  it('skips length, description, daemon and canonical rules on noindex pages', () => {
    const html =
      '<head><title>Short</title>' +
      '<meta name="robots" content="noindex, follow">' +
      '<meta name="description" content="too short"></head>' +
      '<body><h1>H</h1><p>The daemon serves.</p></body>';
    const rules = lintPage(html).map((f) => f.rule);
    expect(rules).not.toContain('title-length');
    expect(rules).not.toContain('description-present');
    expect(rules).not.toContain('term-drift-daemon');
    expect(rules).not.toContain('canonical');
  });

  it('still demands one h1 and the product name on noindex pages', () => {
    const html =
      '<head><title>t</title><meta name="robots" content="noindex"></head>' +
      '<body><p>mod_pagespeed 2.0 is old</p></body>';
    const rules = lintPage(html).map((f) => f.rule);
    expect(rules).toContain('h1-count');
    expect(rules).toContain('product-naming');
  });

  it('flags daemon in visible text but not in code samples', () => {
    const visible = `<html><head>${goodHead}</head><body><h1>H</h1><p>Restart the daemon.</p></body></html>`;
    const coded = `<html><head>${goodHead}</head><body><h1>H</h1><code>systemctl restart daemon</code></body></html>`;
    expect(lintPage(visible).map((f) => f.rule)).toContain('term-drift-daemon');
    expect(lintPage(coded).map((f) => f.rule)).not.toContain('term-drift-daemon');
  });

  it('does not match daemons (plural) for the word-boundary rule', () => {
    const html = `<html><head>${goodHead}</head><body><h1>H</h1><p>Two daemons were considered.</p></body></html>`;
    expect(lintPage(html).map((f) => f.rule)).not.toContain('term-drift-daemon');
  });

  it('flags both product-naming spellings, not the current product', () => {
    const old = (t: string) =>
      `<html><head>${goodHead}</head><body><h1>H</h1><p>${t}</p></body></html>`;
    expect(lintPage(old('Runs ModPageSpeed 2.0 today')).map((f) => f.rule)).toContain('product-naming');
    expect(lintPage(old('Runs mod_pagespeed 2.0 today')).map((f) => f.rule)).toContain('product-naming');
    expect(lintPage(old('Runs mod_pagespeed 2.1 today')).map((f) => f.rule)).not.toContain('product-naming');
  });

  it('flags missing, multiple and malformed canonicals', () => {
    const none = `<head>${goodHead.replace(/<link[^>]*>/, '')}</head>`;
    const two = `<head>${goodHead}<link rel="canonical" href="https://modpagespeed.com/y/"></head>`;
    const relative = `<head>${goodHead.replace('https://modpagespeed.com/x/', '/x/')}</head>`;
    for (const head of [none, two, relative]) {
      const rules = lintPage(`${head}<body><h1>H</h1></body>`).map((f) => f.rule);
      expect(rules).toContain('canonical');
    }
  });
});

describe('lintPages', () => {
  const page = (url: string, description: string) => ({
    url,
    html:
      '<head><title>A title of usable length</title>' +
      `<meta name="description" content="${description}">` +
      '<link rel="canonical" href="https://modpagespeed.com/x/"></head>' +
      '<body><h1>H</h1></body>',
  });

  it('groups identical descriptions on indexable pages', () => {
    const { failures } = lintPages([
      page('/a/', 'The very same description repeated on two pages exactly'),
      page('/b/', 'The very same description repeated on two pages exactly'),
      page('/c/', 'A different description that is long enough to be valid'),
    ]);
    const dup = failures.find((f) => f.rule === 'description-duplicate');
    expect(dup?.message).toMatch(/\/a\//);
    expect(dup?.message).toMatch(/\/b\//);
  });

  it('exempts allowlisted pages and warns about stale entries', () => {
    const { failures, warnings } = lintPages(
      [page('/a/', 'short'), { ...page('/gone/', 'x') }],
      { '/a/': 'historical page, exemption documented', '/nope/': 'stale entry' },
    );
    expect(failures.find((f) => f.url === '/a/')).toBeUndefined();
    expect(warnings.some((w) => w.url === '/a/' && w.rule === 'allowlist')).toBe(true);
    expect(warnings.some((w) => w.url === '/nope/' && /matches no built page/.test(w.message))).toBe(true);
  });

  it('routes warn-level rules to warnings and error rules to failures', () => {
    const naming = {
      url: '/x/',
      html:
        '<head><title>A title of usable length</title>' +
        '<meta name="robots" content="noindex"></head>' +
        '<body><h1>H</h1><p>mod_pagespeed 2.0 mentioned</p></body>',
    };
    const { failures, warnings } = lintPages([naming]);
    expect(warnings.some((w) => w.rule === 'product-naming')).toBe(true);
    expect(failures).toEqual([]);
  });
});

describe('urlOf', () => {
  it('maps dist files to site URLs', () => {
    expect(urlOf('/dist', '/dist/client/index.html')).toBe('/');
    expect(urlOf('/dist', '/dist/client/docs/a/index.html')).toBe('/docs/a/');
    expect(urlOf('/dist', '/dist/client/404.html')).toBe('/404.html');
    expect(urlOf('/dist', '/dist/client/docs/deep/nested/index.html')).toBe('/docs/deep/nested/');
  });
});
