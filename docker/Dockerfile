# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build environment for mod_pagespeed 2.1 with sanitizer support.
#
# Base is Ubuntu 22.04 (jammy, glibc 2.35) deliberately. We statically link
# libc++/libc++abi/libunwind into libpagespeed.so + factory_worker; on
# Ubuntu 24.04 (glibc 2.39) those static archives pull `__isoc23_*` symbols
# that bump the binary's GLIBC baseline to 2.38 and break RHEL 9 / Debian 12
# customers. Jammy's libc++-20-dev archives don't reference C23 scanf/strto*
# symbols, so the built binaries advertise GLIBC_2.34 max — the RHEL 9 floor.
# See lib/pagespeed/BUILD linkopts comment for the full rationale.
# Digest-pinned for reproducibility; the dist-upgrade below keeps packages
# current despite the frozen base (pins without an upgrade pass
# re-freeze whatever CVEs the base carried). Do NOT bump to 24.04: the
# whole point of jammy is the GLIBC_2.34 / RHEL 9 floor explained above.
FROM ubuntu:22.04@sha256:4f838adc7181d9039ac795a7d0aba05a9bd9ecd480d294483169c5def983b64d

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install build essentials and Bazel dependencies
# Use clang-20 (GCC has constexpr bugs with abseil hash_policy_traits.h;
# libc++-18 lacks std::atomic_ref required by Cyclone, added in libc++-19;
# clang-20 is not in Ubuntu 22.04 standard repos — requires LLVM apt repo)
RUN apt-get update && apt-get -y dist-upgrade \
    && apt-get install -y wget gnupg \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
       | tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main" \
       | tee /etc/apt/sources.list.d/llvm-toolchain-jammy-20.list \
    && apt-get update && apt-get install -y \
    build-essential \
    clang-20 \
    clang-format-20 \
    clang-tidy-20 \
    cmake \
    curl \
    git \
    gperf \
    lcov \
    libc++-20-dev \
    libc++abi-20-dev \
    libunwind-20-dev \
    lld-20 \
    nasm \
    ninja-build \
    openssh-client \
    perl \
    python3 \
    unzip \
    zip \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/bin/clang-20 /usr/bin/clang \
    && ln -sf /usr/bin/clang++-20 /usr/bin/clang++ \
    && ln -sf /usr/bin/clang-format-20 /usr/bin/clang-format \
    && ln -sf /usr/bin/clang-tidy-20 /usr/bin/clang-tidy \
    && ln -sf /usr/bin/run-clang-tidy-20 /usr/bin/run-clang-tidy \
    && ln -sf /usr/bin/ld.lld-20 /usr/bin/ld.lld \
    && ln -sf /usr/bin/llvm-profdata-20 /usr/bin/llvm-profdata \
    && ln -sf /usr/bin/llvm-cov-20 /usr/bin/llvm-cov \
    && mkdir -p /root/.ssh && ssh-keyscan github.com >> /root/.ssh/known_hosts 2>/dev/null

# Use clang-20 as default compiler
ENV CC=clang-20
ENV CXX=clang++-20

# Install Bazelisk (manages Bazel versions automatically)
# Detect architecture and download appropriate binary
RUN ARCH=$(dpkg --print-architecture) && \
    if [ "$ARCH" = "amd64" ]; then BAZEL_ARCH="amd64"; \
    elif [ "$ARCH" = "arm64" ]; then BAZEL_ARCH="arm64"; \
    else echo "Unsupported architecture: $ARCH" && exit 1; fi && \
    curl -fsSL "https://github.com/bazelbuild/bazelisk/releases/download/v1.20.0/bazelisk-linux-${BAZEL_ARCH}" \
    -o /usr/local/bin/bazel && chmod +x /usr/local/bin/bazel

# Install .NET 9 SDK (used by NuGet package smoke tests)
RUN curl -fsSL --http1.1 https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --channel 9.0 --install-dir /usr/share/dotnet \
    && ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet \
    && rm /tmp/dotnet-install.sh
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1

