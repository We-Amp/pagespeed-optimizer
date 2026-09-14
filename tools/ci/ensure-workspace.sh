#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# ensure-workspace.sh - Make the box-local CI workspace current, self-healing
# when it holds another run's tree (or none at all).
#
# Why this exists:
#   Build jobs extract the run's vendor tarball into a fixed box-local
#   workspace dir OUTSIDE $GITHUB_WORKSPACE (WORKSPACE_DIR /
#   MAC_WORKSPACE_DIR / ARM64_WORKSPACE_DIR, plus the *-release variants);
#   downstream test jobs reuse that dir instead of re-extracting. Two ways
#   that reuse goes stale:
#     1. FIFO interleave ACROSS runs: a single-runner lane serializes jobs
#        first-in-first-out across workflow runs, so another run's build job
#        can execute between THIS run's build and its downstream test jobs,
#        leaving the workspace at the wrong SHA. The old existence-only
#        guards ([ ! -f GIT_COMMIT ]) pass in that state, so the test job
#        silently tests a DIFFERENT run's tree -- failing on that run's bug
#        while masking it on the run's own PR (same defect class fixed in
#        the sibling repository).
#     2. Multi-box labels: `macos-native` is held by TWO Mac boxes and the
#        the dedicated x64 set by two machines, so a consumer job
#        can be scheduled on a different box than its builder -- where the
#        workspace was never populated for this run at all.
#   Either way the fix is the same: verify the GIT_COMMIT stamp CONTENT
#   against $GITHUB_SHA and re-extract this run's own tarball on mismatch.
#
# Behaviour:
#   - Happy path (stamp == $GITHUB_SHA): no-op; the warm tree and box-local
#     bazel disk cache are reused.
#   - Stale/missing: log what was found, clear the workspace (via a throwaway
#     container when docker is available, so root-owned files left by Docker
#     builds don't survive; plain rm on the native mac lanes), then fetch +
#     extract the run's tarball via extract-vendor-tarball.sh
#     (integrity-checked, retried, with a self-heal) and
#     re-verify the stamp. The only cost on the rare heal is a colder cache.
#
# Usage:
#   ensure-workspace.sh <short_sha> [<local_tarball>]
#
#   <local_tarball>: optional box-local tarball path handed to
#   extract-vendor-tarball.sh --local (the lane's known-local copy, e.g. the
#   vendor job's output on the x64 lane or the shared copy on the
#   Mac-hosted lanes). When it is absent/corrupt on THIS box -- the
#   cross-box case -- the extractor falls back to fetching from the
#   authoritative shared dir, so the self-heal works on whichever box
#   the job landed on.
#
# Requires (set by the workflow env): WORKSPACE_DIR, GITHUB_SHA, GITHUB_WORKSPACE.
set -euo pipefail

SHORT_SHA="${1:-}"
[ -n "$SHORT_SHA" ] || { echo "::error::ensure-workspace.sh: <short_sha> argument required" >&2; exit 2; }
LOCAL_TARBALL="${2:-}"
: "${WORKSPACE_DIR:?ensure-workspace.sh: WORKSPACE_DIR must be set}"
: "${GITHUB_SHA:?ensure-workspace.sh: GITHUB_SHA must be set}"
: "${GITHUB_WORKSPACE:?ensure-workspace.sh: GITHUB_WORKSPACE must be set}"

STAMPED="$(tr -d '[:space:]' < "${WORKSPACE_DIR}/GIT_COMMIT" 2>/dev/null || true)"
if [ "$STAMPED" = "$GITHUB_SHA" ]; then
  echo "ensure-workspace: workspace current on $(hostname) (${GITHUB_SHA}) -- reusing warm tree"
  exit 0
fi

echo "ensure-workspace: workspace missing/stale on $(hostname) (have '${STAMPED}', want '${GITHUB_SHA}') -- self-healing"

# Ensure the workspace dir exists owned by THIS runner uid before we touch it.
# On a box where no build job has run yet, ${WORKSPACE_DIR} may not exist at
# all; a docker bind-mount would then create it ROOT-owned and the
# unprivileged extract below would fail "Permission denied".
mkdir -p "${WORKSPACE_DIR}" 2>/dev/null || true

# Clear any stale tree first. On the Docker lanes a previous run may have
# left root-owned files (Docker writes as root) that the unprivileged runner
# can't rm directly -- nuke them via a throwaway container, also normalising
# ownership of the dir itself back to this runner. The native mac lanes have
# no root-owned files (and may lack docker), so the plain rm suffices there.
if command -v docker >/dev/null 2>&1; then
  docker run --rm -v "${WORKSPACE_DIR}":/workspace alpine sh -c \
    "rm -rf /workspace/* /workspace/.[!.]* 2>/dev/null || true; chown $(id -u):$(id -g) /workspace" \
    2>/dev/null || true
fi
# The tarball payload carries bazel's read-only (0555) dir modes, and rm
# needs write on the parent dir — without this chmod the guarded rm below
# silently leaves residue on the native mac lanes (no docker to escalate).
find "${WORKSPACE_DIR}" -type d -exec chmod u+rwx {} + 2>/dev/null || true
rm -rf "${WORKSPACE_DIR:?}"/* "${WORKSPACE_DIR:?}"/.[!.]* 2>/dev/null || true

# Fetch + extract this run's tarball.
#   --stream : extract via `zstd -d | tar` rather than `tar --zstd -xf`, so
#              this works regardless of whether the runner's tar was built
#              with zstd support (macOS bsdtar lacks --zstd; the self-heal
#              can run on any box in the label set).
#   --local  : only when the caller passed a box-local tarball path; when it
#              is missing/corrupt on this box, extract-vendor-tarball.sh
#              falls back to the authoritative shared dir.
EXTRACT_ARGS=(--sha "${SHORT_SHA}" --dest "${WORKSPACE_DIR}" --prefix modpagespeed-2 --stream)
if [ -n "$LOCAL_TARBALL" ]; then
  EXTRACT_ARGS+=(--local "$LOCAL_TARBALL")
fi
bash "${GITHUB_WORKSPACE}/_ci-tools/tools/ci/extract-vendor-tarball.sh" "${EXTRACT_ARGS[@]}"

STAMPED2="$(tr -d '[:space:]' < "${WORKSPACE_DIR}/GIT_COMMIT")"
[ "$STAMPED2" = "$GITHUB_SHA" ] || {
  echo "::error::ensure-workspace: self-heal extract produced wrong stamp ('${STAMPED2}', want '${GITHUB_SHA}')" >&2
  exit 1
}
echo "ensure-workspace: workspace self-healed and verified (${GITHUB_SHA})"
