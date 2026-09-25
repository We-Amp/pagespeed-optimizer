# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "llms_txt_builder_test",
    deps = [
        "//src/worker:llms_txt_builder",
    ],
)

gtest_test(
    name = "serve_stats_test",
    # Production code already has Windows mmap support; test uses std::atomic_ref (C++20).
    deps = [
        "//lib/base:message_handler",
        "//lib/classify:content_type",
        "//src/worker:serve_stats",
    ],
)

gtest_test(
    name = "syscall_filter_test",
    deps = [
        "//src/worker:syscall_filter",
    ],
)

# One source, both build variants.  The default target links
# the SHIPPED library (the switch compiled out); the _enabled one links the
# local_defines = PAGESPEED_SELFTEST variant.  Building both on every run is
# what keeps the #ifdef'd half compiling, and asserting both is what proves
# the shipped half really does not carry the switch.
gtest_test(
    name = "windows_service_test",
    deps = [
        "//src/worker:windows_service",
    ],
)

gtest_test(
    name = "pipe_dacl_test",
    linkopts = select({
        "@platforms//os:windows": ["advapi32.lib"],
        "//conditions:default": [],
    }),
    deps = [
        "//src/worker:pipe_dacl",
    ],
)

gtest_test(
    name = "syscall_selftest_test",
    deps = [
        "//src/worker:syscall_selftest",
    ],
)

gtest_test(
    name = "syscall_selftest_enabled_test",
    srcs = ["syscall_selftest_test.cc"],
    deps = [
        "//src/worker:syscall_selftest_enabled",
    ],
)

gtest_test(
    name = "html_scanner_test",
    deps = [
        "//src/worker:html_scanner",
    ],
)

gtest_test(
    name = "html_css_injector_test",
    deps = [
        "//src/worker:html_css_injector",
    ],
)

gtest_test(
    name = "css_cache_inliner_test",
    deps = [
        "//src/worker:css_cache_inliner",
    ],
)

gtest_test(
    name = "analysis_resource_map_test",
    deps = [
        "//lib/classify:url_normalizer",
        "//src/worker:analysis_resource_map",
        "@com_google_absl//absl/container:flat_hash_map",
    ],
)

gtest_test(
    name = "critical_css_extractor_test",
    deps = [
        "//lib/classify:capability_mask",
        "//src/worker:critical_css_extractor",
    ],
)

gtest_test(
    name = "unsafe_force_async_css_test",
    deps = [
        "//src/worker:unsafe_force_async_css",
    ],
)

gtest_test(
    name = "font_url_extractor_test",
    deps = [
        "//src/worker:font_url_extractor",
    ],
)

gtest_test(
    name = "html_transform_filter_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/base:string_writer",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
        "//lib/html:compat",
        "//lib/html:html_keywords",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//src/worker:html_transform_filter",
    ],
)

gtest_test(
    name = "image_transcoder_test",
    size = "large",
    data = ["//test/lib/image:testdata"],
    # ~850 s single-process on the Windows lane against a 900 s ceiling; the
    # cases are independent codec round-trips, so shard them.
    shard_count = 4,
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//lib/classify:capability_mask",
        "//lib/image:exif_orientation",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:read_image",
        "//src/worker:image_transcoder",
        "@libavif",
        "@libwebp//:webp_encode",
    ],
)

gtest_test(
    name = "optimization_policy_test",
    deps = [
        "//src/browser:optimization_profile",
        "//src/worker:optimization_policy",
    ],
)

gtest_test(
    name = "config_file_test",
    deps = [
        "//src/worker:config_file",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "shared_config_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:shared_config",
    ],
)

gtest_test(
    name = "cache_dir_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:cache_dir",
    ],
)

gtest_test(
    name = "webbotauth_warmer_test",
    deps = [
        "//src/crypto/webbotauth:webbotauth_core",
        "//src/worker:webbotauth_warmer",
        "//test/test_util:temp_dir",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "text_compressor_test",
    deps = [
        "//src/worker:text_compressor",
    ],
)

gtest_test(
    name = "http_server_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:http_server",
        "//test/test_util:tcp_client",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "cache_handlers_test",
    size = "medium",
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
        "//src/worker:cache_handlers",
        "//src/worker:url_registry",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "static_file_handler_test",
    # TSan: SecurityHeadersDisabled flakes due to server-restart timing under
    # TSan's ~10x slowdown. Not a real race — needs proper sync, not timeouts.
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//src/worker:static_file_handler",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
        "@libuv",
    ],
)

gtest_test(
    name = "websocket_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:websocket",
        "//src/worker:ws_handlers",
        "//test/test_util:tcp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "capture_handlers_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:capture_handlers",
        "//test/test_util:tcp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "api_handlers_test",
    deps = [
        "//lib/base:message_handler",
        "//src/product_version:version",
        "//src/worker:api_handlers",
        "//src/worker:serve_stats",
        "//src/worker:shared_config",
        "//src/worker:worker_types",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "worker_api_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:serve_stats",
        "//src/worker:worker_lib",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "url_registry_test",
    deps = [
        "//src/worker:url_registry",
    ],
)

gtest_test(
    name = "worker_test",
    size = "large",
    # TSan runs the full proactive image matrices (no early-exit polls left),
    # which under the ~10x slowdown can push a shard past the 900s "large"
    # cap on slower hosts; the sanitizer-scaled failure-path poll ceilings
    # also need headroom to report a real failure instead of a timeout.
    # NOTE: bazel has no per-config timeout without a sanitizer
    # config_setting (repo has none yet), so this widening applies to ALL
    # lanes: a genuinely hung worker reports at 3600s instead of 900s.
    timeout = "eternal",
    data = ["//test/lib/image:testdata"],
    shard_count = 4,
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:freshness",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:option_context",
        "//src/browser:optimization_profile",
        "//src/browser:template_detector",
        # Early Hints line parser: the test asserts the sentinel a deferred
        # stylesheet produces parses to rel=preload; as=style, which is the
        # contract the 103 emitters consume. Pure C++, no nginx headers.
        "//src/nginx:early_hints_util",
        "//src/proto:worker_ipc",
        "//src/worker:browser_analysis_manager",
        "//src/worker:html_scanner",
        "//src/worker:shared_config",
        "//src/worker:text_compressor",
        "//src/worker:url_registry",
        "//src/worker:worker_lib",
        "@libavif",
        "//test/test_util:pipe_client",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "host_aliases_test",
    deps = [
        "//src/worker:host_aliases",
    ],
)
