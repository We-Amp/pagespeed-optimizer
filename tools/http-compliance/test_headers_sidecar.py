# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Response-header sidecar, store side (issue #208).

The nginx front stage classifies the origin's response headers before it
records a response:

* only reproducible headers (the security set: ACAO, CSP, nosniff,
  Referrer-Policy) -> recorded, with the headers stored beside the entry;
* any header a cache hit cannot reproduce (X-Powered-By, a custom header)
  -> never cached, served from the origin each time, and an entry recorded
  earlier under different headers is removed;
* a Content-Security-Policy carrying a nonce -> never cached;
* Vary: Origin -> never cached (the existing Vary gate).

Replaying the stored headers on a HIT is the next step and is NOT asserted
here: a HIT still serves exactly as before this change.

The counters live in the shared serve-statistics file the front stage writes
and the optimizer exports on /v1/metrics.  The compliance stack does not
expose the optimizer API, so the counters are read from the file itself,
inside the nginx container.
"""

import os
import struct
import subprocess
import time

import pytest

COMPOSE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "docker-compose.yml")

SERVE_STATS_PATH = "/shared/.pagespeed-serve-stats"
SERVE_STATS_VERSION = 11
# v11 layout (src/worker/serve_stats.h, sizeof == 11488): the five sidecar
# counters sit just before counting_since_unix_day (8 bytes) and boot_id (16).
SIDECAR_COUNTERS_OFFSET = 11488 - 16 - 8 - 5 * 8
SIDECAR_COUNTER_NAMES = (
    "fall_through",
    "never_optimized",
    "replayed",
    "missing",
    "write_failed",
)


def read_sidecar_counters() -> dict[str, int]:
    """Read the five v11 sidecar counters from the shared stats file."""
    out = subprocess.run(
        [
            "docker",
            "compose",
            "-f",
            COMPOSE_FILE,
            "exec",
            "-T",
            "nginx",
            "cat",
            SERVE_STATS_PATH,
        ],
        capture_output=True,
        check=False,
    )
    if out.returncode != 0 or len(out.stdout) < SIDECAR_COUNTERS_OFFSET + 40:
        pytest.skip(
            "serve statistics file not readable from the nginx container: "
            + out.stderr.decode(errors="replace")[:200]
        )
    _magic, version = struct.unpack_from("<II", out.stdout, 0)
    assert version == SERVE_STATS_VERSION, (
        f"serve statistics file is version {version}, expected "
        f"{SERVE_STATS_VERSION}: the layout offsets here are stale"
    )
    values = struct.unpack_from("<5Q", out.stdout, SIDECAR_COUNTERS_OFFSET)
    return dict(zip(SIDECAR_COUNTER_NAMES, values))


def x_pagespeed(response) -> str:
    return response.headers.get("X-PageSpeed", "<absent>")


def requests_never_hit(client, path: str, n: int = 6, pause: float = 0.3):
    """Request `path` n times with pauses; return the X-PageSpeed values."""
    seen = []
    for _ in range(n):
        r = client.get(path)
        assert r.status_code == 200, f"{path}: {r.status_code}"
        seen.append(x_pagespeed(r))
        time.sleep(pause)
    return seen


class TestHeadersSidecarRecord:
    def test_reproducible_security_headers_are_cached(self, client):
        """ACAO + CSP + nosniff + Referrer-Policy: recorded, then a HIT."""
        before = read_sidecar_counters()
        first = client.get("/sidecar/asset.css")
        assert first.status_code == 200
        # The MISS passes every origin header through untouched.
        assert first.headers.get("Access-Control-Allow-Origin") == "*"
        assert first.headers.get("Referrer-Policy") == "no-referrer"
        hit = client.poll_for_hit("/sidecar/asset.css")
        client.assert_hit(hit)
        after = read_sidecar_counters()
        assert after["fall_through"] == before["fall_through"]
        assert after["never_optimized"] == before["never_optimized"]
        assert after["write_failed"] == before["write_failed"]

    def test_unreproducible_header_is_never_cached(self, client):
        """X-Powered-By cannot be reproduced on a hit: never a HIT."""
        before = read_sidecar_counters()
        seen = requests_never_hit(client, "/sidecar/custom.css")
        assert "HIT" not in seen, seen
        assert all(v == "MISS" for v in seen), seen
        # The origin's header reaches the client on every (origin) response.
        r = client.get("/sidecar/custom.css")
        assert r.headers.get("X-Powered-By") == "PHP/8.3"
        after = read_sidecar_counters()
        assert after["fall_through"] >= before["fall_through"] + len(seen), (
            before,
            after,
        )

    def test_nonce_csp_is_never_cached(self, client):
        before = read_sidecar_counters()
        seen = requests_never_hit(client, "/sidecar/nonce.html")
        assert "HIT" not in seen, seen
        after = read_sidecar_counters()
        assert after["never_optimized"] >= before["never_optimized"] + len(seen)
        # Disjoint from the fall-through class.
        assert after["fall_through"] == before["fall_through"]

    def test_vary_origin_is_never_cached(self, client):
        before = read_sidecar_counters()
        seen = requests_never_hit(client, "/sidecar/vary-origin.css")
        assert "HIT" not in seen, seen
        # Refused by the Vary gate before the sidecar gate runs: not counted
        # here a second time.
        after = read_sidecar_counters()
        assert after["fall_through"] == before["fall_through"]

    def test_header_change_evicts_the_cached_entry(self, client, origin_client):
        """A cached URL whose origin starts sending an unreproducible header
        stops being served from cache at the next origin fetch."""
        path = "/sidecar/flip.css"
        origin_client.get("/sidecar/control?custom=off")
        try:
            client.get(path)
            hit = client.poll_for_hit(path)
            client.assert_hit(hit)

            origin_client.get("/sidecar/control?custom=on")
            before = read_sidecar_counters()
            # max-age=1: wait past expiry so the next request goes to origin.
            time.sleep(2.5)
            refetch = client.get(path)
            assert refetch.headers.get("X-Custom-Foo") == "bar"
            assert x_pagespeed(refetch) != "HIT"
            seen = requests_never_hit(client, path, n=4)
            assert "HIT" not in seen, seen
            after = read_sidecar_counters()
            assert after["fall_through"] > before["fall_through"]
        finally:
            origin_client.get("/sidecar/control?custom=off")
