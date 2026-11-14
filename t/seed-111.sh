#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/111-cookie-request.t: stored pages and Markdown copies with
# and without a sharing mark (public / s-maxage) in their Cache-Control.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SEED="${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}"

if [ ! -x "$SEED" ]; then
  echo "Error: seed_test_cache not found at $SEED"
  echo "  Local: bazel build //tools:seed_test_cache"
  echo "  Docker: set SEED_TEST_CACHE env var"
  exit 1
fi

dir=/tmp/pscookie-111
vol="${dir}/cache.vol"
mkdir -p "$dir"
rm -f "${vol}"* "${vol%.vol}"-*.vol

page() {  # page URL CACHE_CONTROL BODY
  "$SEED" --cache "$vol" --url "$1" --host localhost --scheme http \
    --content_type html --mask 0x08 --origin_cc "$2" --content "$3"
}

page /plain "max-age=600" "<html><body>stored plain</body></html>"
page /public "public, max-age=600" "<html><body>stored public</body></html>"
page /smaxage "max-age=600, s-maxage=600" \
  "<html><body>stored smaxage</body></html>"
page /mustrev "max-age=600, must-revalidate" \
  "<html><body>stored mustrev</body></html>"

# A Markdown copy is served only while its recorded hash matches the URL's
# live content hash, so each copy gets both.
hash=$(printf 'ab%.0s' $(seq 32))
markdown() {  # markdown URL PARENT_CACHE_CONTROL
  page "$1" "$2" "<html><body>stored parent</body></html>"
  "$SEED" --cache "$vol" --url "$1" --host localhost --scheme http \
    --mask 0x3C --content_hex "$hash"
  "$SEED" --cache "$vol" --url "$1" --host localhost --scheme http \
    --mask 0x7C --content_type other --origin_ct text/markdown \
    --origin_html_hash "$hash" --content "# markdown $1"
}

markdown /md-plain "max-age=600"
markdown /md-public "public, max-age=600"
