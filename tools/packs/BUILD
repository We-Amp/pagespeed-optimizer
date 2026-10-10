# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# Fixture runner for transform packs: runs a pack over input pages and
# compares with golden outputs. The library backs the test in
# //test/lib/packs:fixture_runner_test; the binary regenerates the goldens.
cc_library(
    name = "fixture_runner_lib",
    srcs = ["fixture_runner.cc"],
    hdrs = ["fixture_runner.h"],
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    visibility = ["//:internal"],
    deps = [
        "//lib/base:string_writer",
        "//lib/html:html_keywords",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//lib/packs:decision",
        "//lib/packs:pack",
        "//lib/packs:pack_filter",
        "//lib/packs:pack_loader",
        "//lib/packs:planner",
        "//src/worker:html_transform_filter",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@nlohmann_json//:json",
    ],
)

cc_binary(
    name = "fixture_runner",
    srcs = ["fixture_runner_main.cc"],
    deps = [":fixture_runner_lib"],
)
