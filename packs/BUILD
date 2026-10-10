# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Example transform pack (documentation and test data).
filegroup(
    name = "examples",
    srcs = ["edge-seo/pack.example.json"],
    visibility = ["//test:__subpackages__"],
)

# Golden fixtures for the edge-seo head rules: the fixture pack and, per case,
# input.html, request.json, expected.html and expected.decisions.jsonl.
filegroup(
    name = "edge_seo_fixtures",
    srcs = glob(["edge-seo/fixtures/**"]),
    visibility = ["//test:__subpackages__"],
)
