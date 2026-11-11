// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Unit tests for the pure parts of scripts/visual-diff.mjs (the opt-in
// before/after tool): what the build-output normalization strips and keeps,
// the rule-level CSS split, the pixel comparison, and the preconditions that
// make the tool refuse to run. No browser and no server is involved.

import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, describe, expect, it, vi } from 'vitest';
import {
  DEFAULT_PATHS,
  comparePixels,
  distDiff,
  formatPercent,
  freezeSvg,
  main,
  multisetDiff,
  normalizeText,
  slugForPath,
  splitCssRules,
} from '../scripts/visual-diff.mjs';

const image = (width: number, height: number, fill = 0) => ({
  data: Buffer.alloc(width * height * 4, fill),
  width,
  height,
});

describe('normalizeText', () => {
  it('strips scoped-style ids, bundle hashes and the build-id tag', () => {
    const a =
      '<meta name="build-id" content="base-1"><link href="/_astro/BaseLayout.CRGEekDt.css">' +
      '<p data-astro-cid-airhgkwu class="x">hi</p><style>.x[data-astro-cid-airhgkwu]{color:red}</style>' +
      '<script src="/_astro/page.B1x_y-Zq.js"></script>';
    const b =
      '<meta name="build-id" content="head-2"><link href="/_astro/BaseLayout.Zz019aQw.css">' +
      '<p data-astro-cid-jnnmztss class="x">hi</p><style>.x[data-astro-cid-jnnmztss]{color:red}</style>' +
      '<script src="/_astro/page.Qq7_abcd.js"></script>';
    expect(normalizeText(a)).toBe(normalizeText(b));
    expect(normalizeText(a)).toContain('/_astro/BaseLayout.css');
    expect(normalizeText(a)).not.toContain('build-id');
  });

  it('keeps real differences and content-hashed images', () => {
    expect(normalizeText('<p class="x">hi</p>')).not.toBe(normalizeText('<p class="y">hi</p>'));
    const img = '<img src="/_astro/hero.D4f5g6h7.webp">';
    expect(normalizeText(img)).toBe(img);
  });
});

describe('splitCssRules', () => {
  it('descends into grouping at-rules and keeps other blocks whole', () => {
    const rules = splitCssRules(
      '@layer a,b;.a{color:red}@media (min-width:1px){.b{color:blue}@supports (x:y){.c{d:e}}}' +
        '@keyframes k{0%{o:0}to{o:1}}.d:after{content:"}"}',
    );
    expect(rules).toEqual([
      ' | @layer a,b;',
      ' | .a{color:red}',
      '@media (min-width:1px) | .b{color:blue}',
      '@media (min-width:1px) @supports (x:y) | .c{d:e}',
      ' | @keyframes k{0%{o:0}to{o:1}}',
      ' | .d:after{content:"}"}',
    ]);
  });

  it('feeds a multiset diff that ignores order and reports each side', () => {
    const base = splitCssRules('.a{x:1}.b{x:2}.b{x:2}');
    const head = splitCssRules('.b{x:2}.a{x:1}.c{x:3}');
    expect(multisetDiff(base, head)).toEqual({ onlyA: [' | .b{x:2}'], onlyB: [' | .c{x:3}'] });
    expect(multisetDiff(base, [...base].reverse())).toEqual({ onlyA: [], onlyB: [] });
  });
});

describe('distDiff', () => {
  const root = mkdtempSync(join(tmpdir(), 'visual-diff-'));
  afterAll(() => rmSync(root, { recursive: true, force: true }));
  const build = (name: string, hash: string, cid: string, body: string, css: string) => {
    const client = join(root, name, 'dist', 'client');
    mkdirSync(join(client, '_astro'), { recursive: true });
    mkdirSync(join(client, 'about'), { recursive: true });
    const page =
      `<meta name="build-id" content="${name}"><link href="/_astro/site.${hash}.css">` +
      `<main data-astro-cid-${cid}>${body}</main>`;
    writeFileSync(join(client, 'index.html'), page);
    writeFileSync(join(client, 'about', 'index.html'), page);
    writeFileSync(join(client, '_astro', `site.${hash}.css`), css);
    return join(root, name);
  };

  it('is empty for two builds that differ only in generated names', () => {
    const a = build('a', 'AAAAaaaa', 'abc123', 'same', '.x[data-astro-cid-abc123]{color:red}');
    const b = build('b', 'BBBBbbbb', 'def456', 'same', '.x[data-astro-cid-def456]{color:red}');
    const result = distDiff(a, b);
    expect(result.empty).toBe(true);
    expect(result.html.compared).toBe(2);
  });

  it('reports a changed page and a changed rule', () => {
    const a = build('c', 'AAAAaaaa', 'abc123', 'before', '.x{color:red}.y{top:0}');
    const b = build('d', 'BBBBbbbb', 'def456', 'after', '.x{color:blue}.y{top:0}');
    const result = distDiff(a, b);
    expect(result.empty).toBe(false);
    expect(result.html.changed.map((c) => c.file).sort()).toEqual([
      'about/index.html',
      'index.html',
    ]);
    expect(result.css.onlyBase).toEqual([' | .x{color:red}']);
    expect(result.css.onlyHead).toEqual([' | .x{color:blue}']);
  });
});

