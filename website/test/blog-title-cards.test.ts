// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// blog-title-cards.test.ts: every published blog post needs its generated title
// card (public/og-cards/<slug>.png) committed with it. The build renders a
// missing card on the fly, so a post that lands without one still gets a social
// image, but every later build then leaves an untracked PNG in the tree. A post
// that sets `coverImage` brings its own image and needs no card.

import { spawnSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const blogDir = path.join(root, 'src/content/blog');
const cardDir = path.join(root, 'public/og-cards');

/** Slugs of the posts that rely on a generated card. */
function postsNeedingCard(): string[] {
  return readdirSync(blogDir)
    .filter((f) => /\.mdx?$/.test(f))
    .filter((file) => {
      const fm =
        /^---\r?\n([\s\S]*?)\r?\n---/.exec(readFileSync(path.join(blogDir, file), 'utf8'))?.[1] ??
        '';
      return !/^draft:\s*true\s*$/m.test(fm) && !/^coverImage:\s*\S/m.test(fm);
    })
    .map((file) => file.replace(/\.mdx?$/, ''));
}

/**
 * Card files git tracks, or null outside a git checkout (a source tarball),
 * where the check falls back to the file being present.
 */
function trackedCards(): Set<string> | null {
  const res = spawnSync('git', ['ls-files', '-z', '--', '.'], { cwd: cardDir, encoding: 'utf8' });
  if (res.status !== 0) return null;
  return new Set(res.stdout.split('\0').filter(Boolean));
}

describe('blog title cards', () => {
  it('every published post has its title card committed', () => {
    const slugs = postsNeedingCard();
    expect(slugs.length).toBeGreaterThan(0);
    const tracked = trackedCards();
    const missing = slugs.filter((slug) =>
      tracked ? !tracked.has(`${slug}.png`) : !existsSync(path.join(cardDir, `${slug}.png`)),
    );
    expect(
      missing,
      'run the build once and commit the generated public/og-cards/<slug>.png',
    ).toEqual([]);
  });
});