# Install nginx source headers for building the dynamic module (.so).
# The Bazel nginx repository rule (bazel/nginx.bzl) needs source headers.
# Stub auto-config headers are generated when objs/ is absent, but the
# core nginx headers under src/ must come from real source.
#
# Use nginx.org's official apt repo (stable channel) instead of the
# distro's nginx. Ubuntu 22.04 (jammy) ships nginx 1.18.0 which predates
# `ngx_table_elt_t.next` (added in nginx 1.23.0); ngx_pagespeed_module.cc
# references that field. nginx.org's stable channel currently provides
# 1.30.x, which has it. Both deb and deb-src are installed so we can
# `apt-get source nginx` for the configure step.
#
# PIN + PROVENANCE: the nginx version is pinned to the exact
# value in docker/nginx-version.txt rather than "whatever nginx.org serves
# today". The module embeds NGINX_VERSION_NUM at build time and the runtime
# nginx refuses to load a module built against a different version
# ("module … version 1030002 instead of 1030003" — [emerg], process exits).
# This dev image is content-hash cached (the CI cache key is the sha256 of
# docker/), so an UNPINNED `apt-get source nginx` let a cached image keep
# building an old module against a runtime nginx that had moved forward — the
# 2026-07 CI-Periodic ABI-skew incident (#933). Pinning here makes the version
# a hashed input: bumping docker/nginx-version.txt changes docker/'s content
# hash and forces a dev-image rebuild everywhere, and the assertion below fails
# the build LOUDLY (not a silent red much later) if the pinned version is not
# installable from the channel — the signal to bump the pin deliberately.
COPY nginx-version.txt /tmp/nginx-version
RUN wget -qO- https://nginx.org/keys/nginx_signing.key | gpg --dearmor \
       > /usr/share/keyrings/nginx-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/nginx-archive-keyring.gpg] http://nginx.org/packages/ubuntu jammy nginx" \
       > /etc/apt/sources.list.d/nginx.list \
    && echo "deb-src [signed-by=/usr/share/keyrings/nginx-archive-keyring.gpg] http://nginx.org/packages/ubuntu jammy nginx" \
       >> /etc/apt/sources.list.d/nginx.list \
    && echo "Package: *\nPin: origin nginx.org\nPin-Priority: 900" > /etc/apt/preferences.d/99nginx \
    && apt-get update \
    && NV="$(tr -d '[:space:]' < /tmp/nginx-version)" \
    && echo "Pinning build-time nginx to ${NV} (docker/nginx-version.txt)" \
    && if ! apt-get install -y --allow-downgrades \
         "nginx=${NV}-1~jammy" dpkg-dev libpcre3-dev zlib1g-dev libssl-dev; then \
         echo "::error::nginx=${NV}-1~jammy is not installable from nginx.org/jammy." >&2; \
         echo "::error::The pinned version in docker/nginx-version.txt was pruned from the channel or is wrong — bump it to a version nginx.org currently ships." >&2; \
         exit 1; \
       fi \
    && mkdir -p /nginx-src \
    && cd /nginx-src && apt-get source "nginx=${NV}-1~jammy" \
    && ln -sf "$(ls -d /nginx-src/nginx-*/)" /nginx-src/nginx \
    # Assert the source tree that the module configures against is exactly the
    # pinned version — if apt silently resolved a different one, fail loudly
    # with both versions rather than shipping a skewed module.
    && SRC_V="$(ls -d /nginx-src/nginx-*/ | head -1 | sed 's#.*/nginx-##; s#/##')" \
    && if [ "${SRC_V}" != "${NV}" ]; then \
         echo "::error::nginx source is ${SRC_V} but docker/nginx-version.txt pins ${NV} — module would embed the wrong NGINX_VERSION_NUM." >&2; \
         exit 1; \
       fi \
    && cd /nginx-src/nginx && ./configure --with-compat \
    && rm -rf /var/lib/apt/lists/* /tmp/nginx-version
ENV NGINX_PATH=/nginx-src/nginx

# Set up workspace
WORKDIR /workspace

# Pre-warm Bazel by extracting it (optional, speeds up first build)
RUN bazel version || true

# Default command
CMD ["bazel", "test", "//..."]
