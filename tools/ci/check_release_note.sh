#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI release-note gate (BLOCKING): user-facing changes must carry a release note.
#
# A change that alters what an operator or a visitor observes, and ships without
# a line in CHANGELOG.md, is invisible. Nobody upgrading learns that the thing
# they were working around is fixed, and nobody debugging learns that behaviour
# moved. The failure is silent by construction: nothing goes red, the release
# simply under-reports itself, and the omission is only ever noticed later by
# someone who needed the note.
#
# So this gate asks one question of every pull request: you touched product
# code — where is the note? It is deliberately easy to satisfy and deliberately
# impossible to ignore. A change with no user-visible effect declares that in
# one line and moves on; what it cannot do is say nothing at all.
#
# Run locally:  bash tools/ci/check_release_note.sh --base origin/main --head HEAD
# Self-test:    bash tools/ci/check_release_note.sh --self-test
#
# In CI the inputs come from the GitHub API instead of git, so the check works
# on the shallow checkout the rest of the job already uses:
#   bash tools/ci/check_release_note.sh --files-from FILE --haystack-from FILE

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------

# The file that carries operator-facing notes.
RELEASE_NOTE_FILE="CHANGELOG.md"

# Product code: changing it can change what someone observes.
USER_FACING_RE='^(src|lib)/'

# Carve-outs inside the product tree that cannot reach a user on their own.
# Tests and build plumbing change no shipped behaviour by themselves; a change
# that alters both a test and the code it covers still trips the gate on the
# code half, which is the intent.
EXEMPT_RE='(^|/)(test|tests|testdata)/|(^|/)[^/]*_test\.(cc|cpp|h|hpp|ts|js)$|(^|/)BUILD(\.bazel)?$|\.md$'

# The waiver. A reason is mandatory: the point is to record the judgement, not
# to provide a mute button. Accepts `-`, `:`, an en dash or an em dash as the
# separator, because people type all four.
WAIVER_RE='^[[:space:]]*Release-Note:[[:space:]]*none[[:space:]]*[-:–—]+[[:space:]]*'
WAIVER_MIN_REASON=12

# ---------------------------------------------------------------------------
# Core decision, kept pure so the self-test can drive every branch
# ---------------------------------------------------------------------------
#
# $1 — newline-separated changed-file list
# $2 — waiver haystack (commit messages and/or PR body)
#
# 0 = pass, 1 = block.
evaluate_release_note() {
  local changed="$1"
  local haystack="$2"
  local triggering note_touched waiver_line reason

  triggering="$(printf '%s\n' "$changed" \
    | grep -E "$USER_FACING_RE" \
    | grep -vE "$EXEMPT_RE" \
    || true)"

  if [ -z "$triggering" ]; then
    echo "OK: no user-facing product code in this change — no release note required."
    return 0
  fi

  note_touched="$(printf '%s\n' "$changed" | grep -Fx "$RELEASE_NOTE_FILE" || true)"
  if [ -n "$note_touched" ]; then
    echo "OK: user-facing change carries a $RELEASE_NOTE_FILE entry."
    return 0
  fi

  # A waiver reason wraps. The gate's own suggested form is a sentence, and a
  # sentence in a commit message runs onto the next line -- so read the waiver
  # line plus its continuation, stopping at a blank line or at the next trailer
  # (Co-Authored-By: and friends). Reading only the first physical line both
  # under-reports the judgement this gate exists to record and can fail the
  # minimum-length check on a reason that is in fact perfectly good.
  waiver_start="$(printf '%s\n' "$haystack" | grep -niE "$WAIVER_RE" | head -n 1 | cut -d: -f1 || true)"
  if [ -n "$waiver_start" ]; then
    waiver_line="$(printf '%s\n' "$haystack" | sed -n "${waiver_start}p")"
    reason="$(printf '%s\n' "$haystack" | awk -v s="$waiver_start" '
        NR < s  { next }
        NR == s { buf = $0; next }
        /^[[:space:]]*$/ { exit }
        /^[[:space:]]*[A-Za-z][A-Za-z-]*:[[:space:]]/ { exit }
        { buf = buf " " $0 }
        END { print buf }' | sed -E "s/$WAIVER_RE//I")"
    # Collapse the joined whitespace, then trim.
    reason="$(printf '%s' "$reason" | sed -E 's/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+$//')"
    if [ "${#reason}" -lt "$WAIVER_MIN_REASON" ]; then
      echo "" >&2
      echo "FAILED: the release-note waiver has no usable reason." >&2
      echo "" >&2
      echo "  found:  $waiver_line" >&2
      echo "" >&2
      echo "A waiver records a judgement, so it has to say something. Write why this" >&2
      echo "change is invisible to users — at least $WAIVER_MIN_REASON characters of actual reason." >&2
      return 1
    fi
    echo "OK: release note waived — $reason"
    return 0
  fi

  echo "" >&2
  echo "FAILED: this change touches user-facing product code but adds no release note." >&2
  echo "" >&2
  echo "Files that triggered the gate:" >&2
  printf '%s\n' "$triggering" | sed 's/^/  /' >&2
  echo "" >&2
  echo "Do one of these:" >&2
  echo "" >&2
  echo "  1. Add an entry to $RELEASE_NOTE_FILE, under the unreleased heading at the" >&2
  echo "     top. Write it for someone deciding whether to upgrade: what they observed" >&2
  echo "     before, what they observe now. See RELEASING.md." >&2
  echo "" >&2
  echo "  2. If nothing a user can observe has changed, say so explicitly:" >&2
  echo "" >&2
  echo "       Release-Note: none — <why this is invisible to users>" >&2
  echo "" >&2
  echo "     Put that line in a commit message (pushing re-runs this check) or in the" >&2
  echo "     pull request description (then re-run this job — editing a description" >&2
  echo "     does not itself re-trigger CI)." >&2
  echo "" >&2
  return 1
}