describe('comparePixels', () => {
  it('reports zero for equal images', () => {
    expect(comparePixels(image(4, 3, 7), image(4, 3, 7))).toMatchObject({ changed: 0, total: 12 });
  });

  it('counts changed pixels, their rows and the largest channel difference', () => {
    const a = image(4, 3);
    const b = image(4, 3);
    b.data[(1 * 4 + 2) * 4] = 9;
    b.data[(2 * 4 + 0) * 4 + 1] = 2;
    expect(comparePixels(a, b)).toMatchObject({ changed: 2, maxDelta: 9, top: 1, bottom: 2 });
    expect(comparePixels(a, b, 2)).toMatchObject({ changed: 1, top: 1, bottom: 1 });
  });

  it('counts the pixels only one image has as changed', () => {
    const cmp = comparePixels(image(4, 3), image(4, 5));
    expect(cmp).toMatchObject({ width: 4, height: 5, total: 20, changed: 8, top: 3, bottom: 4 });
  });
});

describe('helpers', () => {
  it('formats the changed share', () => {
    expect(formatPercent(0, 100)).toBe('0%');
    expect(formatPercent(1, 1_000_000)).toBe('<0.01%');
    expect(formatPercent(125, 1000)).toBe('12.50%');
  });

  it('names a capture after its path', () => {
    expect(slugForPath('/')).toBe('home');
    expect(slugForPath('/docs/getting-started/')).toBe('docs_getting-started');
    expect(new Set(DEFAULT_PATHS.map(slugForPath)).size).toBe(DEFAULT_PATHS.length);
  });

  it('includes the error pages in the default path list', () => {
    expect(DEFAULT_PATHS).toContain('/error/404/');
    expect(DEFAULT_PATHS).toContain('/error/500/');
  });

  it('switches off animations inside an svg image and leaves a still one alone', () => {
    const still = '<svg><rect/></svg>';
    expect(freezeSvg(still)).toBe(still);
    const moving =
      '<svg><style>@keyframes b{to{opacity:0}}.c{animation:b 1s 4}</style>' +
      '<rect class="c"><animate attributeName="x" to="1"/></rect></svg>';
    const frozen = freezeSvg(moving);
    expect(frozen).not.toContain('<animate');
    expect(frozen).toContain('animation:none !important');
  });
});

describe('preconditions', () => {
  const run = async (argv: string[]) => {
    const err = vi.spyOn(console, 'error').mockImplementation(() => {});
    try {
      const code = await main(argv);
      return { code, message: err.mock.calls.map((c) => String(c[0])).join('\n') };
    } finally {
      err.mockRestore();
    }
  };
  const shots = ['shots', '--base', 'http://127.0.0.1:9', '--head', 'http://127.0.0.1:9'];

  it('refuses two equal build ids', async () => {
    const { code, message } = await run([
      ...shots,
      '--base-id',
      'x',
      '--head-id',
      'x',
      '--out',
      'o',
    ]);
    expect(code).toBe(2);
    expect(message).toContain('build ids are equal');
  });

  it('refuses an origin that is not local', async () => {
    const { code, message } = await run([
      'shots',
      '--base',
      'https://example.com',
      '--head',
      'http://127.0.0.1:9',
      '--base-id',
      'a',
      '--head-id',
      'b',
      '--out',
      'o',
    ]);
    expect(code).toBe(2);
    expect(message).toContain('not a loopback address');
  });

  it('refuses an origin where nothing answers', async () => {
    const { code, message } = await run([
      ...shots,
      '--base-id',
      'a',
      '--head-id',
      'b',
      '--out',
      'o',
    ]);
    expect(code).toBe(2);
    expect(message).toContain('nothing answers');
  });

  it('rejects an unknown mode', async () => {
    expect((await run(['nope'])).code).toBe(2);
  });
});
