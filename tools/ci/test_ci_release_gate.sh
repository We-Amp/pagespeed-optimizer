#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/ci_release_gate.sh with a fake gh. The scenario selects
# what the fake serves; a counter file lets a call be empty, fail or report
# "in flight" the first N times. No network, no token.
#
# Run: tools/ci/test_ci_release_gate.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="${HERE}/ci_release_gate.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${TMP}/bin"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

cat > "${TMP}/bin/gh" <<'GH'
#!/usr/bin/env bash
# Fake gh api. Case table (SCENARIO -> what is served):
#   dispatch-full-green       dispatch run 21, all ten jobs success
#   push-pr-skipped           push run 22, green, the five pr / jobs skipped
#   required-skipped          dispatch run 21, full / Lint (whole tree) skipped
#   coe-red                   dispatch run 21, arm64 rig failure, rest green
#   other-red                 dispatch run 21 (success), full / Linux Pkg Smoke failure
#   flaky-empty-then-green    first dispatch query empty, then run 21
#   inflight-then-green       dispatch run 21 in_progress, then completed
#   api-500                   every call fails with HTTP 500
set -uo pipefail
path="$2"
echo "$path" >> "${FAKE_LOG}"
bump() { local n; n=$(cat "${FAKE_STATE}/$1" 2>/dev/null || echo 0); echo $((n + 1)) > "${FAKE_STATE}/$1"; echo "$n"; }
pr='pr / Linux Build
pr / Linux Unit Tests
pr / Windows Build
pr / Windows Unit Tests
pr / Lint (clang-tidy + clang-format)'
full='full / Linux Build
full / Windows Build
full / Linux ASan + UBSan
full / Linux TSan
full / Lint (whole tree)'
all_with() { # all_with <conclusion for pr jobs>
  while IFS= read -r j; do echo "${j}=$1"; done <<<"$pr"
  while IFS= read -r j; do echo "${j}=success"; done <<<"$full"
}
ev=push
case "$path" in *event=workflow_dispatch*) ev=dispatch ;; esac
case "$path" in
  */workflows/ci.yml/runs*)
    n=$(bump "q_${ev}")
    case "$SCENARIO:$ev" in
      api-500:*) echo "gh: Internal Server Error (HTTP 500)" >&2; exit 1 ;;
      push-pr-skipped:push) echo "22 completed success" ;;
      flaky-empty-then-green:dispatch)
        if [ "$n" -lt 1 ]; then : ; else echo "21 completed success"; fi ;;
      inflight-then-green:dispatch)
        if [ "$n" -lt 2 ]; then echo "21 in_progress null"; else echo "21 completed success"; fi ;;
      dispatch-full-green:dispatch|required-skipped:dispatch|coe-red:dispatch|other-red:dispatch) echo "21 completed success" ;;
      *) : ;;
    esac ;;
  */actions/runs/21/jobs*)
    case "$SCENARIO" in
      required-skipped) all_with success | sed 's#^full / Lint (whole tree)=success#full / Lint (whole tree)=skipped#' ;;
      coe-red) all_with success; echo 'full / Daemon Privilege Rig (arm64)=failure' ;;
      other-red) all_with success; echo 'full / Linux Pkg Smoke=failure' ;;
      *) all_with success; echo 'full / Linux Pkg Smoke=success' ;;
    esac ;;
  */actions/runs/22/jobs*) all_with skipped ;;
  *) echo "fake gh: unexpected call: $path" >&2; exit 9 ;;
esac
GH
chmod +x "${TMP}/bin/gh"

# run_gate <scenario> [VAR=value ...]: sets RC and OUT, resets the fake state.
run_gate() {
  local scenario="$1"; shift
  rm -rf "${TMP}/state"; mkdir -p "${TMP}/state"; : > "${TMP}/log"
  RC=0
  OUT="$(env PATH="${TMP}/bin:${PATH}" SCENARIO="$scenario" FAKE_STATE="${TMP}/state" FAKE_LOG="${TMP}/log" \
    GITHUB_REPOSITORY=o/r RELEASE_SHA=abc POLL_SECONDS=0 RETRY_BASE_SECONDS=0 MAX_WAIT_SECONDS=3 \
    "$@" bash "${GATE}" 2>&1)" || RC=$?
}

run_gate dispatch-full-green
if [ "$RC" -eq 0 ] && grep -q 'passed in ci.yml run 21 (workflow_dispatch)' <<<"$OUT"; then
  pass "dispatch-full-green"
else fail "dispatch-full-green (rc=$RC)"; echo "$OUT"; fi

