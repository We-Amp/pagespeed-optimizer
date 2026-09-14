#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Async-CSS probe worker entrypoint — delegates to the canonical entrypoint.
#
# API posture contract: docker/entrypoint-worker.sh. A non-loopback bind needs
# PAGESPEED_API_ALLOW_REMOTE=true AND a token of at least 16 characters;
# tools/ci/check-entrypoint-api-posture.sh enforces that across every copy.

set -euo pipefail

export DATA_DIR=/shared

# --unsafe-force-async-css is CLI-only by design (src/worker/unsafe_force_async_css.h):
# it must not be reachable through config, env, or the management API on a
# running worker. The canonical entrypoint owns the argv (EULA, socket paths)
# and resolves `factory_worker` through PATH, so a PATH shim appends the
# flag without forking that argv construction and without adding a generic
# extra-args hook to the shipped image.
if [ "${ASYNC_CSS_PROBE_FORCE:-0}" = "1" ]; then
    mkdir -p /tmp/probe-bin
    cat > /tmp/probe-bin/factory_worker <<'SHIM'
#!/bin/sh
exec /usr/local/bin/factory_worker "$@" --unsafe-force-async-css
SHIM
    chmod +x /tmp/probe-bin/factory_worker
    export PATH="/tmp/probe-bin:$PATH"
    echo "[probe] forcing async-CSS deferral (--unsafe-force-async-css)" >&2
fi

exec /docker/entrypoint-worker.sh
