#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run ASan + UBSan tests, with leak detection on Linux.

set -e

if [[ "$(uname)" == "Linux" ]]; then
    echo "Running ASan + UBSan + LeakSanitizer (Linux)"
    exec bazel test --config=asan-leaks "$@"
else
    echo "Running ASan + UBSan (leak detection not supported on $(uname))"
    exec bazel test --config=asan "$@"
fi