# query-pins-main-and-ci-yml: reuses the log of the run above.
q_ok=1; q_n=0
while IFS= read -r line; do
  case "$line" in */actions/runs/*) continue ;; esac
  q_n=$((q_n + 1))
  case "$line" in */workflows/ci.yml/runs*) : ;; *) q_ok=0 ;; esac
  case "$line" in *branch=main*) : ;; *) q_ok=0 ;; esac
  case "$line" in *head_sha=*) : ;; *) q_ok=0 ;; esac
done < "${TMP}/log"
if [ "$q_ok" -eq 1 ] && [ "$q_n" -gt 0 ]; then pass "query-pins-main-and-ci-yml"; else fail "query-pins-main-and-ci-yml (n=$q_n)"; cat "${TMP}/log"; fi

run_gate push-pr-skipped
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 22: pr / Linux Build is not success' <<<"$OUT" && [ "$(grep -c 'waiting' <<<"$OUT")" -eq 1 ]; then
  pass "push-pr-tier-skipped-refused"
else fail "push-pr-tier-skipped-refused (rc=$RC)"; echo "$OUT"; fi

run_gate required-skipped
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 21: full / Lint (whole tree) is not success' <<<"$OUT"; then
  pass "required-job-skipped-refused"
else fail "required-job-skipped-refused (rc=$RC)"; echo "$OUT"; fi

run_gate coe-red
if [ "$RC" -eq 0 ] && grep -q 'passed in ci.yml run 21' <<<"$OUT"; then
  pass "coe-job-red-accepted"
else fail "coe-job-red-accepted (rc=$RC)"; echo "$OUT"; fi

run_gate other-red
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 21: full / Linux Pkg Smoke is failure' <<<"$OUT"; then
  pass "other-job-red-refused"
else fail "other-job-red-refused (rc=$RC)"; echo "$OUT"; fi

run_gate flaky-empty-then-green
if [ "$RC" -eq 0 ] && [ "$(grep -c 'waiting' <<<"$OUT")" -eq 1 ] && grep -q 'passed in ci.yml run 21' <<<"$OUT"; then
  pass "flaky-empty-then-green"
else fail "flaky-empty-then-green (rc=$RC)"; echo "$OUT"; fi

run_gate inflight-then-green
if [ "$RC" -eq 0 ] && grep -q 'still in flight' <<<"$OUT" && grep -q 'passed in ci.yml run 21' <<<"$OUT"; then
  pass "inflight-then-green"
else fail "inflight-then-green (rc=$RC)"; echo "$OUT"; fi

run_gate api-500
if [ "$RC" -eq 1 ] && [ "$(wc -l < "${TMP}/log")" -eq 5 ] && grep -q 'failed after 5 tries' <<<"$OUT" && grep -q 'refuses' <<<"$OUT"; then
  pass "api-500-exhausted"
else fail "api-500-exhausted (rc=$RC)"; echo "$OUT"; fi

run_gate required-skipped DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q '::warning::ci gate refuses this release: ci.yml run 21: full / Lint (whole tree) is not success' <<<"$OUT"; then
  pass "dry-run-refusal-warns"
else fail "dry-run-refusal-warns (rc=$RC)"; echo "$OUT"; fi

# The optimizer workflow passes tag_sha || github.sha; a pre-tag dry run has an
# empty tag_sha, so the fallback commit is what the script queries.
run_gate dispatch-full-green RELEASE_SHA=fallbacksha DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q 'passed in ci.yml run 21' <<<"$OUT" && grep -q 'head_sha=fallbacksha' "${TMP}/log"; then
  pass "dry-run-empty-sha-uses-fallback"
else fail "dry-run-empty-sha-uses-fallback (rc=$RC)"; echo "$OUT"; fi

run_gate dispatch-full-green RELEASE_SHA= DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q '::warning::.*RELEASE_SHA is empty' <<<"$OUT" && [ ! -s "${TMP}/log" ]; then
  pass "empty-sha-dry-run-warns"
else fail "empty-sha-dry-run-warns (rc=$RC)"; echo "$OUT"; fi

run_gate dispatch-full-green RELEASE_SHA=
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: RELEASE_SHA is empty' <<<"$OUT"; then
  pass "empty-sha-real-refused"
else fail "empty-sha-real-refused (rc=$RC)"; echo "$OUT"; fi

# The jobs call reads the latest attempt, all pages.
run_gate dispatch-full-green
if grep -q 'runs/21/jobs?filter=latest&per_page=100' "${TMP}/log"; then pass "jobs-call-latest-attempt"; else fail "jobs call lacks filter=latest"; cat "${TMP}/log"; fi

[ "$FAILS" -eq 0 ] || { echo "$FAILS failure(s)"; exit 1; }
echo "all ok"
