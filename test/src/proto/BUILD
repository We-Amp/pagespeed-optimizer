# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "notification_sender_test",
    size = "large",
    deps = [
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "worker_ipc_test",
    deps = [
        "//lib/classify:option_context",
        "//src/proto:worker_ipc",
    ],
)
