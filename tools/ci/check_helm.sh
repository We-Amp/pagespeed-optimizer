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

# The Compose recipe pins the same images as the chart. Its default tag must
# be the chart's appVersion: the worker and nginx images publish version tags
# only, so a default of `latest` (or a stale version) fails to pull. Needs no
# helm, so it runs before the helm check below.
# appVersion may be bare, single-quoted or double-quoted (all valid YAML).
app_version=$(grep -E '^appVersion:' "$CHART_DIR/Chart.yaml" | head -1 \
  | sed -E "s/^appVersion:[[:space:]]*//; s/[[:space:]]+#.*$//; s/[[:space:]]+$//; s/^\"(.*)\"$/\1/; s/^'(.*)'$/\1/")
if [ -z "$app_version" ]; then
  echo "ERROR: cannot read appVersion from $CHART_DIR/Chart.yaml"
  exit 1
fi

echo "=== Compose default image tag tracks appVersion ==="
compose_tags=$(grep -Eo 'pagespeed-(worker|nginx):\$\{PAGESPEED_VERSION:-[^}]*\}' deploy/docker-compose.yml \
  | sed -E 's/.*:-([^}]*)\}/\1/' | sort -u)
env_tag=$(grep -E '^#?[[:space:]]*PAGESPEED_VERSION=' deploy/.env.example | head -1 | sed -E 's/.*=//')
echo "  appVersion=$app_version compose=$(echo $compose_tags) .env.example=$env_tag"
if [ "$(grep -c '^[[:space:]]*image: ghcr.io/we-amp/pagespeed-\(worker\|nginx\):' deploy/docker-compose.yml)" != 2 ] \
  || [ "$compose_tags" != "$app_version" ] || [ "$env_tag" != "$app_version" ]; then
  echo "ERROR: deploy/docker-compose.yml and deploy/.env.example must default both images to appVersion ($app_version)"
  exit 1
fi
echo "OK: Compose defaults to $app_version (= appVersion)"
echo ""

if ! command -v helm &>/dev/null; then
  # Locally a missing helm is a skip; in CI (CHECK_HELM_REQUIRED=1) it is a
  # failure, so the gate cannot pass without having run.
  if [ "${CHECK_HELM_REQUIRED:-0}" = "1" ]; then
    echo "ERROR: helm not installed and CHECK_HELM_REQUIRED=1"
    exit 1
  fi
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
echo "=== Helm guard: the pod shares one PID namespace ==="
# The pod keeps every cache writer in one PID namespace (as the compose files
# do) with the pause container as PID 1; see the comment in deployment.yaml.
if ! helm template pagespeed-test "$CHART_DIR" | grep -qE '^ +shareProcessNamespace: true$'; then
  echo "ERROR: the rendered pod does not set shareProcessNamespace: true"
  exit 1
fi
echo "OK: shareProcessNamespace: true"

echo ""
echo "All Helm checks passed."
