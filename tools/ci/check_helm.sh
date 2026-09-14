#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI check: Validate the Helm chart.
# Requires: helm CLI installed.

set -euo pipefail

CHART_DIR="deploy/helm/pagespeed"

if [ ! -d "$CHART_DIR" ]; then
  echo "ERROR: $CHART_DIR not found"
  exit 1
fi

if ! command -v helm &>/dev/null; then
  echo "SKIP: helm not installed"
  exit 0
fi

echo "=== Helm lint ==="
helm lint "$CHART_DIR"

echo ""
echo "=== Helm template (default values) ==="
helm template pagespeed-test "$CHART_DIR" > /dev/null
echo "OK: default values template renders"

echo ""
echo "=== Helm template (custom values) ==="
helm template pagespeed-test "$CHART_DIR" \
  --set worker.cacheSize=1073741824 \
  --set ingress.enabled=true \
  --set ingress.hosts[0].host=example.com \
  --set ingress.hosts[0].paths[0].path=/ \
  --set ingress.hosts[0].paths[0].pathType=Prefix \
  > /dev/null
echo "OK: custom values template renders"

echo ""
echo "=== Helm lockstep: worker and nginx share ONE image tag ==="
rendered=$(helm template pagespeed-test "$CHART_DIR")
worker_tag=$(echo "$rendered" | grep 'pagespeed-worker:' | head -1 | sed -E 's/.*pagespeed-worker:([^"[:space:]]+).*/\1/')
nginx_tag=$(echo "$rendered" | grep 'pagespeed-nginx:' | head -1 | sed -E 's/.*pagespeed-nginx:([^"[:space:]]+).*/\1/')
app_version=$(grep -E '^appVersion:' "$CHART_DIR/Chart.yaml" | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
echo "  worker=$worker_tag nginx=$nginx_tag appVersion=$app_version"
if [ -z "$worker_tag" ] || [ "$worker_tag" != "$nginx_tag" ]; then
  echo "ERROR: worker ($worker_tag) and nginx ($nginx_tag) image tags differ — lockstep broken"
  exit 1
fi
if [ "$worker_tag" != "$app_version" ]; then
  echo "ERROR: default image tag ($worker_tag) does not track appVersion ($app_version)"
  exit 1
fi
echo "OK: both images pinned to $worker_tag (= appVersion)"

echo ""
echo "=== Helm guard: legacy per-image tag is rejected (no silent split) ==="
if helm template pagespeed-test "$CHART_DIR" --set worker.image.tag=9.9.9 >/dev/null 2>&1; then
  echo "ERROR: --set worker.image.tag should FAIL (it would allow a worker/nginx split) but rendered"
  exit 1
fi
echo "OK: worker.image.tag override is rejected loud"

echo ""
echo "All Helm checks passed."
