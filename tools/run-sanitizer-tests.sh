#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run tests with sanitizers enabled.
# Used by pre-commit hooks and CI.

set -e

echo "=== Running tests with ASan + UBSan ==="
./tools/run-asan-tests.sh //...

echo ""
echo "=== Running tests with TSan ==="
bazel test --config=tsan //...

echo ""
echo "All sanitizer tests passed."
