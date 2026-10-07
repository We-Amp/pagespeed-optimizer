#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Packages the `pagespeed` Helm chart and (re)generates the classic Helm-repo
# index that is published as a static asset under the public website and served
# at https://modpagespeed.com/charts/.
#
# Run this whenever the chart changes (template/values edits or a Chart.yaml
# version bump), then commit the regenerated files under
# website/public/charts/. Customers consume the result with:
#
#   helm repo add weamp https://modpagespeed.com/charts
#   helm install pagespeed weamp/pagespeed ...
#
# Usage: deploy/helm/package-chart.sh   (set HELM=/path/to/helm to override)
set -euo pipefail

REPO_URL="https://modpagespeed.com/charts"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHART_DIR="$HERE/pagespeed"
OUT_DIR="$HERE/../../website/public/charts"
HELM="${HELM:-helm}"

mkdir -p "$OUT_DIR"
"$HELM" package "$CHART_DIR" --destination "$OUT_DIR"
# Regenerate the index over every packaged version present in the directory.
# --merge keeps the original `created` timestamps of previously published
# entries instead of re-stamping them on every repackage.
"$HELM" repo index "$OUT_DIR" --url "$REPO_URL" --merge "$OUT_DIR/index.yaml"

echo "Packaged chart + index.yaml -> $OUT_DIR"
ls -1 "$OUT_DIR"