# ---------------------------------------------------------------------------
# Self-test — every branch above, including the ones that must block
# ---------------------------------------------------------------------------

self_test() {
  local failures=0 n=0

  check() {
    local name="$1" want="$2" changed="$3" haystack="$4"
    local got=0
    n=$((n + 1))
    evaluate_release_note "$changed" "$haystack" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }

  echo "Self-test: release-note gate"

  check "product code + note                 -> pass" 0 \
    "$(printf 'src/worker/rewrite.cc\nCHANGELOG.md\n')" ""

  check "product code, no note, no waiver    -> BLOCK" 1 \
    "$(printf 'src/worker/rewrite.cc\n')" ""

  check "product code + waiver in commit     -> pass" 0 \
    "$(printf 'src/worker/rewrite.cc\n')" \
    "$(printf 'refactor: rename a local\n\nRelease-Note: none - pure rename, no behaviour change\n')"

  check "product code + waiver in PR body    -> pass" 0 \
    "$(printf 'lib/js/js_minify.cc\n')" \
    "Release-Note: none — internal assertion only, unreachable in release builds"

  check "waiver with em dash                 -> pass" 0 \
    "$(printf 'src/a.cc\n')" "Release-Note: none — dead code deleted, never compiled in"

  check "waiver with colon                   -> pass" 0 \
    "$(printf 'src/a.cc\n')" "Release-Note: none: dead code deleted, never compiled in"

  check "waiver with no reason               -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "Release-Note: none -"

  check "waiver with a token reason          -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "Release-Note: none - nfc"

  check "tests only                          -> pass" 0 \
    "$(printf 'test/lib/js/js_minify_test.cc\n')" ""

  check "test file inside src tree           -> pass" 0 \
    "$(printf 'src/worker/rewrite_test.cc\n')" ""

  check "BUILD file only                     -> pass" 0 \
    "$(printf 'src/worker/BUILD\n')" ""

  check "docs and CI only                    -> pass" 0 \
    "$(printf 'docs/configuration.md\n.github/dependabot.yml\n')" ""

  check "samples only                        -> pass" 0 \
    "$(printf 'samples/aspnetcore/samples/DemoSite/Program.cs\n')" ""

  check "code + test, note absent            -> BLOCK" 1 \
    "$(printf 'src/worker/rewrite.cc\nsrc/worker/rewrite_test.cc\n')" ""

  check "note alone, no product code         -> pass" 0 \
    "$(printf 'CHANGELOG.md\n')" ""

  # The separator is a RUN, not one character. The gate's own help text
  # suggests `Release-Note: none -- <why>`, so the double hyphen has to work;
  # before it did, the second `-` survived into the reason and counted toward
  # the minimum length.
  check "waiver with a double hyphen         -> pass" 0 \
    "$(printf 'src/a.cc\n')" "Release-Note: none -- dead code deleted, never compiled in"

  check "double hyphen does not pad a token  -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "Release-Note: none -- nfc"

  # A reason that wraps is read whole, and stops before the next trailer.
  check "waiver wrapping onto the next line  -> pass" 0 \
    "$(printf 'src/a.cc\n')" \
    "$(printf 'fix: something\n\nRelease-Note: none --\n  the whole reason lives on the following line\n\nCo-Authored-By: Someone <x@example.invalid>\n')"

  check "waiver must not match a mention     -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "I wondered whether to write Release-Note: none but decided against it"

  # End-to-end through the entry point, because the CI path and the fail-closed
  # branches live there rather than in the pure function above. A gate that
  # cannot see its input must go red, not quietly green.
  local self="${BASH_SOURCE[0]}" tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN

  e2e() {
    local name="$1" want="$2"; shift 2
    local got=0
    n=$((n + 1))
    bash "$self" "$@" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }

  printf 'src/worker/rewrite.cc\n' > "$tmp/files-block.txt"
  printf 'src/worker/rewrite.cc\nCHANGELOG.md\n' > "$tmp/files-pass.txt"
  printf 'Release-Note: none - build plumbing only, nothing user visible\n' > "$tmp/waiver.txt"

  e2e "--files-from missing               -> BLOCK" 1 --files-from "$tmp/nope.txt"
  e2e "--haystack-from missing            -> BLOCK" 1 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/nope.txt"
  e2e "--files-from, no note              -> BLOCK" 1 --files-from "$tmp/files-block.txt"
  e2e "--files-from, with note            -> pass " 0 --files-from "$tmp/files-pass.txt"
  e2e "--files-from + waiver file         -> pass " 0 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/waiver.txt"
  e2e "no arguments at all                -> BLOCK" 1
  e2e "unresolvable base                  -> BLOCK" 1 --base 0000000000000000000000000000000000000000

  echo ""
  if [ "$failures" -ne 0 ]; then
    echo "FAILED: $failures of $n self-test cases did not behave as specified." >&2
    return 1
  fi
  echo "OK: all $n self-test cases behave as specified."
  return 0
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

BASE=""
HEAD="HEAD"
FILES_FROM=""
HAYSTACK_FROM=""

while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) self_test; exit $? ;;
    --base) BASE="${2:-}"; shift 2 ;;
    --head) HEAD="${2:-}"; shift 2 ;;
    --files-from) FILES_FROM="${2:-}"; shift 2 ;;
    --haystack-from) HAYSTACK_FROM="${2:-}"; shift 2 ;;
    -h|--help)
      sed -n '6,26p' "${BASH_SOURCE[0]}"
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

