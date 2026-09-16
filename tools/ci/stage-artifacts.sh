#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Stage pre-built CI artifacts for Dockerfile.prebuilt consumption.
#
# Usage: tools/ci/stage-artifacts.sh <source-dir>
#
# Copies factory_worker, ngx_pagespeed_module.so, the brotli module, and
# libc++ runtime libs from <source-dir>/.ci-artifacts/ to .ci-artifacts/ in
# the current directory (which should be the repo checkout root).
set -euo pipefail

SRC="${1:?Usage: stage-artifacts.sh <source-dir>}"

# Fail fast with a clear error if Docker isn't healthy — we'll need it
# below to build pagespeed2-base-runtime, and the cascade error from a
# raw `docker build` is much harder to diagnose.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}/docker-preflight.sh"

# Invalidate a STALE runner-local bundle before the existence check below.
# Persistent CI runners keep ${SRC}/.ci-artifacts/ across jobs, so a
# prior (different-SHA) Build's factory_worker can satisfy the existence check
# and be used silently — an HTTP-Compliance 404 was once traced to a stale
# ngx_pagespeed_module.so from a pre-merge build sitting on the runner. The
# Linux Build job's "Upload CI artifacts" step records the producing commit in
# metadata.json; if it disagrees with this run's GITHUB_SHA, wipe the staging
# dir so the SHA-keyed the CI hub fetch below pulls the correct bundle. No-op when
# metadata.json is absent (old/foreign bundle) — same behaviour as before.
# Mirrors the same stale-bundle guard the packaging/native-asset consumers
# already apply (ci.yml "Download CI artifacts for packaging"). Skip when
# GITHUB_SHA is unset (local/dev invocation) so we never wipe based on an
# empty expected SHA.
if [[ "${GITHUB_SHA:-}" =~ ^[0-9a-f]{40}$ ]]; then
  "${SCRIPT_DIR}/invalidate-stale-artifacts.sh" "${SRC}/.ci-artifacts" "${GITHUB_SHA}"
fi

