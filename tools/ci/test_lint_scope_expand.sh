#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/lint-scope-expand.sh against a small synthetic source tree
# (lib/, src/, test/) in a temporary directory.
#
# Run: tools/ci/test_lint_scope_expand.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPAND="${HERE}/lint-scope-expand.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p "${WORK}/lib/a" "${WORK}/src" "${WORK}/test" "${WORK}/lib/js" "${WORK}/src/nginx"
cd "${WORK}"
# leaf.h <- one.cc, two.cc ; mid.h includes leaf.h <- three.cc ;
# top.h includes mid.h <- four.cc (transitive, two hops from leaf.h);
# orphan.h is included by nobody; angle.h is included with angle brackets;
# nginx_user.cc includes leaf.h but is not in the compile database.
: > lib/a/leaf.h
printf '#include "lib/a/leaf.h"\n' > lib/a/mid.h
printf '#include "lib/a/mid.h"\n' > lib/a/top.h
: > lib/a/orphan.h
: > lib/a/angle.h
printf '#include "lib/a/leaf.h"\n#include "gtest/gtest.h"\n' > lib/a/one.cc
printf '#include "lib/a/leaf.h"\n' > src/two.cc
printf '#include "lib/a/mid.h"\n' > src/three.cc
printf '#include "lib/a/top.h"\n' > test/four.cc
printf '#include <lib/a/angle.h>\n' > test/five.cc
printf '#include "lib/a/leaf.h"\n' > src/nginx/early_hints.cc
printf '#include "lib/a/leaf.h"\n' > lib/js/js_vendored.cc
# A same-named header elsewhere must not be confused with lib/a/leaf.h.
mkdir -p lib/b
: > lib/b/leaf.h
printf '#include "lib/b/leaf.h"\n' > src/six.cc
: > lib/a/pair.h
printf '#include "lib/a/pair.h"\n' > src/seven.cc
printf '#include "lib/a/pair.h"\n' > src/eight.cc

CC="${WORK}/cc.txt"
HDR="${WORK}/hdr.txt"
DB="${WORK}/compile_commands.json"
cat > "${DB}" <<'JSON'
[
  {"file": "lib/a/one.cc", "directory": "/workspace"},
  {"file": "src/two.cc", "directory": "/workspace"},
  {"file": "src/three.cc", "directory": "/workspace"},
  {"file": "test/four.cc", "directory": "/workspace"},
  {"file": "test/five.cc", "directory": "/workspace"},
  {"file": "src/six.cc", "directory": "/workspace"}
]
JSON

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# run_case <name> <cc lines> <header lines> <expected output (newline separated)> [compdb]
run_case() {
  local name="$1" cc="$2" hdr="$3" want="$4" db="${5:-}"
  printf '%s' "${cc}" > "${CC}"
  printf '%s' "${hdr}" > "${HDR}"
  local got rc=0
  got="$("${EXPAND}" "${CC}" "${HDR}" ${db:+"${db}"})" || rc=$?
  if [ "${rc}" -ne 0 ]; then fail "${name}: exit ${rc}"; return; fi
  if [ "${got}" = "${want}" ]; then pass "${name}"; else
    fail "${name}: want [${want}] got [${got}]"
  fi
  if printf '%s' "${got}" | grep -q '^[[:space:]]*$' && [ -n "${got}" ]; then
    fail "${name}: blank line emitted"
  fi
}

run_case "changed .cc only" $'src/two.cc\n' "" "src/two.cc"

run_case "leaf header with two includers" "" $'lib/a/pair.h\n' $'src/eight.cc\nsrc/seven.cc'

run_case "same-named header elsewhere is not confused" "" $'lib/b/leaf.h\n' "src/six.cc"

# early_hints.cc and lib/js/js_vendored.cc include leaf.h too; the carve-outs drop them.
run_case "leaf header: direct includers plus the chain above it" "" $'lib/a/leaf.h\n' \
  $'lib/a/one.cc\nsrc/three.cc\nsrc/two.cc\ntest/four.cc'

run_case "transitive chain from the top header only" "" $'lib/a/top.h\n' "test/four.cc"

run_case "mid header reaches the top header's includer" "" $'lib/a/mid.h\n' $'src/three.cc\ntest/four.cc'

run_case "header with no includers" "" $'lib/a/orphan.h\n' ""

run_case "angle-bracket include of a repo header" "" $'lib/a/angle.h\n' "test/five.cc"

run_case "changed .cc plus header are merged" $'src/six.cc\n' $'lib/a/mid.h\n' \
  $'src/six.cc\nsrc/three.cc\ntest/four.cc'

run_case "TU outside the compile database is dropped" "" $'lib/a/leaf.h\n' \
  $'lib/a/one.cc\nsrc/three.cc\nsrc/two.cc\ntest/four.cc' "${DB}"

run_case "changed .cc absent from the database is dropped" $'src/nginx/early_hints.cc\n' "" "" "${DB}"

run_case "empty inputs" "" "" ""

run_case "blank lines in the input lists" $'\n\nsrc/two.cc\n\n' $'\n\n' "src/two.cc"

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "all cases passed"
