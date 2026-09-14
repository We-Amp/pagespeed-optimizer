# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "abi_layout_test",
    deps = [
        "//lib/pagespeed:pagespeed_api",
    ],
)

gtest_test(
    name = "abi_option_context_test",
    deps = [
        "//lib/pagespeed:pagespeed_api",
    ],
)

gtest_test(
    name = "abi_origin_state_test",
    size = "medium",
    deps = [
        "//lib/cache:cache_control_header",
        "//lib/cache:freshness",
        "//lib/classify:alternate_metadata",
        "//lib/pagespeed:pagespeed_api",
        "//test/test_util:temp_dir",
    ],
)
