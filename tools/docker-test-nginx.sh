#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run test-nginx integration tests inside the pagespeed2-dev Docker container.
# Installs nginx + Test::Nginx, builds the module, and runs prove.
#
# Usage (from host):
#   docker run --rm \
#     -v "$PWD:/workspace" -v "pagespeed2-bazel-cache:/root/.cache/bazel" \
#     -w /workspace pagespeed2-dev bash tools/docker-test-nginx.sh
# To run a single test file, pass it as an argument:
#   ... pagespeed2-dev bash tools/docker-test-nginx.sh t/200-miss-path.t
set -euxo pipefail

echo "=== Step 1: Installing nginx + Test::Nginx ==="
apt-get update -qq
apt-get install -y -qq nginx perl cpanminus libipc-run-perl libpcre3-dev zlib1g-dev 2>&1 | tail -3
cpanm --notest Test::Nginx 2>&1 | tail -3
perl -MTest::Nginx::Socket -e 'print "Test::Nginx: OK\n"'

echo "=== Step 2: Downloading matching nginx source ==="
NGINX_VERSION=$(nginx -v 2>&1 | grep -oP 'nginx/\K[0-9]+\.[0-9]+\.[0-9]+')
echo "nginx version: $NGINX_VERSION"
curl -L "https://nginx.org/download/nginx-$NGINX_VERSION.tar.gz" -o /tmp/nginx.tar.gz
cd /tmp && tar xzf nginx.tar.gz
cd "/tmp/nginx-$NGINX_VERSION"
./configure --with-compat 2>&1 | tail -3
echo "nginx source configured"

echo "=== Step 3: Building module ==="
cd /workspace
NGINX_PATH="/tmp/nginx-$NGINX_VERSION" bazel build --config=docker --symlink_prefix=/dev/null/ //src/nginx:ngx_pagespeed_module.so 2>&1 | tail -5

BAZEL_EXECROOT="$(bazel info --config=docker execution_root 2>/dev/null)"
MODULE_SO="$BAZEL_EXECROOT/$(bazel cquery --config=docker --output=files //src/nginx:ngx_pagespeed_module.so 2>/dev/null)"
echo "Module at: $MODULE_SO"

echo "=== Step 4: Building seed_test_cache + webbotauth_sign_tool ==="
bazel build --config=docker --symlink_prefix=/dev/null/ //tools:seed_test_cache //src/crypto/webbotauth:webbotauth_sign_tool 2>&1 | tail -3
SEED_TOOL="$BAZEL_EXECROOT/$(bazel cquery --config=docker --output=files //tools:seed_test_cache 2>/dev/null)"
echo "Seed tool at: $SEED_TOOL"
# t/103-webbotauth-valid.t mints fresh RFC 9421 signatures with this tool
# (the suite self-skips when the variable is unset).
WEBBOTAUTH_SIGN_TOOL="$BAZEL_EXECROOT/$(bazel cquery --config=docker --output=files //src/crypto/webbotauth:webbotauth_sign_tool 2>/dev/null)"
echo "Sign tool at: $WEBBOTAUTH_SIGN_TOOL"

echo "=== Step 5: ABI check ==="
echo "load_module $MODULE_SO;
events { worker_connections 64; }
http { server { listen 19840; } }" > /tmp/abi-test.conf
nginx -t -c /tmp/abi-test.conf 2>&1
echo "ABI check passed"

echo "=== Step 6: Seeding cache for HIT-path tests ==="
rm -f /tmp/test-ps-*.vol /tmp/test-ps-*.vol.gen /tmp/test-ps-*.sock
export SEED_TEST_CACHE="$SEED_TOOL"
for seed_script in /workspace/t/seed-*.sh; do
  [ -f "$seed_script" ] && bash "$seed_script"
done

echo "=== Step 7: Running test-nginx tests ==="
export TEST_NGINX_LOAD_MODULES="$MODULE_SO"
export TEST_NGINX_LOG_LEVEL=warn
export TEST_NGINX_SERVER_PORT=19840
export WEBBOTAUTH_SIGN_TOOL
# PROVE_OPTS is prove options only; test files are positional arguments.
# PROVE_ARGS (older name) holds the whole prove invocation.
if [ -z "${PROVE_OPTS:-}" ] && [ -n "${PROVE_ARGS:-}" ]; then
  # shellcheck disable=SC2086
  prove $PROVE_ARGS
else
  if [ $# -eq 0 ]; then
    # No test files given: run the full suite.
    PROVE_OPTS="${PROVE_OPTS:--v -r}"
    set -- t/
  fi
  prove ${PROVE_OPTS:--v} "$@"
fi
