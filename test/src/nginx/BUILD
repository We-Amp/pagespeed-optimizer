# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "ngx_pagespeed_test",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//src/nginx:mime_util",
        "//src/nginx:zerocopy_barrier",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "authz_cache_gate_test",
    deps = [
        "//lib/classify:alternate_metadata",
        "//src/nginx:authz_cache_gate",
    ],
)

gtest_test(
    name = "revalidation_storability_test",
    deps = [
        "//lib/classify:alternate_metadata",
        "//src/nginx:revalidation_storability",
    ],
)

# Cache-effect tests for the restamp dispatch: instantiates the
# ApplyRestampVerdict template against a REAL PageSpeedCache on a temp
# volume, so eviction/skip are asserted by their effect on the cache.
gtest_test(
    name = "restamp_apply_test",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//src/nginx:revalidation_storability",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "etag_util_test",
    deps = [
        "//lib/classify:alternate_metadata",
        "//src/nginx:etag_util",
    ],
)

gtest_test(
    name = "early_hints_test",
    deps = [
        "//src/nginx:early_hints_util",
    ],
)

gtest_test(
    name = "mime_util_test",
    deps = [
        "//src/nginx:mime_util",
    ],
)
