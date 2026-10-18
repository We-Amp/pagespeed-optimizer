#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Release guard of the optimizer lane (pipeline-release-optimizer.yml): the tag
# `optimizer-v<X>` must name the version VERSION.txt carries at the tagged
# commit -- the number the daemon's --version and /v1/health report, generated
# from VERSION.txt. The NuGet lane holds `v<X>` to VERSION.txt the same way
# (pipeline-release-nuget.yml, "Verify VERSION.txt matches the release tag").
#
# Enforced from 2.5.0 on. Before it the host packages were numbered on the
# module's 1.x line while VERSION.txt carried the product line (2.N ==
# 1.(N+15): optimizer-v1.19.0 was built from VERSION.txt 2.4.0), and a hotfix
# on that line must still release; below 2.5.0 the guard passes with a notice.
# Dry-run tags (`-pre`, `-pre.N`) are not releases and are ignored the same way.
# A release candidate (optimizer-v2.5.0-rc.1) is cut from a commit whose
# VERSION.txt names the release it leads to, so VERSION.txt may equal the full
# version or its X.Y.Z core.
#
#   check_optimizer_tag_version.sh TAG VERSION_FILE [--dry-run]
#       exit 0: match, or a skip (notice); 1: mismatch (a warning and exit 0
#       under --dry-run); 2: bad input.
#   check_optimizer_tag_version.sh --self-test
set -euo pipefail

ENFORCED_SINCE="2.5.0"
SEMVER_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-([0-9A-Za-z]+(\.[0-9A-Za-z]+)*))?$'

# core_lt A B: X.Y.Z A < X.Y.Z B, numerically per field.
core_lt() {
  local IFS=. i
  # shellcheck disable=SC2206  # splitting on IFS=. is the point
  local -a a=($1) b=($2)
  for i in 0 1 2; do
    (( a[i] < b[i] )) && return 0
    (( a[i] > b[i] )) && return 1
  done
  return 1
}

# verdict TAG VERSION_TEXT: prints "pass|skip|fail<TAB>message"; returns 2 on
# bad input.
verdict() {
  local tag="$1" have="$2" ver core pre
  case "$tag" in
    optimizer-v[0-9]*) ;;
    *) echo "tag '$tag' does not match optimizer-v<X.Y.Z>[-<prerelease>]" >&2; return 2 ;;
  esac
  ver="${tag#optimizer-v}"
  if ! printf '%s' "$ver" | grep -Eq "$SEMVER_RE"; then
    echo "'$ver' is not X.Y.Z[-prerelease] (no build metadata)" >&2; return 2
  fi
  core="${ver%%-*}"
  pre=""; case "$ver" in *-*) pre="${ver#*-}" ;; esac
  have="$(printf '%s' "$have" | tr -d '[:space:]')"
  if [[ "$pre" =~ ^pre([.0-9]|$) ]]; then
    printf 'skip\t%s is a dry-run tag (-%s); the VERSION.txt guard ignores it\n' "$tag" "$pre"
  elif core_lt "$core" "$ENFORCED_SINCE"; then
    printf 'skip\t%s is below %s: the legacy 1.x host-package numbering pairs it with VERSION.txt %s; the equality is enforced from %s on\n' \
      "$tag" "$ENFORCED_SINCE" "${have:-?}" "$ENFORCED_SINCE"
  elif [ "$have" = "$ver" ] || [ "$have" = "$core" ]; then
    printf 'pass\tVERSION.txt=%s matches release tag %s\n' "$have" "$tag"
  else
    printf 'fail\tVERSION.txt (%s) != release tag (%s -> %s). Bump VERSION.txt on main and tag the bump commit.\n' \
      "$have" "$tag" "$core"
  fi
}

