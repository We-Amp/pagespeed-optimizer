#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/300-hit-path.t tests.
# Called by tools/run-test-nginx.sh or the test's BEGIN block.
# Supports both local (bazel-bin/) and Docker (SEED_TEST_CACHE env var).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Allow override via environment (used by docker-test-nginx.sh)
SEED="${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}"

if [ ! -x "$SEED" ]; then
  echo "Error: seed_test_cache not found at $SEED"
  echo "  Local: bazel build //tools:seed_test_cache"
  echo "  Docker: set SEED_TEST_CACHE env var"
  exit 1
fi

# CSS alternate at default mask (Desktop/Identity = 0x08).  --scheme http:
# the cache key is scheme://host/url and Test::Nginx issues plain-http
# requests — an https-seeded key is a permanent MISS.
"$SEED" \
  --cache /tmp/test-ps-300-1.vol \
  --url "/style.css" \
  --host "localhost" \
  --scheme http \
  --content "h1{color:red}" \
  --content_type css \
  --mask 0x08
