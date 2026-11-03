#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed the cache for t/107-host-vouch.t: a stored /llms.txt index under each
# hostname the tests ask for, listed by a server name or not.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SEED="${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}"

if [ ! -x "$SEED" ]; then
  echo "Error: seed_test_cache not found at $SEED"
  echo "  Local: bazel build //tools:seed_test_cache"
  echo "  Docker: set SEED_TEST_CACHE env var"
  exit 1
fi

dir=/tmp/psvouch-107
vol="${dir}/cache.vol"
mkdir -p "$dir"
rm -f "${vol}"* "${vol%.vol}"-*.vol

# The test server's own port (Test::Nginx default), and another one.
port="${TEST_NGINX_SERVER_PORT:-1984}"
other_port=65000
[ "$port" = "$other_port" ] && other_port=65001

# 0x8C is the /llms.txt index sentinel.
for host in site.test a.wild.test r7.regex.test canon.test unlisted.test \
  10.0.0.9 "site.test:${port}" "site.test:${other_port}"; do
  "$SEED" \
    --cache "$vol" \
    --url "/llms.txt" \
    --host "$host" \
    --scheme http \
    --content "# index for ${host}" \
    --content_type other \
    --mask 0x8C
done