run() {
  local tag="${1:-}" file="${2:-}" dry="${3:-}" out v msg
  [ -f "$file" ] || { echo "::error::no such VERSION file: $file" >&2; return 2; }
  if ! out="$(verdict "$tag" "$(cat "$file")" 2>&1)"; then
    echo "::error::$out" >&2; return 2
  fi
  v="${out%%$'\t'*}"; msg="${out#*$'\t'}"
  case "$v" in
    pass) echo "$msg" ;;
    skip) echo "::notice::$msg" ;;
    fail)
      if [ "$dry" = --dry-run ]; then echo "::warning::dry run: $msg"; return 0; fi
      echo "::error::$msg" >&2; return 1 ;;
  esac
}

self_test() {
  local fail=0 tag have want got line
  while read -r tag have want; do
    [ -n "$tag" ] || continue
    [ "$have" = '-' ] && have=""
    got="$(verdict "$tag" "$have" 2>/dev/null)" || got="refused"
    got="${got%%$'\t'*}"
    if [ "$got" = "$want" ]; then line=ok; else line="FAIL (want $want)"; fail=1; fi
    printf '  %-26s %-12s %-8s %s\n' "$tag" "${have:-''}" "$got" "$line"
  done <<'EOF'
optimizer-v2.5.0           2.5.0       pass
optimizer-v2.5.1           2.5.0       fail
optimizer-v2.5.0           2.4.0       fail
optimizer-v2.5.0-rc.1      2.5.0       pass
optimizer-v2.5.0-rc.1      2.5.0-rc.1  pass
optimizer-v2.5.0-rc.1      2.4.0       fail
optimizer-v2.10.0          2.10.0      pass
optimizer-v2.10.0          2.1.0       fail
optimizer-v3.0.0           2.9.0       fail
optimizer-v1.19.0          2.4.0       skip
optimizer-v1.19.1          2.4.0       skip
optimizer-v1.100.0         2.4.0       skip
optimizer-v2.4.0           2.3.0       skip
optimizer-v1.19.1-pre.1    2.4.0       skip
optimizer-v2.5.0-pre.2     2.4.0       skip
optimizer-v2.5.0-preview.1 2.4.0       fail
optimizer-v2.5.0           -           fail
optimizer-v2.5             2.5.0       refused
optimizer-v2.5.0+r1        2.5.0       refused
optimizer-v02.5.0          2.5.0       refused
v2.5.0                     2.5.0       refused
module-v2.5.0              2.5.0       refused
EOF
  # run(): exit codes and the dry-run downgrade, end to end
  local t; t="$(mktemp -d)"
  printf '2.5.0\n' > "$t/V"
  run optimizer-v2.5.0 "$t/V" >/dev/null 2>&1 || { echo "FAIL: run pass"; fail=1; }
  run optimizer-v1.19.0 "$t/V" >/dev/null 2>&1 || { echo "FAIL: run skip"; fail=1; }
  if run optimizer-v2.5.1 "$t/V" >/dev/null 2>&1; then echo "FAIL: run mismatch accepted"; fail=1; fi
  run optimizer-v2.5.1 "$t/V" --dry-run 2>/dev/null | grep -q '^::warning::dry run' \
    || { echo "FAIL: dry-run mismatch should warn"; fail=1; }
  local rc=0; run optimizer-v2.5 "$t/V" >/dev/null 2>&1 || rc=$?
  [ "$rc" = 2 ] || { echo "FAIL: bad tag exit $rc, want 2"; fail=1; }
  rc=0; run optimizer-v2.5.0 "$t/missing" >/dev/null 2>&1 || rc=$?
  [ "$rc" = 2 ] || { echo "FAIL: missing file exit $rc, want 2"; fail=1; }
  rm -rf "$t"
  if [ "$fail" = 0 ]; then echo "check_optimizer_tag_version self-test: ok"; fi
  return "$fail"
}

case "${1:-}" in
  --self-test) self_test ;;
  *) [ $# -ge 2 ] || { echo "usage: $0 TAG VERSION_FILE [--dry-run] | --self-test" >&2; exit 2; }
     run "$@" ;;
esac
