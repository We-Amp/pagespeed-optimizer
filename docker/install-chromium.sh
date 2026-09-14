#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Install Chromium from Debian bookworm in Docker containers.
# Ubuntu 24.04 only offers snap-based Chromium which doesn't work in Docker.
# Debian bookworm provides a real chromium package that works on arm64.
#
# Usage in Dockerfile:
#   COPY docker/install-chromium.sh /tmp/
#   RUN /tmp/install-chromium.sh [--with-fonts]
#
# --with-fonts: also install libgtk-3-0t64 and fonts-liberation (for headed tests)

set -euo pipefail

EXTRA_PKGS=""
if [ "${1:-}" = "--with-fonts" ]; then
  EXTRA_PKGS="libgtk-3-0t64 fonts-liberation"
fi

# Ensure gnupg is available for keyring import.
apt-get update && apt-get install -y --no-install-recommends gnupg

# Add Debian bookworm repo with pinning (only chromium from bookworm).
echo "deb [signed-by=/usr/share/keyrings/debian-bookworm.gpg] http://deb.debian.org/debian bookworm main" \
    > /etc/apt/sources.list.d/bookworm.list
curl -fsSL https://ftp-master.debian.org/keys/release-12.asc \
    | gpg --dearmor -o /usr/share/keyrings/debian-bookworm.gpg
printf 'Package: *\nPin: release n=bookworm\nPin-Priority: 100\n\nPackage: chromium chromium-common\nPin: release n=bookworm\nPin-Priority: 900\n' \
    > /etc/apt/preferences.d/bookworm-chromium

# Install chromium + optional font packages.
# The noble t64 libraries are installed FIRST and by their noble names:
# bookworm's chromium depends on pre-t64 names (libasound2, libcups2, …),
# and if apt is left to resolve those it pulls ~18 bookworm libraries into a
# noble image — an unpatched cross-distro surface the dep scan flags forever.
# Pre-installing the noble t64 builds satisfies the deps via Provides, so
# the bookworm pull shrinks to chromium itself plus a couple of libraries
# noble has no compatible build for (libdav1d6, libminizip1).
# shellcheck disable=SC2086
apt-get update \
    && apt-get install -y --no-install-recommends \
       libasound2t64 libcups2t64 libgtk-3-0t64 \
    && apt-get install -y --no-install-recommends chromium $EXTRA_PKGS

# Create symlink expected by headless browser launcher.
ln -sf /usr/bin/chromium /usr/bin/chrome-headless-shell

# Cleanup.
apt-get purge -y gnupg && apt-get autoremove -y
rm -rf /var/lib/apt/lists/*