# If artifacts don't exist locally, try fetching from the CI hub via rsync.
# Retry with backoff to absorb transient network/SSH blips, but
# distinguish "upstream never produced artifacts" (rsync exit 23/24,
# directory missing) from "transient failure" — the former is a hard
# failure that won't be fixed by retrying.
if [ ! -f "${SRC}/.ci-artifacts/factory_worker" ]; then
  echo "::warning::Local CI artifacts not found in ${SRC}/.ci-artifacts/ — fetching from the CI hub"
  mkdir -p "${SRC}/.ci-artifacts"

  # Defensive: GITHUB_SHA is normally a 40-char hex SHA injected by GHA, but
  # validate before splicing it into a remote rsync path so an unexpected
  # value can't traverse outside the artifacts tree.
  if ! [[ "${GITHUB_SHA:-}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "::error::GITHUB_SHA is missing or not a 40-char hex SHA (got: '${GITHUB_SHA:-<unset>}'); refusing to construct rsync path." >&2
    exit 1
  fi
  # CI_ARTIFACTS_PLATFORM selects the shared-dir variant to fetch. Default
  # linux-x64 is ci.yml's push bundle (--config=ci, with nginx-version +
  # brotli); ci-periodic's x64 test jobs set linux-x64-periodic to fetch the
  # --config=opt bundle their Build (x64) publishes, so the two producers
  # never read each other's binaries for the same SHA.
  # CI_HUB_SSH / CI_HUB_ARTIFACTS_ROOT come from GitHub repository variables
  # (wired into every calling workflow's top-level env:); nothing
  # environment-specific is hardcoded here.
  : "${CI_HUB_SSH:?CI_HUB_SSH must be set (GitHub repository variable)}"
  : "${CI_HUB_ARTIFACTS_ROOT:?CI_HUB_ARTIFACTS_ROOT must be set (GitHub repository variable)}"
  RSYNC_REMOTE="${CI_HUB_SSH}:${CI_HUB_ARTIFACTS_ROOT}/${GITHUB_SHA}/${CI_ARTIFACTS_PLATFORM:-linux-x64}/"
  RSYNC_SSH='ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=10'

  attempt=1
  max_attempts=3
  delay=5
  while :; do
    rc=0
    rsync -az -e "${RSYNC_SSH}" "${RSYNC_REMOTE}" "${SRC}/.ci-artifacts/" || rc=$?
    if [ "$rc" -eq 0 ]; then
      break
    fi
    # Exit-code classification policy:
    #   23/24 (partial transfer, usually missing source path) → TERMINAL.
    #     The upstream Linux Build job didn't publish artifacts; no amount
    #     of retry will conjure them.
    #   anything else non-zero → TRANSIENT by default (SSH/network blips,
    #     transient peer errors). This is conservative — codes like 2/5/10/12
    #     are arguably terminal in theory, but in practice we've only seen
    #     them under transient network/SSH conditions, so retry is safe and
    #     retrying-then-failing is cheap. Do NOT widen the terminal set
    #     without evidence of a class of failure that retry masks.
    if [ "$rc" -eq 23 ] || [ "$rc" -eq 24 ]; then
      echo "::error::No artifacts at ${RSYNC_REMOTE} (rsync exit ${rc})." >&2
      echo "::error::The upstream Linux Build job for SHA ${GITHUB_SHA} did not publish artifacts — check that job's log first; this rsync failure is a cascade." >&2
      exit 1
    fi
    if [ "$attempt" -ge "$max_attempts" ]; then
      echo "::error::rsync from the CI hub failed ${max_attempts} times (last exit ${rc}). Giving up." >&2
      exit 1
    fi
    echo "::warning::rsync attempt ${attempt}/${max_attempts} failed (exit ${rc}); retrying in ${delay}s..." >&2
    sleep "$delay"
    attempt=$((attempt + 1))
    delay=$((delay * 2))
  done

  if [ ! -f "${SRC}/.ci-artifacts/factory_worker" ]; then
    echo "::error::rsync reported success but factory_worker is still missing under ${SRC}/.ci-artifacts/." >&2
    exit 1
  fi
fi

# Ensure pagespeed2-base-runtime exists AND matches the current Dockerfile.
# Persistent CI runners keep the image across jobs, so a plain
# "rebuild if missing" check (the previous behaviour) silently reuses a
# stale image when Dockerfile.base-runtime changes — invisibly breaking
# downstream consumers (e.g. nginx version drift between builder and
# runtime → "module … version X instead of Y", HTTP Compliance red).
# Stamp the image with the SHA-256 of the Dockerfile and rebuild on drift.
EXPECTED_HASH=$(sha256sum docker/Dockerfile.base-runtime | awk '{print $1}')
ACTUAL_HASH=$(docker image inspect pagespeed2-base-runtime \
  --format '{{ index .Config.Labels "weamp.base-runtime.dockerfile_sha256" }}' 2>/dev/null || true)
if [ "$ACTUAL_HASH" != "$EXPECTED_HASH" ]; then
  echo "Rebuilding pagespeed2-base-runtime (Dockerfile sha mismatch: have='${ACTUAL_HASH}' want='${EXPECTED_HASH}')"
  # APT_REFRESH busts the apt dist-upgrade layer so a warm cache can't re-freeze
  # OS-package CVEs (the image CVE gate); pass today's UTC date fresh each run.
  docker build -t pagespeed2-base-runtime \
    --build-arg APT_REFRESH="$(date -u +%F)" \
    --label "weamp.base-runtime.dockerfile_sha256=${EXPECTED_HASH}" \
    -f docker/Dockerfile.base-runtime .
fi

mkdir -p .ci-artifacts
install -m 755 "${SRC}/.ci-artifacts/factory_worker" .ci-artifacts/
install -m 644 "${SRC}/.ci-artifacts/ngx_pagespeed_module.so" .ci-artifacts/
# libc++/libc++abi/libunwind statically linked into factory_worker since v2.0.2.

# nginx-version: the exact nginx version the prebuilt module was built against,
# recorded by the Linux Build job. Dockerfile.prebuilt pins its runtime nginx +
# brotli to this so the module loads (base-runtime tracks nginx.org's current
# release and can drift a point release ahead of a cached module).
# Back-compat: artifacts predating this recording lack the file — fall back to
# base-runtime's own nginx version so the pin becomes a harmless no-op rather
# than failing the COPY (such old artifacts behave exactly as before; only
# fresh artifacts get the real drift fix).
if [ -f "${SRC}/.ci-artifacts/nginx-version" ]; then
  install -m 644 "${SRC}/.ci-artifacts/nginx-version" .ci-artifacts/
else
  echo "::warning::nginx-version absent from ${SRC}/.ci-artifacts/ — deriving from base-runtime (no-op pin; old artifact)."
  docker run --rm pagespeed2-base-runtime sh -c "nginx -v 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1" \
    > .ci-artifacts/nginx-version
fi
[ -s .ci-artifacts/nginx-version ] || { echo "::error::Could not determine nginx-version for prebuilt image." >&2; exit 1; }
echo "Prebuilt image will pin nginx to: $(cat .ci-artifacts/nginx-version)"

# Brotli module: Dockerfile.prebuilt COPYs a pre-built
# ngx_http_brotli_filter_module.so instead of recompiling it from source on
# every (cold-cache) image build. The x64 Linux Build job builds it on a
# dedicated build runner (warm Docker layer cache) and ships it in the
# artifact bundle, so the CI test runners never recompile brotli.
# Producers that don't ship it (arm64 ci-periodic; older artifact bundles) fall
# back to building it here — keyed by nginx-version, so it's a Docker
# layer-cache no-op on a warm host. Build needs .ci-artifacts/nginx-version,
# established just above.
if [ -f "${SRC}/.ci-artifacts/ngx_http_brotli_filter_module.so" ]; then
  install -m 644 "${SRC}/.ci-artifacts/ngx_http_brotli_filter_module.so" .ci-artifacts/
  echo "Staged pre-built brotli module from ${SRC}."
else
  echo "::warning::brotli module absent from ${SRC}/.ci-artifacts/ — building it (no prebuilt artifact: arm64 periodic or old bundle)."
  "${SCRIPT_DIR}/build-brotli-module.sh" "$(pwd)"
fi

echo "Staged CI artifacts from ${SRC}:"
ls -lh .ci-artifacts/
