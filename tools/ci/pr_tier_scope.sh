#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Fleet scope of the pull request tier (docs/ci/pipelines.md, "CI_SCOPE").
# Decides whether the fleet jobs of pipeline-pr.yml run for this event.
#
# The workflow reads this file from the default branch and runs it from the
# runner's temp directory, never from the tree under test. merge_group and
# workflow_dispatch never reach it: the workflow decides those inline (always
# the full tier).
#
# Env: GITHUB_EVENT_NAME, GITHUB_REPOSITORY, CI_SCOPE (the repository
# variable), SHA (the commit under test), GH_TOKEN (checks: read),
# FORK_OR_BOT, PR_FULL, BEFORE_SHA (the push's previous tip, optional). Run
# from a checkout of SHA with its parent commit; with BEFORE_SHA, also with
# that commit.
#
# Stdout: exactly two lines, `fleet=<true|false>` and `heavy=<true|false>`.
# Everything else goes to stderr: one `fleet scope:` line and, on a push, one
# `shadow:` line (what the push rule would decide, whatever CI_SCOPE says).
#
# The push rule: a push whose commit the merge queue already proved (a
# completed, successful check suite of GitHub Actions on a gh-readonly-queue/
# branch for the same SHA, and the pull request of every commit in the push,
# looked up by the last "(#N)" of its subject, is same-repo and not from
# dependabot[bot])
# skips the fleet jobs, unless the push changes the pipeline itself. A group
# with a fork or Dependabot pull request ran on hosted runners, so it is not
# proof. It is on only when CI_SCOPE is push, pr-build or pr; any
# other value, or none, is `full` and runs everything. Every other event runs
# everything. Anything unexpected fails safe to the full tier.

set -uo pipefail

log() { echo "$*" >&2; }

event="${GITHUB_EVENT_NAME:-}"
scope="$(printf '%s' "${CI_SCOPE:-}" | tr '[:upper:]' '[:lower:]')"
case "${scope}" in
  push|pr-build|pr) ;;
  *) scope=full ;;
esac

fleet=true
heavy=true
reason="event ${event:-none}"

# pipeline_change: the push touches the PR tier's own definition, or there is
# no sound range to compare. With BEFORE_SHA set (a push carries up to a few
# squashed commits), the range is BEFORE_SHA..HEAD; an all-zero or unreachable
# BEFORE_SHA is a change. Without it, the range is HEAD^..HEAD. Renames are not
# detected: git lists only the destination of a rename by default, which would
# hide a move out of a pipeline path.
pipeline_change() {
  local changed base
  git rev-parse -q --verify "HEAD^{commit}" >/dev/null 2>&1 || return 0
  if [ -n "${BEFORE_SHA:-}" ]; then
    case "${BEFORE_SHA}" in
      *[!0]*) ;;
      *) return 0 ;;
    esac
    git rev-parse -q --verify "${BEFORE_SHA}^{commit}" >/dev/null 2>&1 || return 0
    base="${BEFORE_SHA}"
  else
    git rev-parse -q --verify "HEAD^^{commit}" >/dev/null 2>&1 || return 0
    base="HEAD^"
  fi
  changed="$(git diff --no-renames --name-only "${base}" HEAD 2>/dev/null)" || return 0
  # A here-string, not a pipe: grep -q exits at the first match, and under
  # pipefail a writer killed by SIGPIPE would turn a match into a miss.
  grep -qE '^(\.github/workflows/(pipeline-pr|ci)\.yml$|\.github/actions/|tools/ci/)' <<< "${changed}"
}

# pr_status <n>: prints true, false or error for one pull request: true when
# it is same-repo, not a fork and not authored by dependabot[bot]; false when
# it is a fork, from another repository or Dependabot; error for anything
# unknown (failed lookup, missing head repository).
pr_status() {
  local n="$1" tries=0 delay=2 out fork login full want_lc full_lc
  while :; do
    tries=$((tries + 1))
    if out="$(gh api "repos/${GITHUB_REPOSITORY}/pulls/${n}" \
      --jq '[(.head.repo.fork | tostring), (.user.login // ""), (.head.repo.full_name // "")] | @tsv' 2>/dev/null)" &&
      [ -n "${out}" ]; then
      break
    fi
    if [ "${tries}" -ge 3 ]; then
      echo error
      return
    fi
    sleep "${delay}"
    delay=$((delay * 2))
  done
  fork="${out%%$'\t'*}"
  out="${out#*$'\t'}"
  login="${out%%$'\t'*}"
  full=""
  case "${out}" in *$'\t'*) full="${out#*$'\t'}" ;; esac
  want_lc="$(printf '%s' "${GITHUB_REPOSITORY}" | tr '[:upper:]' '[:lower:]')"
  full_lc="$(printf '%s' "${full}" | tr '[:upper:]' '[:lower:]')"
  if [ "${fork}" != "false" ] && [ "${fork}" != "true" ]; then
    echo error
  elif [ "${fork}" = "true" ] || [ "${full_lc}" != "${want_lc}" ] || [ "${login}" = "dependabot[bot]" ] || [ -z "${login}" ] || [ -z "${full}" ]; then
    echo false
  else
    echo true
  fi
}

