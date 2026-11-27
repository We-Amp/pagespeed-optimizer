# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_test")
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

gtest_test(
    name = "abi_site_test",
    size = "medium",
    deps = [
        "//lib/pagespeed:pagespeed_api",
        "//src/proto:worker_ipc",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "abi_sited_html_test",
    size = "medium",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/pagespeed:pagespeed_api",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "abi_markdown_coding_test",
    size = "medium",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/pagespeed:pagespeed_api",
        "//src/worker:shared_config",
        "//test/test_util:temp_dir",
        "@cyclone",
    ],
)

# The public header compiled as C at its stated floor, C11 (it uses an
# anonymous union), pedantic, with warnings as errors.
cc_test(
    name = "c_header_test",
    size = "small",
    srcs = ["c_header_test.c"],
    copts = select({
        "@platforms//os:windows": ["/std:c11"],
        "//conditions:default": [
            "-std=c11",
            "-pedantic-errors",
            "-Wall",
            "-Wextra",
            "-Werror",
        ],
    }),
    deps = ["//lib/pagespeed:pagespeed_api"],
)
