# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "url_norm_test",
    deps = [
        "//lib/packs:url_norm",
    ],
)

gtest_test(
    name = "template_test",
    deps = [
        "//lib/packs:template",
    ],
)

gtest_test(
    name = "matcher_test",
    deps = [
        "//lib/packs:matcher",
        "//lib/packs:pack",
        "//lib/packs:pack_loader",
    ],
)

gtest_test(
    name = "pack_loader_test",
    data = ["//packs:examples"],
    deps = [
        "//lib/packs:matcher",
        "//lib/packs:pack",
        "//lib/packs:pack_loader",
        "//src/product_version:version",
        "@com_google_absl//absl/strings",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "planner_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/packs:decision",
        "//lib/packs:pack",
        "//lib/packs:pack_loader",
        "//lib/packs:planner",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "pack_filter_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/base:string_writer",
        "//lib/html:html_keywords",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//lib/packs:pack",
        "//lib/packs:pack_filter",
        "//lib/packs:pack_loader",
        "//src/worker:html_transform_filter",
        "@com_google_absl//absl/strings",
    ],
)
