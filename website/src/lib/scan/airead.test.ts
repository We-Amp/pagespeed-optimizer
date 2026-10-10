// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Grading parity: the v2 panel's formatter must produce the same grade, score,
// word and category rows as the original result page. The original is an
// inline script, so its grading block is sliced out of the page source and
// run in a sandbox, the same way the gate sync test does it.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import { describe, expect, it } from 'vitest';
import { formatAiread } from './airead';

const PAGE = fileURLToPath(new URL('../../pages/ai-readability/index.astro', import.meta.url));
const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      fileURLToPath(new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url)),
      'utf8',
    ),
  ).report;

function v1Html(r: unknown): { grade: string; cats: string } {
  const src = readFileSync(PAGE, 'utf8');
  const escStart = src.indexOf('const esc = (s) =>');
  const gradeStart = src.indexOf('const grade =\n', src.indexOf('function render(data)'));
  const gradeEnd = src.indexOf('// SECTION 2', gradeStart);
  if (escStart < 0 || gradeStart < 0 || gradeEnd < 0) throw new Error('v1 anchors moved');
  const escSrc = extractStatement(src, escStart);
  const code = `${escSrc}\n${src.slice(gradeStart, gradeEnd)}\nglobalThis.__out__ = { grade, cats };`;
  const ctx = vm.createContext({ r });
  vm.runInContext(code, ctx);
  return (ctx as { __out__: { grade: string; cats: string } }).__out__;
}

// Take a `const x = ...;` statement by balancing brackets up to the closing semicolon.
function extractStatement(src: string, start: number): string {
  let depth = 0;
  for (let i = start; i < src.length; i++) {
    const ch = src[i];
    if ('([{'.includes(ch)) depth++;
    else if (')]}'.includes(ch)) depth--;
    else if (ch === ';' && depth === 0) return src.slice(start, i + 1);
  }
  throw new Error('unterminated statement');
}

const decode = (s: string) =>
  s
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&');
// Drop everything between a '<' and the next '>' by walking the string, so no
// tag pattern is matched with a regular expression.
function stripTags(html: string): string {
  let out = '';
  let inTag = false;
  for (const ch of html) {
    if (ch === '<') inTag = true;
    else if (ch === '>' && inTag) inTag = false;
    else if (!inTag) out += ch;
  }
  return out;
}
const text = (html: string) => decode(stripTags(html));

describe.each(['full', 'clean'])('grading parity (%s report)', (name) => {
  const report = fixture(name);
  const m = formatAiread(report);
  const v1 = v1Html(report);

  it('grade, score and word match', () => {
    expect(v1.grade).toContain(`>${m.grade}</div>`);
    expect(v1.grade).toContain(`<div class="ar-score">${m.score}/100</div>`);
    expect(text(v1.grade)).toContain(`Grade ${m.grade} — ${m.word}`);
    expect(m.word).not.toBe('');
  });

  it('the five category rows match', () => {
    const rows = v1.cats.split('<div class="ar-cat">').slice(1);
    expect(rows).toHaveLength(5);
    expect(m.categories).toHaveLength(5);
    rows.forEach((row, i) => {
      const c = m.categories[i];
      const flag = /<span class="ar-flag">([^<]*)<\/span>/.exec(row)?.[1] ?? '';
      const nameSpan =
        /<div class="ar-top"><span>(.*?)(?:<span class="ar-flag">|<\/span><span>)/.exec(row)![1];
      expect(decode(nameSpan)).toBe(c.name);
      expect(flag).toBe(c.flag);
      expect(row).toContain(`<span>${c.score}/${c.max}</span>`);
      expect(row).toContain(`<i style="width:${c.pct}%">`);
      expect(decode(/<div class="ar-v">(.*?)<\/div>/.exec(row)![1])).toBe(c.verdict);
      expect(text(row)).toContain(c.verdict);
    });
  });
});

describe('formatAiread extras', () => {
  it('flags the content gap on the full report only', () => {
    expect(formatAiread(fixture('full')).categories.map((c) => c.flag)).toContain(
      'invisible without JS',
    );
    expect(formatAiread(fixture('clean')).categories.every((c) => c.flag === '')).toBe(true);
  });
  it('fires the three gate sentences on the full report only', () => {
    expect(formatAiread(fixture('full')).alsoFound.map((f) => f.wedge)).toEqual([
      'tollbooth',
      'agentpass',
      'compliancefix',
    ]);
    expect(formatAiread(fixture('clean')).alsoFound).toEqual([]);
  });
  it('builds the token line from the report', () => {
    expect(formatAiread(fixture('full')).tokenLine).toBe(
      'Readable text in raw HTML: ~190 tokens · after JavaScript: ~540 tokens',
    );
  });
});
