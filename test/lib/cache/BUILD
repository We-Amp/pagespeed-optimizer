# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "cache_control_header_test",
    deps = [
        "//lib/cache:cache_control_header",
        "//lib/classify:alternate_metadata",
    ],
)

gtest_test(
    name = "cache_freshness_test",
    deps = [
        "//lib/cache:freshness",
        "//lib/classify:alternate_metadata",
    ],
)

gtest_test(
    name = "cache_test",
    size = "large",  # medium (300s) too tight on arm64/macOS runners
    srcs = ["cache_test.cc"],
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//test/test_util:cache_test_peer",
        "//test/test_util:temp_dir",
        "@cyclone",
    ],
)

gtest_test(
    name = "durable_originals_test",
    size = "large",  # same volume/temp-dir shape as cache_test
    srcs = ["durable_originals_test.cc"],
    deps = [
        "//lib/cache:freshness",
        "//lib/cache:headers_sidecar",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//test/test_util:cache_test_peer",
        "//test/test_util:temp_dir",
        "@cyclone",
    ],
)

gtest_test(
    name = "option_context_key_test",
    size = "large",  # same volume/temp-dir shape as cache_test
    srcs = ["option_context_key_test.cc"],
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:option_context",
        "//test/test_util:cache_test_peer",
        "//test/test_util:temp_dir",
        "@cyclone",
    ],
)

gtest_test(
    name = "headers_sidecar_test",
    srcs = ["headers_sidecar_test.cc"],
    deps = [
        "//lib/cache:headers_sidecar",
        "//lib/cache:vary_storability",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
    ],
)

gtest_test(
    name = "headers_sidecar_store_test",
    size = "large",  # same volume/temp-dir shape as cache_test
    srcs = ["headers_sidecar_store_test.cc"],
    deps = [
        "//lib/cache:headers_sidecar",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//test/test_util:cache_test_peer",
        "//test/test_util:temp_dir",
        "@cyclone",
    ],
)

gtest_test(
    name = "cache_burst_test",
    size = "large",
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "vary_storability_test",
    deps = [
        "//lib/cache:vary_storability",
        "//lib/classify:capability_mask",
    ],
)

gtest_test(
    name = "vary_emission_test",
    deps = [
        "//lib/cache:vary_emission",
        "//lib/classify:content_type",
    ],
)

gtest_test(
    name = "origin_cache_control_test",
    deps = [
        "//lib/cache:origin_cache_control",
        "//lib/classify:alternate_metadata",
    ],
)

gtest_test(
    name = "alternate_publish_race_test",
    size = "medium",
    srcs = ["alternate_publish_race_test.cc"],
    # Reads one storage-layer statistic from another thread while a write is
    # parked; the sanitizer run of this case adds nothing the default run
    # does not show.
    tags = ["no_tsan"],
    deps = [
        "//test/test_util:temp_dir",
        "@cyclone",
    ],
)

# A full purge while other threads read and write through the cache layer.
# Its memory-safety proof is the sanitizer run (linux-asan / linux-tsan jobs);
# Linux only, so it runs in the Linux containers.
gtest_test(
    name = "purge_under_load_test",
    size = "large",
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//test/test_util:temp_dir",
    ],
)
