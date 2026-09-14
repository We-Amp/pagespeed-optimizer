# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "cache_control_header",
    srcs = ["cache_control_header.cc"],
    hdrs = ["cache_control_header.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:alternate_metadata",
        "//lib/classify:content_type",
    ],
)

cc_library(
    name = "freshness",
    srcs = ["freshness.cc"],
    hdrs = ["freshness.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:alternate_metadata",
        "//lib/classify:content_type",
    ],
)

cc_library(
    name = "vary_storability",
    srcs = ["vary_storability.cc"],
    hdrs = ["vary_storability.h"],
    visibility = ["//:internal"],
)

cc_library(
    name = "vary_emission",
    srcs = ["vary_emission.cc"],
    hdrs = ["vary_emission.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:content_type",
    ],
)

cc_library(
    name = "headers_sidecar",
    srcs = ["headers_sidecar.cc"],
    hdrs = ["headers_sidecar.h"],
    visibility = ["//:internal"],
    deps = [
        ":vary_storability",
        "//lib/classify:alternate_id",
    ],
)

cc_library(
    name = "origin_cache_control",
    srcs = ["origin_cache_control.cc"],
    hdrs = ["origin_cache_control.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:alternate_metadata",
    ],
)

cc_library(
    name = "pagespeed_cache",
    srcs = ["cache.cc"],
    hdrs = ["cache.h"],
    visibility = ["//:internal"],
    deps = [
        ":headers_sidecar",
        "//lib/base:atomic_file_writer",
        "//lib/base:message_handler",
        "//lib/base:posix_compat",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:hostname",
        "//lib/classify:option_context",
        "//lib/classify:pagespeed_selector",
        "@cyclone",
    ],
)
