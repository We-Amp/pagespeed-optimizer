#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for the public-tree hygiene gate: one fixture per new rule class,
# each of which must make the gate FAIL, plus a clean tree that must pass.
# The blocked literals are non-public, so they are not written here: fixtures
# are stored rot13-encoded and decoded at run time.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dec() { printf '%s' "$1" | tr 'A-Za-z' 'N-ZA-Mn-za-m'; }

run_case() { # name expect(pass|fail) file content
  local name="$1" expect="$2" file="$3" content="$4" tmp rc=0
  tmp="$(mktemp -d)"
  mkdir -p "$tmp/tools/ci" "$tmp/.github" "$tmp/docs"
  cp "$ROOT/tools/ci/check-public-hygiene."{sh,py} "$tmp/tools/ci/"
  cp "$ROOT/.github/hygiene-rules.txt" "$ROOT/.github/hygiene-hashed-tokens.txt" "$tmp/.github/"
  printf 'ok\n' > "$tmp/docs/a.md"
  mkdir -p "$(dirname "$tmp/$file")"
  printf '%s\n' "$content" > "$tmp/$file"
  (cd "$tmp" && git init -q && git add -A && tools/ci/check-public-hygiene.sh >/dev/null 2>&1) || rc=$?
  rm -rf "$tmp"
  if [[ "$expect" == pass && $rc -ne 0 ]] || [[ "$expect" == fail && $rc -eq 0 ]]; then
    echo "FAIL: $name (expected $expect, rc=$rc)" >&2; return 1
  fi
  echo "ok: $name"
}

rc=0
run_case clean pass docs/b.md 'See [a](a.md) and [dir](../docs).' || rc=1
run_case host-extra fail docs/b.txt "$(dec 'ehaf ba zco-v9 abj')" || rc=1
run_case host-prefix fail docs/b.txt "$(dec 'ehaf ba ohefg-yvahk abj')" || rc=1
run_case build-path fail docs/b.txt "$(dec '/zag/q/pv/jbex')" || rc=1
run_case tooling-path fail docs/b.txt "$(dec 'frr gbbyf/choyvfu/ ')" || rc=1
run_case private-slug fail docs/b.md "$(dec 'uggcf://tvguho.pbz/Jr-Nzc/nqzvavfgengvr')" || rc=1
run_case license-header fail docs/b.txt "$(dec 'FCQK-Yvprafr-Vqragvsvre: OHFY-1.1')" || rc=1
run_case license-title fail docs/LICENSE.txt "$(dec 'Ohfvarff Fbhepr Yvprafr 1.1')" || rc=1
run_case md-only-path fail docs/b.md "$(dec 'frr qbpf/cynaf/k.zq')" || rc=1
run_case md-only-path-nonmd pass docs/b.txt "$(dec 'frr qbpf/cynaf/k.zq')" || rc=1
run_case dead-link fail docs/b.md 'see [x](missing.md)' || rc=1
run_case dead-link-in-fence pass docs/b.md $'```\n[x](missing.md)\n```' || rc=1
run_case url-link pass docs/b.md '[x](https://example.com/y) [f](#frag)' || rc=1
exit $rc
