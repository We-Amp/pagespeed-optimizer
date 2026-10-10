# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Transform-pack model, loader and matching. Leaf library: pure functions over
# strings, no HTML parsing and no I/O beyond reading the pack file.

# URL equality normalization and hreflang code validation.
cc_library(
    name = "url_norm",
    srcs = ["url_norm.cc"],
    hdrs = ["url_norm.h"],
    visibility = ["//:internal"],
    deps = [
        "@com_google_absl//absl/strings",
    ],
)

# Placeholder templates: parse, expand, escape.
cc_library(
    name = "template",
    srcs = ["template.cc"],
    hdrs = ["template.h"],
    visibility = ["//:internal"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

# Pack model, limits and value checks.
cc_library(
    name = "pack",
    srcs = ["pack.cc"],
    hdrs = ["pack.h"],
    visibility = ["//:internal"],
    deps = [
        ":template",
        ":url_norm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_re2//:re2",
    ],
)

# Host, path and rule matching; table and cluster lookup.
cc_library(
    name = "matcher",
    srcs = ["matcher.cc"],
    hdrs = ["matcher.h"],
    visibility = ["//:internal"],
    deps = [
        ":pack",
        ":url_norm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_re2//:re2",
    ],
)

# JSON pack file parsing and strict validation.
cc_library(
    name = "pack_loader",
    srcs = ["pack_loader.cc"],
    hdrs = ["pack_loader.h"],
    visibility = ["//:internal"],
    deps = [
        ":matcher",
        ":pack",
        ":url_norm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_re2//:re2",
        "@nlohmann_json//:json",
    ],
)
