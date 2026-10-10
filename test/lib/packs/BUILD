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
        "@com_google_absl//absl/strings",
        "@nlohmann_json//:json",
    ],
)
