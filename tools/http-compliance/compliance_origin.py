# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Programmable HTTP origin for compliance testing.

A full-featured Python HTTP server that supports:
- Conditional requests (If-None-Match → 304, If-Modified-Since → 304)
- HEAD method (same headers as GET, no body)
- Range requests (206 Partial Content)
- Error routes (404, 500, 502, 503)
- Per-route headers (ETag, Last-Modified, Cache-Control, Vary)
- POST/PUT/DELETE echo routes
- Chunked transfer encoding simulation
- Binary data passthrough

Routes are configured via static files in /var/www plus hardcoded
special routes for compliance testing.
"""

import hashlib
import http.server
import os
import sys
import time
from email.utils import formatdate, parsedate_to_datetime

TESTDATA_DIR = os.environ.get("COMPLIANCE_TESTDATA", "/var/www")

# Runtime switches for the /vary/flip* routes, toggled via /vary/control.
# Single-threaded server, so a plain dict is enough.
#
# "etag-rev" revises /vary/flip-etag.css's validator WITHOUT changing its
# bytes, which is how a test forces that route to answer a full 200 instead
# of a 304 -- the distinction matters because a 304 that omits `Vary` means
# "unchanged", not "cleared" (RFC 9111 Section 4.3.4).
VARY_FLIP = {"flip": False, "flip-etag": False, "etag-rev": "1"}

# Content type mapping
MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".svg": "image/svg+xml",
    ".woff2": "font/woff2",
    ".pdf": "application/pdf",
    ".dat": "application/octet-stream",
    ".txt": "text/plain; charset=utf-8",
}


def etag_for(data: bytes) -> str:
    """Generate a strong ETag from content bytes."""
    h = hashlib.md5(data).hexdigest()[:16]
    return f'"{h}"'


def weak_etag_for(data: bytes) -> str:
    """Generate a weak ETag from content bytes."""
    h = hashlib.md5(data).hexdigest()[:16]
    return f'W/"{h}"'


class ComplianceHandler(http.server.BaseHTTPRequestHandler):
    """HTTP request handler for compliance testing."""

    server_version = "ComplianceOrigin/1.0"

    # Monotonic per-process counter for /authz/stale-must-revalidate: lets
    # tests tell a stale cache serve apart from a fresh origin fetch.
    authz_stale_counter = 0

    # Same idea for /authz/anon-unchanged: a per-fetch body makes a
    # store-side overwrite of the anonymously cached entry observable.
    authz_anon_counter = 0

    def _guess_type(self, path: str) -> str:
        _, ext = os.path.splitext(path)
        return MIME_TYPES.get(ext.lower(), "application/octet-stream")

    def _read_file(self, path: str) -> bytes | None:
        filepath = os.path.join(TESTDATA_DIR, path.lstrip("/"))
        if os.path.isfile(filepath):
            with open(filepath, "rb") as f:
                return f.read()
        return None

    def _compute_validators(self, content: bytes, path: str):
        """Compute ETag and Last-Modified for content."""
        tag = etag_for(content)
        mtime = formatdate(timeval=time.time(), localtime=False, usegmt=True)
        filepath = os.path.join(TESTDATA_DIR, path.lstrip("/"))
        if os.path.isfile(filepath):
            mtime = formatdate(
                timeval=os.path.getmtime(filepath),
                localtime=False,
                usegmt=True,
            )
        return tag, mtime

    def _check_conditional(self, tag: str, mtime: str) -> bool:
        """Check If-None-Match and If-Modified-Since. Return True if 304."""
        inm = self.headers.get("If-None-Match")
        if inm:
            etags = [e.strip() for e in inm.split(",")]
            if tag in etags or "*" in etags:
                self.send_response(304)
                self.send_header("ETag", tag)
                self.send_header("Date", formatdate(usegmt=True))
                self.send_header("Cache-Control", "public, max-age=3600")
                self.end_headers()
                return True

        ims = self.headers.get("If-Modified-Since")
        if ims and not inm:
            try:
                ims_dt = parsedate_to_datetime(ims)
                mtime_dt = parsedate_to_datetime(mtime)
                if mtime_dt <= ims_dt:
                    self.send_response(304)
                    self.send_header("Last-Modified", mtime)
                    self.send_header("Date", formatdate(usegmt=True))
                    self.send_header("Cache-Control", "public, max-age=3600")
                    self.end_headers()
                    return True
            except (ValueError, TypeError):
                pass
        return False

    def _send_full_response(
        self,
        content: bytes,
        content_type: str,
        tag: str,
        mtime: str,
        send_body: bool = True,
    ):
        """Send a complete 200 response with standard headers."""
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("ETag", tag)
        self.send_header("Last-Modified", mtime)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Date", formatdate(usegmt=True))
        self.send_header("Cache-Control", "public, max-age=3600")
        self.end_headers()
        if send_body:
            self.wfile.write(content)

    def _handle_range(self, content: bytes, content_type: str) -> bool:
        """Handle Range header. Return True if 206 sent."""
        range_header = self.headers.get("Range")
        if not range_header:
            return False

        if_range = self.headers.get("If-Range")
        if if_range:
            tag = etag_for(content)
            if if_range != tag:
                return False  # Fall through to full 200

        if not range_header.startswith("bytes="):
            self.send_error(416, "Unsupported range unit")
            return True

        range_spec = range_header[6:]
        parts = range_spec.split("-")
        if len(parts) != 2:
            self.send_error(416, "Invalid range")
            return True

        total = len(content)
        try:
            if parts[0] == "":
                # Suffix range: -N (last N bytes)
                suffix_len = int(parts[1])
                start = max(0, total - suffix_len)
                end = total - 1
            elif parts[1] == "":
                # Open-ended range: N-
                start = int(parts[0])
                end = total - 1
            else:
                start = int(parts[0])
                end = int(parts[1])
        except ValueError:
            self.send_error(416, "Invalid range values")
            return True

        if start > end or start >= total:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{total}")
            self.end_headers()
            return True

        end = min(end, total - 1)
        partial = content[start : end + 1]

        self.send_response(206)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(partial)))
        self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Date", formatdate(usegmt=True))
        self.end_headers()
        self.wfile.write(partial)
        return True

    def _serve_special_route(self, path: str, send_body: bool = True) -> bool:
        """Handle special compliance test routes. Return True if handled."""
        # Error routes
        if path == "/error/404":
            self.send_error(404, "Not Found")
            return True
        if path == "/error/403":
            self.send_error(403, "Forbidden")
            return True
        if path == "/error/500":
            self.send_error(500, "Internal Server Error")
            return True
        if path == "/error/502":
            self.send_error(502, "Bad Gateway")
            return True
        if path == "/error/503":
            self.send_error(503, "Service Unavailable")
            return True

        # Redirect routes
        if path == "/redirect/301":
            self.send_response(301)
            self.send_header("Location", "/small.html")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        if path == "/redirect/302":
            self.send_response(302)
            self.send_header("Location", "/small.html")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True

        # Cache-Control test routes
        if path == "/cc/no-store":
            content = b"<html><body>no-store content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/no-cache":
            content = b"<html><body>no-cache content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/private":
            content = b"<html><body>private content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "private")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/max-age":
            content = b"<html><body>max-age content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "public, max-age=3600")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/must-revalidate":
            content = b"<html><body>must-revalidate content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "public, must-revalidate")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/max-age-must-revalidate":
            content = b"<html><body>max-age + must-revalidate content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=60, must-revalidate")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/max-age-no-cache":
            content = b"<html><body>max-age + no-cache content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-cache, max-age=60")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/s-maxage":
            content = b"<html><body>s-maxage content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "public, s-maxage=7200")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # RFC 9111 §3.5 Authorization-gate routes (test_authorization.py).
        # Each sends an ETag (like /cc/max-age-must-revalidate) so the module
        # records the entry; Cache-Control varies which §3.5 permit (if any)
        # the stored response carries.  §3.5 names exactly three permitting
        # directives: public, s-maxage, must-revalidate — bare max-age is
        # NOT a permit.
        authz_cc = {
            "/authz/no-permit": "max-age=3600",
            "/authz/public": "public, max-age=3600",
            "/authz/s-maxage": "max-age=3600, s-maxage=7200",
            "/authz/must-revalidate": "max-age=3600, must-revalidate",
        }
        if path in authz_cc:
            content = f"<html><body>authz {path}</body></html>".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", authz_cc[path])
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/authz/anon-unchanged":
            # Per-fetch counter body: every origin fetch yields a UNIQUE
            # body, so the authenticated pass-through's response is
            # distinguishable from every other fetch — if it were recorded
            # over the anonymously stored entry (a store-side §3.5
            # regression), the next anonymous HIT would serve exactly that
            # body and test_authorization.py would catch it.  A constant
            # body could not observe the overwrite.  No permit directive:
            # anonymous storage needs none.
            ComplianceHandler.authz_anon_counter += 1
            content = (
                "<html><body>authz-anon-counter:%d</body></html>"
                % ComplianceHandler.authz_anon_counter
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=3600")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/authz/stale-must-revalidate":
            # Short freshness so the test can observe staleness; the body
            # carries a monotonic counter so a stale cache serve (old
            # counter) is distinguishable from a genuine origin fetch (the
            # counter only advances when THIS handler runs).  The ETag
            # tracks the content, so a revalidation can never 304-restamp
            # the stale entry back to fresh mid-test.
            ComplianceHandler.authz_stale_counter += 1
            content = (
                "<html><body>authz-counter:%d</body></html>"
                % ComplianceHandler.authz_stale_counter
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=5, must-revalidate")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Vary-flip control: turn the origin's own Accept negotiation on or
        # off at runtime, so a test can exercise a URL that was already
        # cached (and optimized) BEFORE its origin started declaring
        # `Vary: Accept` -- the upgrade population, which a route that has
        # always declared it cannot reach.
        if path == "/vary/control":
            query = self.path.split("?", 1)[1] if "?" in self.path else ""
            params = dict(
                p.split("=", 1) for p in query.split("&") if "=" in p
            )
            for key in ("flip", "flip-etag"):
                if key in params:
                    VARY_FLIP[key] = params[key] in ("1", "on", "true")
            if "etag-rev" in params:
                VARY_FLIP["etag-rev"] = params["etag-rev"]
            body = ("%s\n" % VARY_FLIP).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(body)
            return True

        # Flip route WITHOUT validators: a stale entry here is re-fetched in
        # full (200), which is the path that carries the origin-refresh
        # signal.  Short max-age so the transition happens in test time.
        if path == "/vary/flip.css":
            content = b"body { color: red; }"
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=1")
            if VARY_FLIP["flip"]:
                self.send_header("Vary", "Accept")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Flip route WITH an ETag: a stale entry here is revalidated and
        # answered 304, which is the conditional-revalidation path.  The 304
        # carries the current `Vary` so a cache can re-decide from it
        # (RFC 9111 Section 4.3.4).
        if path == "/vary/flip-etag.css":
            content = b"body { color: blue; }"
            tag = etag_for(content + VARY_FLIP["etag-rev"].encode())
            inm = self.headers.get("If-None-Match")
            if inm and tag in inm:
                self.send_response(304)
                self.send_header("ETag", tag)
                self.send_header("Cache-Control", "max-age=1")
                if VARY_FLIP["flip-etag"]:
                    self.send_header("Vary", "Accept")
                self.send_header("Date", formatdate(usegmt=True))
                self.end_headers()
                return True
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("ETag", tag)
            self.send_header("Cache-Control", "max-age=1")
            if VARY_FLIP["flip-etag"]:
                self.send_header("Vary", "Accept")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Short-lived image WITHOUT validators, so an expired entry is
        # re-fetched in full rather than revalidated. The re-fetch path is
        # where the serving stage used to leave its own `Vary` on the
        # response before handing off to the origin -- and for an image that
        # value names `Sec-CH-DPR`, which the store-side allowlist does not
        # admit, so the response read its own header back and was refused
        # storage. Body is served from testdata so it is a real image.
        if path == "/stale-image.jpg":
            src = os.path.join(TESTDATA_DIR, "1x1.jpg")
            try:
                with open(src, "rb") as fh:
                    content = fh.read()
            except OSError:
                self.send_error(404)
                return True
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(content)))
            # max-age=2, not 1: the serve-stale grace window is
            # min(10, max_age), so a one-second lifetime leaves the test's
            # poll interval sitting on the boundary between "expired" and
            # "inside grace".  Two seconds buys margin without changing what
            # is being tested.
            self.send_header("Cache-Control", "max-age=2")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # HTML with NO Cache-Control at all. With the default html_max_age of
        # 0 this lands in the serve-with-no-cache freshness arm, which is a
        # cache-HIT serve path distinct from the ordinary fresh-hit one -- and
        # is the DEFAULT arm for HTML from an origin that sets no
        # Cache-Control, i.e. a very ordinary site.
        if path == "/no-cc.html":
            content = (
                b"<!DOCTYPE html><html><head><title>no cc</title></head>"
                b"<body><h1>no cache-control</h1></body></html>"
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Vary test route
        if path == "/vary/accept":
            content = b"body { color: red; }"
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Vary", "Accept")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Weak ETag route
        if path == "/weak-etag":
            content = b"body { color: blue; }"
            tag = weak_etag_for(content)
            # Check conditional
            inm = self.headers.get("If-None-Match")
            if inm:
                etags = [e.strip() for e in inm.split(",")]
                if tag in etags or "*" in etags:
                    self.send_response(304)
                    self.send_header("ETag", tag)
                    self.send_header("Date", formatdate(usegmt=True))
                    self.end_headers()
                    return True
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("ETag", tag)
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Chunked transfer encoding test
        if path == "/chunked/css":
            content = self._read_file("/large.css")
            if content is None:
                self.send_error(404)
                return True
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                # Send in chunks
                chunk_size = 8192
                for i in range(0, len(content), chunk_size):
                    chunk = content[i : i + chunk_size]
                    self.wfile.write(f"{len(chunk):x}\r\n".encode())
                    self.wfile.write(chunk)
                    self.wfile.write(b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
            return True

        # Hop-by-hop header test — origin sends hop-by-hop headers
        # that a proxy should strip
        if path == "/hop-by-hop":
            content = b"<html><body>hop-by-hop test</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Connection", "keep-alive")
            self.send_header("Keep-Alive", "timeout=5")
            self.send_header("Proxy-Authenticate", "Basic")
            self.send_header("Trailer", "X-Checksum")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Pragma: no-cache backward compat test
        if path == "/pragma/no-cache":
            content = b"<html><body>pragma no-cache</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Pragma", "no-cache")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Timeout simulation (slow response)
        if path == "/slow":
            time.sleep(35)  # Longer than typical proxy timeout
            content = b"slow response"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Echo route for POST/PUT/DELETE methods
        if path == "/echo":
            return False  # Handled by do_POST, do_PUT, do_DELETE

        return False

    def do_GET(self):
        path = self.path.split("?")[0]  # Strip query string

        # Check special routes first
        if self._serve_special_route(path, send_body=True):
            return

        # Serve static file
        content = self._read_file(path)
        if content is None:
            self.send_error(404, f"File not found: {path}")
            return

        content_type = self._guess_type(path)
        tag, mtime = self._compute_validators(content, path)

        # Handle conditional requests (before sending any response)
        if self._check_conditional(tag, mtime):
            return

        # Handle range requests
        if self._handle_range(content, content_type):
            return

        # Send full 200 response
        self._send_full_response(content, content_type, tag, mtime)

    def do_HEAD(self):
        path = self.path.split("?")[0]

        # Check special routes (no body)
        if self._serve_special_route(path, send_body=False):
            return

        content = self._read_file(path)
        if content is None:
            self.send_error(404, f"File not found: {path}")
            return

        content_type = self._guess_type(path)
        tag, mtime = self._compute_validators(content, path)

        # Send full response without body
        self._send_full_response(content, content_type, tag, mtime, send_body=False)

    def do_POST(self):
        self._handle_echo("POST")

    def do_PUT(self):
        self._handle_echo("PUT")

    def do_DELETE(self):
        self._handle_echo("DELETE")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS")
        self.send_header("Content-Length", "0")
        self.send_header("Date", formatdate(usegmt=True))
        self.end_headers()

    def _handle_echo(self, method: str):
        """Echo back the request method and body."""
        content_length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_length) if content_length > 0 else b""

        response = f"Method: {method}\nPath: {self.path}\nBody-Length: {len(body)}\n"
        response_bytes = response.encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(response_bytes)))
        self.send_header("Date", formatdate(usegmt=True))
        self.end_headers()
        self.wfile.write(response_bytes)

    def log_message(self, format, *args):
        """Log to stderr for Docker container visibility."""
        sys.stderr.write(f"[origin] {self.address_string()} - {format % args}\n")


def main():
    port = int(os.environ.get("PORT", "8081"))
    bind = os.environ.get("BIND", "0.0.0.0")
    server = http.server.HTTPServer((bind, port), ComplianceHandler)
    print(f"Compliance origin listening on {bind}:{port}", flush=True)
    print(f"Serving files from {TESTDATA_DIR}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
