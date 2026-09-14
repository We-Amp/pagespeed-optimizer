#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run test coverage inside Docker and generate HTML report.
# Usage: ./tools/docker-coverage.sh [targets...]
# Examples:
#   ./tools/docker-coverage.sh                      # All tests
#   ./tools/docker-coverage.sh //test/lib/base/...  # Specific tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

TARGETS="${@:-//...}"

echo "=== Running coverage in Docker: $TARGETS ==="

# Source mounted read-write so the HTML report lands on the host.
# Some tests may fail (e.g. browser tests needing Chrome); we still
# generate the report from whatever coverage data was collected.
"$PROJECT_ROOT/docker/run-bazel.sh" --rw --no-ssh -- \
    bash -c "
        bazel coverage --config=docker --symlink_prefix=/dev/null/ \"$TARGETS\" || true
        LCOV_REPORT=\"\$(bazel info output_path)/_coverage/_coverage_report.dat\"
        if [ ! -f \"\$LCOV_REPORT\" ] || [ ! -s \"\$LCOV_REPORT\" ]; then
            echo 'ERROR: No coverage data collected'
            exit 1
        fi
        # Strip ML-generated predictor code (287K lines, not meaningful to cover).
        FILTERED=\"\$(mktemp)\"
        lcov --remove \"\$LCOV_REPORT\" '*/lib/image/generated/*' 'lib/image/generated/*' \
            --output-file \"\$FILTERED\" --quiet \
            --ignore-errors inconsistent,inconsistent 2>/dev/null || true
        if [ ! -s \"\$FILTERED\" ]; then
            cp \"\$LCOV_REPORT\" \"\$FILTERED\"
        fi
        rm -rf coverage-html
        genhtml \"\$FILTERED\" --output-directory coverage-html --quiet \
            --ignore-errors inconsistent,inconsistent \
            --ignore-errors unmapped,unmapped
        rm -f \"\$FILTERED\"
        echo ''
        echo 'Coverage report generated.'
    "

echo ""
echo "Coverage report: $PROJECT_ROOT/coverage-html/index.html"
