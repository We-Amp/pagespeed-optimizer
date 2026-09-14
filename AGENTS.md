# AGENTS.md

**`CLAUDE.md` in this directory is the canonical agent guide — read it first.** This
file is a thin pointer for non-Claude tools; it does not duplicate CLAUDE.md, it only
restates the few facts most expensive to miss. Per-subsystem detail lives in the
`*/CLAUDE.md` files listed at the top of CLAUDE.md.

This is a multi-language, multi-build-system repo: Bazel C++23 (worker + nginx
module), C#/.NET middleware (`samples/aspnetcore/`), and a pnpm workbench
(`tools/workbench/`).

## Must-not-miss rules

- **Confirm your checkout first.** This machine hosts many clones/worktrees. Run
  `git rev-parse --show-toplevel` and `pwd` before editing; never edit sibling clones,
  worktrees, or `reference/` (gitignored upstream). See CLAUDE.md → "Canonical
  Checkout & Multi-Clone Safety".
- **Stage files explicitly.** Use `git add <file>...`; never `git add .` or
  `git commit -a`. Pre-commit hooks will modify files — expect a two-pass commit.
- **Touched `src/` or `lib/`? Write the release note in the same PR.** CI blocks
  a pull request that changes product code without a `CHANGELOG.md` entry
  (`tools/ci/check_release_note.sh`). If nothing a user can observe changed, put
  `Release-Note: none — <reason>` in a commit message. A reason is required.
  See RELEASING.md → "Every user-facing change carries its note".
- **clang-format is pinned in `.pre-commit-config.yaml`** (the `mirrors-clang-format`
  rev; CI mirrors it as `clang-format-20` today). Match that rev rather than assuming a
  version — the pin is the source of truth on a bump. Reproduce the CI lint gate with
  `./tools/run-clang-tidy.sh [--fix]`; bare `clang-tidy` is not equivalent.
- **`bazel build //...` excludes the nginx module** (manual tag). Build it with
  `NGINX_PATH=/path/to/nginx bazel build //src/nginx:ngx_pagespeed_module.so`.
- **Cross-process cache invariant.** Both nginx and worker must open the cache with
  `enable_mmap_directory = true` and nginx must use `config.volume_size = 0`, or
  worker writes are invisible to nginx (HIT serves the original, not optimized,
  content). See CLAUDE.md → "Cross-Process Cache Sharing".

Default branch: `main`.
