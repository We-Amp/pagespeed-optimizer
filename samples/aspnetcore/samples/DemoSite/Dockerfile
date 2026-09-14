# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# PageSpeed ASP.NET Core Demo — multi-stage build
#
# Builds libpagespeed.so + factory_worker from source (Bazel + clang-20),
# then builds and publishes the DemoSite ASP.NET Core app. Two runtime
# targets: aspnet-runtime (ASP.NET app) and worker-runtime (C++ worker).
#
# Standalone (no worker):
#   docker build -f samples/aspnetcore/samples/DemoSite/Dockerfile \
#                --target aspnet-runtime -t pagespeed-aspnetcore-demo .
#   docker run --rm -p 5100:8080 pagespeed-aspnetcore-demo
#
# Full stack (with worker):
#   docker compose -f samples/aspnetcore/samples/DemoSite/docker-compose.yml up --build
#
# Prerequisites:
#   reference/cyclone/ must exist (gitignored; clone separately).

# ── Stage 1: Build native binaries ───────────────────────────────────────────

FROM ubuntu:24.04 AS build-native

ENV DEBIAN_FRONTEND=noninteractive

# clang-20 + libc++-20 (GCC has constexpr bugs with abseil;
# libc++-18 lacks std::atomic_ref required by Cyclone;
# clang-20 requires LLVM apt repo — not in Ubuntu 24.04 standard repos)
RUN apt-get update && apt-get install -y wget gnupg \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
       | tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" \
       | tee /etc/apt/sources.list.d/llvm-toolchain-noble-20.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang-20 \
        curl \
        git \
        gperf \
        libc++-20-dev \
        libc++abi-20-dev \
        libclang-rt-20-dev \
        lld-20 \
        nasm \
        python3 \
        unzip \
        zip \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/bin/clang-20 /usr/bin/clang \
    && ln -sf /usr/bin/clang++-20 /usr/bin/clang++ \
    && ln -sf /usr/bin/ld.lld-20 /usr/bin/ld.lld

ENV CC=clang-20
ENV CXX=clang++-20

# Bazelisk (auto-downloads the correct Bazel version)
RUN ARCH=$(dpkg --print-architecture) && \
    curl -fsSL "https://github.com/bazelbuild/bazelisk/releases/download/v1.20.0/bazelisk-linux-${ARCH}" \
        -o /usr/local/bin/bazel && \
    chmod +x /usr/local/bin/bazel

WORKDIR /workspace

# Copy only what Bazel needs — avoids sending the entire repo as context.
# Layer order: least-changing files first for better caching.
COPY .bazelrc .bazelversion MODULE.bazel BUILD ./
COPY bazel/ bazel/
COPY third_party/ third_party/
COPY reference/cyclone/ reference/cyclone/
COPY lib/ lib/
COPY src/ src/

# Build both the shared library and the worker binary.
RUN bazel build --config=docker -c opt --cxxopt=-O3 --cxxopt=-DNDEBUG \
        //lib/pagespeed:libpagespeed.so \
        //src/worker:factory_worker

# Stage the binaries for the final images. libc++/libc++abi/libunwind are
# statically linked into both binaries since v2.0.2 — no separate runtime
# .so files needed.
RUN mkdir /staging && \
    cp bazel-bin/lib/pagespeed/libpagespeed.so /staging/ && \
    cp bazel-bin/src/worker/factory_worker /staging/

# ── Stage 2: Build .NET app ────────────────────────────────────────────────

FROM mcr.microsoft.com/dotnet/sdk:9.0-noble AS build-dotnet

WORKDIR /src

# Copy source (exclude local obj/bin via find+delete after COPY).
COPY samples/aspnetcore/src/ src/
COPY samples/aspnetcore/samples/DemoSite/ samples/DemoSite/
RUN find /src -type d \( -name obj -o -name bin \) -exec rm -rf {} + 2>/dev/null; true
RUN dotnet publish samples/DemoSite/DemoSite.csproj \
        -c Release \
        -o /app

# ── Stage 3a: ASP.NET runtime ───────────────────────────────────────────────

FROM mcr.microsoft.com/dotnet/aspnet:9.0-noble AS aspnet-runtime

# Cache volume directory
RUN mkdir -p /tmp/pagespeed-demo

WORKDIR /app
COPY --from=build-dotnet /app .

# Native library — placed in /app so .NET's default DllImportResolver
# finds it next to the assembly. C++ runtime (libc++/libc++abi/libunwind)
# is statically linked into libpagespeed.so since v2.0.2.
COPY --from=build-native /staging/libpagespeed.so /app/

ENV ASPNETCORE_URLS=http://+:8080
EXPOSE 8080

ENTRYPOINT ["dotnet", "DemoSite.dll"]

# ── Stage 2b: Build web console (SvelteKit static) ──────────────────────────

FROM node:22-slim AS build-console

RUN corepack enable && corepack prepare pnpm@latest --activate

WORKDIR /workbench
COPY tools/workbench/pnpm-workspace.yaml tools/workbench/package.json tools/workbench/pnpm-lock.yaml tools/workbench/tsconfig.base.json ./
COPY tools/workbench/packages/ packages/
RUN pnpm install --frozen-lockfile
RUN pnpm -C packages/web-shell build

# ── Stage 3b: Worker runtime ────────────────────────────────────────────────

FROM ubuntu:24.04 AS worker-runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY docker/install-chromium.sh /tmp/install-chromium.sh
RUN /tmp/install-chromium.sh --with-fonts && rm /tmp/install-chromium.sh

COPY --from=build-native /staging/factory_worker /usr/local/bin/
# C++ runtime statically linked into factory_worker since v2.0.2.

COPY --from=build-console /workbench/packages/web-shell/dist/ /opt/console/

COPY samples/aspnetcore/samples/DemoSite/entrypoint-worker.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

RUN mkdir -p /shared

EXPOSE 9880

ENTRYPOINT ["/entrypoint.sh"]