# pr_trusted <base>: prints true, false or error for the whole queue group.
# <base> is the group's base commit, taken from the proof suite's branch
# (gh-readonly-queue/<default>/pr-N-<base>). Every commit in <base>..SHA (a
# merge group lands up to 3 entries) must map, by the last "(#N)" of its
# subject, to a trusted pull request: the proof ran the whole group, so a push
# range shorter than the group would hide an untrusted entry that merged in an
# earlier push. A queue check suite alone is not proof for a fork or Dependabot
# group, which ran on hosted runners. Anything unknown (BEFORE_SHA missing,
# all-zero, not an ancestor of SHA; <base> not an ancestor of SHA; an empty or
# unreadable range; a merge commit; more than 3 commits; a subject without a
# number; a failed lookup) is error, which runs the full tier.
pr_trusted() {
  local base="$1" shas sha subject n nums="" count=0 st result=true
  case "${BEFORE_SHA:-}" in
    ""|*[!0]*) ;;
    *) echo error; return ;;
  esac
  [ -n "${BEFORE_SHA:-}" ] || { echo error; return; }
  git rev-parse -q --verify "${BEFORE_SHA}^{commit}" >/dev/null 2>&1 || { echo error; return; }
  git merge-base --is-ancestor "${BEFORE_SHA}" "${SHA:-HEAD}" 2>/dev/null || { echo error; return; }
  git rev-parse -q --verify "${base}^{commit}" >/dev/null 2>&1 || { echo error; return; }
  git merge-base --is-ancestor "${base}" "${SHA:-HEAD}" 2>/dev/null || { echo error; return; }
  shas="$(git rev-list "${base}..${SHA:-HEAD}" 2>/dev/null)" || { echo error; return; }
  [ -n "${shas}" ] || { echo error; return; }
  [ -z "$(git rev-list --min-parents=2 "${base}..${SHA:-HEAD}" 2>/dev/null)" ] || { echo error; return; }
  for sha in ${shas}; do
    count=$((count + 1))
    if [ "${count}" -gt 3 ]; then
      echo error
      return
    fi
    subject="$(git log -1 --format=%s "${sha}" 2>/dev/null)" || { echo error; return; }
    n="$(printf '%s\n' "${subject}" | grep -oE '\(#[0-9]+\)' | tail -n 1 | tr -dc '0-9' || true)"
    [ -n "${n}" ] || { echo error; return; }
    nums="${nums} ${n}"
  done
  for n in $(printf '%s\n' ${nums} | sort -un); do
    st="$(pr_status "${n}")"
    case "${st}" in
      true) ;;
      false) result=false ;;
      *) echo error; return ;;
    esac
  done
  echo "${result}"
}

# proof_from_branches <"none" | space-separated suite branches>: prints true,
# false or error. The group's base commit is the last "-"-separated part of a
# branch gh-readonly-queue/<default>/pr-N-<base>; it must be a full commit id,
# and all suites must agree on it.
proof_from_branches() {
  local b base bases=""
  [ "$1" != none ] || { echo false; return; }
  for b in $1; do
    base="${b##*-}"
    case "${base}" in
      *[!0-9a-f]*) echo error; return ;;
    esac
    [ "${#base}" = 40 ] || { echo error; return; }
    case " ${bases} " in *" ${base} "*) ;; *) bases="${bases} ${base}" ;; esac
  done
  set -- ${bases}
  [ "$#" = 1 ] || { echo error; return; }
  pr_trusted "$1"
}

# queue_proof: prints true, false or error.
queue_proof() {
  local path="repos/${GITHUB_REPOSITORY:-}/commits/${SHA:-}/check-suites?app_id=15368&per_page=100"
  local tries=0 delay=2 body result
  if [ -z "${GITHUB_REPOSITORY:-}" ] || [ -z "${SHA:-}" ]; then
    echo error
    return
  fi
  while :; do
    tries=$((tries + 1))
    if body="$(gh api "${path}")" && [ -n "$(printf '%s' "${body}" | tr -d '[:space:]')" ] &&
      result="$(printf '%s' "${body}" | jq -r '
        [ (.check_suites? // [])[]?
          | select(((.head_branch // "") | startswith("gh-readonly-queue/"))
                   and .status == "completed" and .conclusion == "success")
          | .head_branch ] | unique | if length == 0 then "none" else join(" ") end' 2>/dev/null)" &&
      [ -n "${result}" ]; then
      proof_from_branches "${result}"
      return
    fi
    if [ "${tries}" -ge 3 ]; then
      echo error
      return
    fi
    sleep "${delay}"
    delay=$((delay * 2))
  done
}

if [ "${event}" = push ]; then
  if pipeline_change; then
    pc=true
    proof=false
  else
    pc=false
    proof="$(queue_proof)"
  fi
  would_skip=false
  if [ "${pc}" = false ] && [ "${proof}" = true ]; then
    would_skip=true
  fi
  if [ "${scope}" = full ]; then
    reason="scope full"
  elif [ "${pc}" = true ]; then
    reason="pipeline change"
  elif [ "${proof}" = true ]; then
    fleet=false
    heavy=false
    reason="queue proof"
  elif [ "${proof}" = error ]; then
    reason="queue proof unreadable"
  else
    reason="no queue proof"
  fi
  log "shadow: would_skip=${would_skip} proof=${proof} pipeline_change=${pc}"
fi

log "fleet scope: event=${event:-none} scope=${scope} fleet=${fleet} heavy=${heavy} reason=${reason}"
printf 'fleet=%s\nheavy=%s\n' "${fleet}" "${heavy}"
