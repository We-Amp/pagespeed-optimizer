# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "temp_dir",
    hdrs = ["temp_dir.h"],
    visibility = ["//test:__subpackages__", "//lib:__subpackages__"],
)

cc_library(
    name = "tcp_client",
    hdrs = ["tcp_client.h"],
    visibility = ["//test:__subpackages__", "//lib:__subpackages__"],
    deps = ["//lib/base:string_util"],
)

cc_library(
    name = "cdp_pipe",
    hdrs = ["cdp_pipe.h"],
    visibility = ["//test:__subpackages__", "//lib:__subpackages__"],
    deps = ["@libuv"],
)

cc_library(
    name = "cdp_scripted_peer",
    testonly = True,
    hdrs = ["cdp_scripted_peer.h"],
    visibility = ["//test:__subpackages__", "//lib:__subpackages__"],
    deps = [
        ":cdp_pipe",
        "//src/browser:cdp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "png_image",
    testonly = True,
    hdrs = ["png_image.h"],
    visibility = ["//test:__subpackages__", "//lib:__subpackages__"],
    deps = ["@libpng"],
)

cc_library(
    name = "pipe_client",
    hdrs = ["pipe_client.h"],
    visibility = ["//test:__subpackages__"],
)

# Test-only access to the raw substrate under PageSpeedCache.  `testonly` is
# the enforcement half of the narrowed accessor: production targets cannot
# depend on this, so there is no production path to a whole-key write.
cc_library(
    name = "cache_test_peer",
    testonly = True,
    hdrs = ["cache_test_peer.h"],
    visibility = ["//test:__subpackages__", "//lib:__subpackages__"],
    deps = [
        "//lib/cache:pagespeed_cache",
        "@cyclone",
    ],
)