cd "$REPO_ROOT"

# CI path: the changed-file list and the waiver text are supplied by the caller,
# so no git history is needed and a depth-1 checkout is fine.
if [ -n "$FILES_FROM" ]; then
  if [ ! -f "$FILES_FROM" ]; then
    echo "FAILED: --files-from '$FILES_FROM' does not exist." >&2
    echo "This check cannot run, so it is failing rather than passing silently." >&2
    exit 1
  fi
  HAYSTACK=""
  if [ -n "$HAYSTACK_FROM" ]; then
    if [ ! -f "$HAYSTACK_FROM" ]; then
      echo "FAILED: --haystack-from '$HAYSTACK_FROM' does not exist." >&2
      exit 1
    fi
    HAYSTACK="$(cat "$HAYSTACK_FROM")"
  fi
  evaluate_release_note "$(cat "$FILES_FROM")" "$HAYSTACK"
  exit $?
fi

if [ -z "$BASE" ]; then
  echo "FAILED: --base is required (or pass --self-test / --files-from)." >&2
  exit 1
fi

# Fail closed. A gate that cannot see the diff must not report success — that is
# indistinguishable from a gate that passed, which is the whole failure mode
# this file exists to prevent. A shallow clone is the common cause: the CI job
# needs fetch-depth: 0.
if ! MERGE_BASE="$(git merge-base "$BASE" "$HEAD" 2>/dev/null)" || [ -z "$MERGE_BASE" ]; then
  echo "FAILED: cannot resolve a merge base between '$BASE' and '$HEAD'." >&2
  echo "This check cannot run, so it is failing rather than passing silently." >&2
  echo "In CI, ensure actions/checkout uses fetch-depth: 0." >&2
  exit 1
fi

CHANGED="$(git diff --name-only "$MERGE_BASE" "$HEAD")"
COMMIT_MESSAGES="$(git log --format='%B' "$MERGE_BASE..$HEAD")"
HAYSTACK="$(printf '%s\n%s\n' "$COMMIT_MESSAGES" "${PR_BODY:-}")"

evaluate_release_note "$CHANGED" "$HAYSTACK"
