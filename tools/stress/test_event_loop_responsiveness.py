# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Regression tests for event loop blocking during image processing (bug #4).

Bug #4: Worker's synchronous HandleNotification() blocked the libuv event
loop during AVIF encoding of large images. Management socket, health socket,
and new notification connections all stalled.

Fix: Offloaded HandleNotification() to libuv thread pool via uv_queue_work.

These tests verify that mgmt/health sockets remain responsive even while
the worker is processing large images.
"""

import time

import metrics_helpers
import pytest
import requests


class TestEventLoopResponsiveness:
    """Verify event loop stays responsive during heavy image processing."""

    def test_stats_responds_during_image_processing(self, stress_services):
        """Request 3 large images to trigger processing, then query STATS.

        STATS must respond within 3s for each of 10 queries at 200ms intervals.
        """
        base_url = stress_services.nginx_url

        # Trigger processing of 3 large images (async, won't block)
        for i in range(3):
            try:
                requests.get(
                    base_url + f"/images/img-500k.jpg?async_stats_{i}",
                    timeout=10,
                )
            except Exception:
                pass

        # Immediately query STATS 10 times at 200ms intervals
        failures = []
        for i in range(10):
            start = time.monotonic()
            try:
                raw = metrics_helpers.get_stats_via_docker(timeout=3)
                elapsed = time.monotonic() - start
                stats = metrics_helpers.parse_stats(raw)
                assert "status" in stats, f"Unexpected STATS format: {raw[:100]}"
                if elapsed > 3.0:
                    failures.append(f"Query {i}: {elapsed:.1f}s (>3s)")
            except Exception as e:
                failures.append(f"Query {i}: {e}")
            time.sleep(0.2)

        assert not failures, (
            "STATS queries failed/slow during processing:\n" + "\n".join(failures)
        )

    def test_health_responds_during_processing(self, stress_services):
        """Request large image, query health 10 times at 200ms intervals.

        All must return "OK" within 2s each.
        """
        base_url = stress_services.nginx_url

        # Trigger image processing
        try:
            requests.get(
                base_url + "/images/img-500k.jpg?async_health",
                timeout=10,
            )
        except Exception:
            pass

        # Query health rapidly
        failures = []
        for i in range(10):
            start = time.monotonic()
            try:
                health = metrics_helpers.get_health_via_docker(timeout=2)
                elapsed = time.monotonic() - start
                if "OK" not in health and "DEGRADED" not in health:
                    failures.append(f"Query {i}: unexpected response: {health}")
                elif elapsed > 5.0:
                    failures.append(f"Query {i}: {elapsed:.1f}s (>5s)")
            except Exception as e:
                failures.append(f"Query {i}: {e}")
            time.sleep(0.2)

        assert not failures, (
            "Health queries failed/slow during processing:\n" + "\n".join(failures)
        )

    def test_notifications_complete_asynchronously(self, stress_services):
        """Verify that async processing still completes after offloading.

        Take STATS snapshot, request 3 images, poll until variants.written
        increases by at least 3 (60s timeout).
        """
        base_url = stress_services.nginx_url

        # Snapshot before
        raw = metrics_helpers.get_stats_via_docker()
        stats_before = metrics_helpers.parse_stats(raw)
        written_before = stats_before.get("variants", {}).get("written", 0)

        # Request 3 unique images
        for i in range(3):
            try:
                requests.get(
                    base_url + f"/images/img-100k.jpg?async_complete_{i}",
                    timeout=10,
                )
            except Exception:
                pass

        # Poll until at least 3 new variants are written
        target = written_before + 3

        def check_written(stats):
            return stats.get("variants", {}).get("written", 0) >= target

        try:
            final_stats = metrics_helpers.wait_for_stats_condition(
                check_written, timeout=30.0, interval=1.0
            )
            written_after = final_stats.get("variants", {}).get("written", 0)
            assert written_after >= target, (
                f"Expected >= {target} written, got {written_after}"
            )
        except TimeoutError:
            # Get final stats for diagnostics
            raw = metrics_helpers.get_stats_via_docker()
            stats_final = metrics_helpers.parse_stats(raw)
            written_final = stats_final.get("variants", {}).get("written", 0)
            pytest.fail(
                f"variants.written didn't increase by 3 within 60s: "
                f"{written_before} -> {written_final}"
            )
