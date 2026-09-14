# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary")

# Q3 threshold measurement. Manual: it needs a real browser and the byte-frozen
# fixture, so it is never part of a test run. See the file header for how to
# run it and what its output is for.
cc_binary(
    name = "measure_validation_threshold",
    srcs = ["measure_validation_threshold.cc"],
    tags = ["manual"],
    deps = [
        "//src/browser:browser_css_extractor",
        "//src/browser:chrome_process",
        "//src/browser:critical_css_validator",
        "//src/browser:visual_regression_gate",
        "//src/worker:css_cache_inliner",
        "//src/worker:html_scanner",
        "@com_google_absl//absl/status:statusor",
        "@libuv",
    ],
)
