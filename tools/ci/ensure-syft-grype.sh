#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Install pinned syft + grype for the dependency-scan jobs, resiliently.
#
# Usage:
#   SYFT_VERSION=v1.44.0 GRYPE_VERSION=v0.112.0 tools/ci/ensure-syft-grype.sh
#
# Both versions are REQUIRED (passed via the calling workflow's env — the
# per-PR blocking dependency scan and the scheduled image scan). This script
# does not pick versions; it only installs the ones it's told to, so the pins
# stay with the callers.
#
# Why this exists
# ---------------
# The dependency-scan jobs used to inline `curl … install.sh | sh` for syft and
# grype. The installer curls github.com release assets at job time, which
# intermittently fails — one run hit `received HTTP status=000`
# (a transient network/rate-limit blip) and then `unable to find tag=''`,
# reddening the whole "Blocking Dep Scan (npm + cargo)" gate over a single
# flaky fetch.
#
# Two layers of resilience, smallest sound change:
#
#   1. Runner-local persistent cache. These are persistent CI runners,
#      so we cache the pinned binaries in a stable per-runner dir
#      (CI_BIN_CACHE, default ~/.weamp/ci-bin) keyed by tool+version. Once a
#      version is installed there, every later run links it into the job-local
#      bin without touching the network at all. github.com is hit ONCE per new
#      pin, not once per run.
#
#   2. Retry with backoff. When a cache miss does force a download, the
#      `curl … | sh` installer is wrapped in a retry loop (default 4 attempts,
#      exponential backoff) mirroring the house idiom in
#      tools/ci/stage-artifacts.sh and tools/ci/extract-vendor-tarball.sh, so a
#      single transient blip no longer fails the job.
#
# The job-local bin (RUNNER_TEMP/bin, or BIN_DIR override) is what ends up on
# PATH for the rest of the job; the cache is an install accelerator behind it.
set -euo pipefail

SYFT_VERSION="${SYFT_VERSION:?SYFT_VERSION is required (e.g. v1.44.0)}"
GRYPE_VERSION="${GRYPE_VERSION:?GRYPE_VERSION is required (e.g. v0.112.0)}"

# Job-local bin that goes on PATH for the rest of the job. Defaults to the
# GitHub-provided per-job RUNNER_TEMP; overridable for local testing.
BIN_DIR="${BIN_DIR:-${RUNNER_TEMP:?RUNNER_TEMP is required when BIN_DIR is unset}/bin}"

# Persistent per-runner cache. Survives across jobs/runs on persistent CI
# runners; the ~/.weamp dir is the established machine-local state location.
CI_BIN_CACHE="${CI_BIN_CACHE:-${HOME}/.weamp/ci-bin}"

# Retry knobs (overridable for tests / tuning), matching the house idiom.
DL_RETRIES="${DL_RETRIES:-4}"
DL_BACKOFF="${DL_BACKOFF:-5}"   # initial seconds, doubled each retry

mkdir -p "$BIN_DIR" "$CI_BIN_CACHE"

# Put the job-local bin on PATH for subsequent steps (no-op outside GHA).
if [ -n "${GITHUB_PATH:-}" ]; then
  echo "$BIN_DIR" >> "$GITHUB_PATH"
fi
export PATH="$BIN_DIR:$PATH"

# Report the installed version of a tool on PATH as `vX.Y.Z`, or empty if absent.
have() {
  command -v "$1" >/dev/null 2>&1 && "$1" version 2>/dev/null | awk '/^Version:/{print "v"$2}'
}

# Download + install one tool at a pinned version into BIN_DIR, retrying the
# installer on transient failure. The anchore installers exit non-zero on a
# failed curl (status=000 etc.); any non-zero is treated as transient here —
# the pin is known-good, so a failure is overwhelmingly network, not a real
# "no such release". Retrying-then-failing is cheap.
install_tool() {
  local tool="$1" version="$2"
  local url="https://raw.githubusercontent.com/anchore/${tool}/${version}/install.sh"
  local attempt backoff rc
  backoff="$DL_BACKOFF"
  for attempt in $(seq 1 "$DL_RETRIES"); do
    rc=0
    # Installer fetched from the PINNED release tag (not main), then run pinned
    # to the same version. `set -o pipefail` (inherited) makes a failed curl
    # propagate through the pipe.
    curl -sSfL "$url" | sh -s -- -b "$BIN_DIR" "$version" || rc=$?
    if [ "$rc" -eq 0 ] && [ "$(have "$tool")" = "$version" ]; then
      return 0
    fi
    if [ "$attempt" -lt "$DL_RETRIES" ]; then
      echo "::warning::${tool} ${version} install attempt ${attempt}/${DL_RETRIES} failed (rc=${rc}); retrying in ${backoff}s (transient anchore/github.com fetch — e.g. HTTP status=000)..." >&2
      sleep "$backoff"
      backoff=$((backoff * 2))
    fi
  done
  echo "::error::Failed to install ${tool} ${version} after ${DL_RETRIES} attempts (last rc=${rc}). github.com release fetch likely down/rate-limited; re-run the job." >&2
  return 1
}

# Ensure one pinned tool is present in BIN_DIR, preferring the runner-local
# cache and only hitting the network on a cache miss.
ensure_tool() {
  local tool="$1" version="$2"
  local cached="${CI_BIN_CACHE}/${tool}-${version}"

  # Already the right version on PATH (e.g. pre-installed on the runner)? Done.
  if [ "$(have "$tool")" = "$version" ]; then
    echo "${tool} ${version} already on PATH — no install needed."
    return 0
  fi

  # Cache hit: link the pinned binary into the job-local bin. No network.
  if [ -x "$cached" ]; then
    ln -sf "$cached" "${BIN_DIR}/${tool}"
    if [ "$(have "$tool")" = "$version" ]; then
      echo "${tool} ${version} restored from runner-local cache (${cached}) — no network."
      return 0
    fi
    echo "::warning::cached ${tool} at ${cached} did not report ${version}; re-downloading." >&2
    rm -f "$cached" "${BIN_DIR}/${tool}"
  fi

  # Cache miss: download (with retry) into BIN_DIR, then populate the cache.
  install_tool "$tool" "$version"
  cp -f "${BIN_DIR}/${tool}" "$cached"
  echo "${tool} ${version} installed and cached at ${cached}."
}

ensure_tool syft  "$SYFT_VERSION"
ensure_tool grype "$GRYPE_VERSION"

syft version  | awk '/^Version:/{print "syft "$2}'
grype version | awk '/^Version:/{print "grype "$2}'
