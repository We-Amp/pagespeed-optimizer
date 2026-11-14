# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Cookie-bearing requests and the shared page cache.

A page fetched for a request that carried a Cookie header is stored only
when the origin marked it explicitly shareable (`public`, or `s-maxage`),
and a stored page is served to a cookie-bearing request only when the
stored page carries the same mark.  Pages fetched without a cookie are
stored and served as before.  The rule is the HTML cache policy's
(lib/cache/html_cache_policy.h), applied in the nginx port through
src/nginx/cookie_cache_gate.h.

The origin routes live in compliance_origin.py under /cookie/<kind>/<name>:
the page names the visitor from the request's `visitor` cookie and carries a
per-fetch counter, so a page stored for one visitor and replayed to another
is visible in the body.

The Markdown copy is covered by t/111-cookie-request.t (the compliance stack
does not run agent optimization).
"""

import time
import uuid

ALICE = {"Cookie": "visitor=alice"}
BOB = {"Cookie": "visitor=bob"}
AUTH = {"Authorization": "Basic dXNlcjpwYXNz"}  # user:pass


def _fresh(kind: str) -> str:
    """A cache key no earlier test or run has touched."""
    return f"/cookie/{kind}/{uuid.uuid4().hex}"


def _requests_over(client, path, headers=None, seconds=3.0, interval=0.25):
    """Every response to repeated requests for `path` over `seconds`."""
    out = []
    deadline = time.time() + seconds
    while True:
        out.append(client.get(path, headers=headers))
        if time.time() >= deadline:
            return out
        time.sleep(interval)


class TestCookieRequestStore:
    """What is stored when the request carried a cookie."""

    def test_cookie_request_without_public_is_not_shared(self, client):
        path = _fresh("plain")
        first = client.get(path, headers=ALICE)
        assert first.status_code == 200
        assert "visitor:alice" in first.text

        for r in _requests_over(client, path):
            assert r.status_code == 200
            assert "visitor:alice" not in r.text, (
                "a page fetched for a cookie-bearing request without "
                "public or s-maxage was served to a visitor without that "
                f"cookie (X-PageSpeed: {r.headers.get('X-PageSpeed')})"
            )

    def test_cookie_request_with_public_is_shared(self, client):
        path = _fresh("public")
        first = client.get(path, headers=ALICE)
        assert "visitor:alice" in first.text

        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        assert "visitor:alice" in hit.text, (
            "a public page fetched with a cookie should be the stored copy"
        )

    def test_cookie_request_with_s_maxage_is_shared(self, client):
        path = _fresh("s-maxage")
        first = client.get(path, headers=ALICE)
        assert "visitor:alice" in first.text

        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        assert "visitor:alice" in hit.text

    def test_must_revalidate_does_not_make_a_cookie_page_shared(self, client):
        path = _fresh("must-revalidate")
        client.get(path, headers=ALICE)
        for r in _requests_over(client, path):
            assert "visitor:alice" not in r.text


class TestCookieRequestServe:
    """What a cookie-bearing request is served from the cache."""

    def test_stored_page_without_public_is_not_served_to_a_cookie(self, client):
        path = _fresh("plain")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        assert "visitor:nobody" in stored.text

        r = client.get(path, headers=BOB)
        assert r.status_code == 200
        assert r.headers.get("X-PageSpeed") != "HIT", (
            "a page stored without public or s-maxage was served to a "
            "cookie-bearing request"
        )
        assert "visitor:bob" in r.text

        # The cookie-bearing response did not replace the stored page.
        after = client.get(path)
        client.assert_hit(after)
        assert "visitor:bob" not in after.text

    def test_stored_public_page_is_served_to_a_cookie(self, client):
        path = _fresh("public")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)

        r = client.get(path, headers=BOB)
        client.assert_hit(r)
        assert r.content == stored.content

    def test_stored_s_maxage_page_is_served_to_a_cookie(self, client):
        path = _fresh("s-maxage")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)

        r = client.get(path, headers=BOB)
        client.assert_hit(r)
        assert r.content == stored.content


class TestCookieAndAuthorization:
    """The Authorization rule is unchanged; a request with both passes both."""

    def test_authorization_without_cookie_keeps_its_permits(self, client):
        # must-revalidate is an Authorization permit (RFC 9111 §3.5).
        path = _fresh("must-revalidate")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        r = client.get(path, headers=AUTH)
        client.assert_hit(r)

    def test_authorization_and_cookie_need_a_sharing_mark(self, client):
        path = _fresh("must-revalidate")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        r = client.get(path, headers={**AUTH, **BOB})
        assert r.headers.get("X-PageSpeed") != "HIT"
        assert "visitor:bob" in r.text

    def test_authorization_and_cookie_on_a_public_page(self, client):
        path = _fresh("public")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        r = client.get(path, headers={**AUTH, **BOB})
        client.assert_hit(r)
