# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "cache_flow_test",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "pipeline_test",
    size = "large",
    data = ["//test/lib/image:testdata"],
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
        "//src/worker:worker_lib",
        "//test/test_util:pipe_client",
        "//test/test_util:temp_dir",
    ],
)
