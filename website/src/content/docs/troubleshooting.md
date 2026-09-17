---
title: 'Troubleshoot common issues'
description: 'Fix common ModPageSpeed 2.0 issues: cache misses, images not converting to WebP or AVIF, worker not processing, license warnings, and socket diagnostics.'
order: 33
group: 'Operate'
lastUpdated: 2026-09-17
---

Most ModPageSpeed 2.0 problems surface in one place: the `X-PageSpeed`
response header. `HIT` means the worker served a cached response (normally an
optimized variant); `MISS` means it served the original. Read that header first,
then jump to the symptom
below that matches what you see. Flags referenced throughout are documented in
the [configuration reference](/docs/configuration/).

## Cache Miss on Every Request

**Symptom:** Every response has `X-PageSpeed: MISS`, even for URLs that have
been requested before.

**Possible causes:**

1. **Cache file not shared.** The nginx module and worker must use the
   same cache file path. Verify that `pagespeed_cache_path` in your nginx
   config matches the worker's cache (`--cache-dir` / `--cache-path`).

   **After upgrading to 2.1 this is the first thing to check.** The worker's
   default paths moved to `/var/cache/pagespeed-optimizer/v1/cache` and
   `/run/pagespeed-optimizer/notify.sock`, but your web-server configuration
   keeps whatever it was set to. A configuration still naming the old
   `/var/lib/pagespeed-optimizer/...` paths makes the module attach to the
   abandoned cache and report the socket as **absent** — "start the daemon" —
   while the daemon is running perfectly well somewhere else. Repoint the
   directives and restart the web server.

   ```bash
   # Check nginx config
   nginx -T 2>/dev/null | grep pagespeed_cache_path

   # Check worker process
   ps aux | grep factory_worker
   ```

2. **Permissions.** The cache directory and every file in it must be owned
   by the worker's user (`pagespeed` in the packaged install), mode 0660
   owner+group — and the nginx worker user must be in group `pagespeed`.
   Check:

   ```bash
   ls -la /var/cache/pagespeed-optimizer/v1/
   id www-data   # (or apache / nginx) — must list "pagespeed"
   ```

   Fix with (the module package's postinst normally does the join):

   ```bash
   sudo usermod -a -G pagespeed www-data
   sudo systemctl restart nginx   # group membership applies on restart
   ```

3. **Memory-mapped directory not enabled.** Both processes must open the cache
   with mmap directory sharing. This is automatic in the worker and nginx
   module, but if you see persistent misses, the processes may have separate
   in-memory directories. Restart both to re-sync:

   ```bash
   sudo systemctl restart pagespeed-worker
   sudo systemctl reload nginx
   ```

4. **Cache size too small.** If the cache is full, LRU eviction removes older
   entries before they can be served. Check cache utilization via the management
   socket:

   ```bash
   echo "STATS" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
   ```

   Look at `cache.size` relative to your `--cache-size` setting. If they are
   close, increase the cache size.

## X-PageSpeed: MISS on Every Reload in Chrome

**Symptom:** In Chrome you keep seeing `X-PageSpeed: MISS` and the original
image on every reload, and you are not sure optimization is working.

**Cause:** Almost certainly nothing is wrong. Chrome DevTools open with
**Disable cache** ticked, or a hard reload (`Cmd/Ctrl+Shift+R`), both make the
browser send `Cache-Control: no-cache` on every request. ModPageSpeed honors
that by revalidating and serving the unmodified origin response
(`X-PageSpeed: MISS`) instead of a cached optimized variant. Real visitors do
not send `no-cache`, so they always get the optimized response.

**Fix:** Untick **Disable cache** (or do a normal reload) and the same image URL
returns the optimized variant with `X-PageSpeed: HIT` and a smaller
`image/webp` or `image/avif` body. From the command line, request without the
no-cache header to see the variant directly:

```bash
curl -sI http://localhost:5050/hero.jpg -H 'Accept: image/webp'
```

## Worker Not Processing Content

**Symptom:** The cache has original content (`X-PageSpeed: HIT`) but images
are not transcoded, CSS/JS are not minified, and no optimized variants appear.

**Possible causes:**

1. **Worker not running.** Verify the worker process is active:

   ```bash
   # systemd
   sudo systemctl status pagespeed-worker

   # Docker
   docker compose ps worker
   ```

2. **Socket path mismatch.** The worker writes its socket path to
   `pagespeed-shared.conf` (next to the cache file), which nginx reads
   automatically. Verify the shared config file exists and contains the
   correct socket path:

   ```bash
   # Check the shared config and socket file
   cat /var/cache/pagespeed-optimizer/v1/pagespeed-shared.conf
   ls -la /run/pagespeed-optimizer/notify.sock
   ```

3. **Socket permissions.** If the socket exists but nginx cannot connect,
   the nginx worker user is almost certainly not in group `pagespeed` (the
   socket is 0660 `pagespeed:pagespeed` by design — never world-writable).
   The worker's log distinguishes "permission denied" from "socket absent";
   the fix is the group join plus a web-server restart.

4. **Content type disabled.** The worker may have processing disabled for
   specific content types. Check for `--disable-html`, `--disable-css`,
   `--disable-js`, or `--disable-image` flags in the worker's startup command.

5. **Content too large.** The worker silently skips content exceeding size
   limits. Check worker logs for "too large" warnings:

   ```bash
   sudo journalctl -u pagespeed-worker | grep "too large"
   ```

   Increase limits with `--max-html-size`, `--max-css-size`, `--max-js-size`,
   or `--max-image-size` as needed.

## ASP.NET Core Dashboard Shows Zeros

**Symptom:** The Dashboard shows zeros and nothing moves in DevTools when using
the ASP.NET Core middleware.

**Cause:** Almost always worker coordination. On ASP.NET Core releases before
2.0.14, `Worker.SocketPath` could default to a value that left the middleware
and worker unconnected, so no optimized variants were built and the Dashboard
stayed at zero. From 2.0.14 the default is an auto-resolved per-process socket
with coordination on, so the minimal `AddPageSpeed()` + `UsePageSpeed()` setup
works with no `Worker` config.

**Fix:** Upgrade to 2.0.14+ and confirm you have not set `Worker.SocketPath` to
`null` or `""` (either disables coordination and logs a startup warning) or
`Worker.AutoStart` to `false`.

One thing that is _not_ a fault: unlike
[the 1.x `.pagespeed.` URL scheme](/pagespeed-markers/#pagespeed-url), 2.0 does not
rewrite URLs into those links — the HTML stays clean and the original URL serves
optimized bytes through content negotiation, so you will not see new asset URLs in the
page source. To verify it is working, request a content route and check for
`X-PageSpeed: HIT`:

```bash
curl -i http://localhost:5050/
```

Then confirm transcoding on the same image URL — the following should return a
much smaller `Content-Type: image/webp` response with `Vary: Accept`:

```bash
curl -sI http://localhost:5050/hero.jpg -H 'Accept: image/webp'
```

A full walkthrough is in the
[getting-started guide](/docs/aspnet-getting-started/#verify-the-install).

## Images Not Converting to WebP/AVIF

**Symptom:** Image requests with `Accept: image/webp` still return the original
JPEG or PNG.

**Possible causes:**

1. **Worker has not processed yet.** After the first request (cache miss), the
   worker optimizes asynchronously. The first few requests may serve the
   original. Wait a moment and request again.

2. **Image too large.** Images larger than `--max-image-size` (default 10 MB)
   are skipped. Check worker logs:

   ```bash
   sudo journalctl -u pagespeed-worker | grep "Image too large"
   ```

3. **Image transcoding disabled.** Verify `--disable-image` is not set.

4. **Transcoding failed.** Some images (corrupted, unusual color profiles,
   very large dimensions) may fail to transcode. Check for error messages:

   ```bash
   sudo journalctl -u pagespeed-worker | grep -i "transcode\|failed\|error"
   ```

5. **Decoded pixel buffer too large.** The worker enforces a 50 MB limit on
   decoded pixel buffers to prevent out-of-memory conditions. A 10000x10000
   RGBA image decodes to ~400 MB and will be rejected. There is no
   configuration override for this limit.

6. **Proactive variants not enabled.** Without `--proactive-image-variants`,
   the worker only produces the single format matching the notification mask.
   WebP variants are only created when a WebP-capable client triggers the
   first notification.

## Debug Logging

Enable debug-level logging to see detailed processing information:

```bash
# systemd: edit the service file
sudo systemctl edit pagespeed-worker --full
# Change --log-level info to --log-level debug

# Docker: set environment variable or modify command
docker compose exec worker factory_worker --log-level debug ...
```

Debug logging shows:

- Every notification received (URL, content type, capability mask)
- Deduplication decisions (which notifications are skipped)
- Cache reads and writes (key, size, success/failure)
- Image decode/encode details (format, dimensions, output size)
- CSS/JS minification results (original size vs. minified size)
- HTML scanning and critical CSS extraction details

For JSON log format (recommended for parsing):

```bash
factory_worker --log-format json --log-level debug
```

Parse JSON logs with `jq`:

```bash
sudo journalctl -u pagespeed-worker -o cat | jq 'select(.level == "ERROR")'
```

## Management Socket Diagnostics

The management socket provides real-time insight into the worker's state.

### Check Overall Health

```bash
echo "STATS" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
```

Look for:

- **High `errors` count** — Processing failures. Check logs for details.
- **`cache.entries` is 0** — Cache may not be opening correctly.
- **`notifications.received` is 0** — The worker is not receiving notifications
  from nginx. Check socket connectivity.
- **`variants.written` is 0 but `notifications.received` is high** — The worker
  receives notifications but fails to write variants. Check for permission
  issues or content processing errors.

### Purge and Re-test a Specific URL

To force the worker to re-process a URL:

```bash
# Purge all variants
echo "PURGE localhost /images/photo.jpg" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt

# Request the URL again (triggers a cache miss and new notification)
curl -H "Accept: image/webp,*/*" http://localhost/images/photo.jpg -o /dev/null -w "%{size_download}\n"

# Wait a moment for the worker to process, then request again
sleep 2
curl -H "Accept: image/webp,*/*" http://localhost/images/photo.jpg -o /dev/null -w "%{size_download}\n"
```

If the second request returns a smaller size, the worker is processing
correctly for that URL.

### Cannot Connect to Management Socket

If `socat` or Python fails to connect to the management socket:

```bash
# Verify the socket file exists
ls -la /var/lib/pagespeed/pagespeed.sock.mgmt

# Verify the worker is running
sudo systemctl status pagespeed-worker

# Check socket permissions
stat /var/lib/pagespeed/pagespeed.sock.mgmt
```

The management socket is created when the worker starts and removed when it
shuts down. If the socket file does not exist, the worker is not running or
failed during initialization.

## Common Error Messages

### "Failed to open cache at ..." {#failed-to-open-cache-at}

The worker cannot open or create the cache file, or refused to start. The
daemon refuses to start (loudly, naming the cause) when its cache directory
is missing, unwritable, or holds content owned by another uid — it never
chowns or migrates content itself. Read the refusal line in the journal; it
distinguishes "does not exist" from "permission denied" from
"foreign-owned". For the packaged layout, recreate the directory the
declarative way:

```bash
sudo systemd-tmpfiles --create pagespeed-optimizer.conf
```

### "Failed to bind to ..." {#failed-to-bind-to}

The Unix socket path is already in use by another process, or a stale socket
file exists from a previous crash. The worker removes stale socket files on
startup, but if another instance is running, you will see this error. Stop the
other instance first.

### "Max connections reached"

The worker is at its connection limit. This can happen under heavy load when
nginx sends many notifications simultaneously. Increase the limit:

```bash
factory_worker --max-connections 256
```

### "Client buffer exceeded max size"

A single notification is larger than `--max-buffer-size` (default 1 MB). This
typically indicates a malformed message. If you have legitimate very long URLs,
increase the buffer size.

### "URL too long"

The notification URL exceeds `--max-url-length` (default 8192 bytes). Increase
the limit if your site uses very long URLs, or consider whether the long URL
is intentional.

## Browser Analysis Issues

Browser analysis requires headless Chrome and is disabled by default. Enable it
with `--enable-browser-analysis`. These issues only apply when browser analysis
is active.

### Chrome Not Found

**Symptom:** Worker logs `Failed to start Chrome` or `chrome binary not found`
on startup with browser analysis enabled.

**Fix:** The worker looks for Chrome at the path specified by `--chrome-binary`
(default: `/usr/bin/chrome-headless-shell`). Verify the binary exists and is
executable:

```bash
ls -la /usr/bin/chrome-headless-shell
# or wherever your Chrome is installed

# If Chrome is elsewhere, specify the path:
factory_worker --enable-browser-analysis --chrome-binary /usr/bin/chromium
```

In Docker containers, install `chrome-headless-shell` or `chromium`. The
workbench-demo Docker image includes it by default.

### CDP Connection Failures

**Symptom:** Worker logs `CDP pipe read error` or `Chrome pipe EOF` during
analysis. Browser profiles are not generated.

**Possible causes:**

1. **Chrome crashed.** The worker automatically restarts Chrome after a 2-second
   delay. Check logs for the crash reason:

   ```bash
   sudo journalctl -u pagespeed-worker | grep -i "chrome\|crash\|exit"
   ```

2. **Memory limit exceeded.** If Chrome's RSS exceeds `--chrome-max-memory`
   (default 512 MB), the worker kills and restarts it. Increase the limit for
   sites with large pages:

   ```bash
   factory_worker --enable-browser-analysis --chrome-max-memory 1024
   ```

3. **Startup timeout.** Chrome may take longer to start in resource-constrained
   environments. Increase `--chrome-startup-timeout` (default 10000 ms):

   ```bash
   factory_worker --enable-browser-analysis --chrome-startup-timeout 20000
   ```

### Analysis Timeouts

**Symptom:** Worker logs `session timeout` for browser analysis. Some pages
never get browser-validated profiles.

**Fix:** The per-page timeout is controlled by `--chrome-page-timeout` (default
60000 ms). Complex pages with many stylesheets may need more time. However,
if timeouts are frequent, the root cause is often Chrome struggling with
inlined CSS volume. Check the page's stylesheet count and total CSS size.

```bash
# Check browser analysis status via management socket
echo "BROWSER-STATUS" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
```

The response includes `analysis_errors`, `chrome_crashes`, and `queue_depth`
counters.

### Browser Analysis Not Improving Results

**Symptom:** Browser analysis is enabled and running, but HTML output is
identical to heuristic-only mode.

**Possible causes:**

1. **Profile TTL too short.** If `--browser-profile-ttl` is very short, profiles
   expire before they are used. The default (86400 seconds / 24 hours) works for
   most sites.

2. **Template mismatch.** Browser profiles are keyed by DOM structure hash. If
   every page has a unique structure (e.g., inline content changes the DOM tree),
   each page gets its own profile and re-analysis runs constantly. This is normal
   for highly dynamic sites but reduces the benefit.

3. **Individual features disabled.** Check whether `--no-browser-critical-css`,
   `--no-browser-lazy-loading`, `--no-browser-lcp-preload`, or
   `--no-browser-image-sizing` flags are set. Each disables a specific browser
   analysis output.

## SVG Vectorization Issues

SVG auto-vectorization converts suitable raster images (logos, icons, flat
illustrations) to SVG format. It runs in `detect` mode by default, which only
evaluates candidacy without producing SVG output. Set `--svg-mode auto` for
production serving.

### No SVG Variants Produced

**Symptom:** The worker processes images but no SVG variants appear in the cache.

**Possible causes:**

1. **SVG mode is `detect` (the default).** In detect mode, the worker evaluates
   images for SVG candidacy and logs scores, but does not vectorize. Set
   `--svg-mode auto` or `--svg-mode preview` to produce SVG output:

   ```bash
   factory_worker --cache-path /data/cache.vol --svg-mode auto
   ```

2. **Candidacy threshold too high.** The `--svg-candidacy-threshold` (default 50)
   filters out images with low vectorization suitability. Photos and complex
   textures score low and are rejected. This is by design -- SVG is only
   beneficial for simple graphics. Lower the threshold to see more candidates:

   ```bash
   factory_worker --svg-candidacy-threshold 30
   ```

3. **Images too large.** The `--svg-max-pixels` flag (default 65536, about 256x256)
   limits which images are evaluated. Large photos are excluded because
   vectorization produces enormous SVGs. Increase for larger icons:

   ```bash
   factory_worker --svg-max-pixels 262144  # ~512x512
   ```

4. **Image processing disabled.** If `--disable-image` is set, all image
   processing is skipped, including SVG vectorization.

### SVG Variants Larger Than Raster

**Symptom:** Debug logs show `svg_size_rejected` counter increasing. SVGs are
produced but discarded.

This is the size gate working correctly. If the vectorized SVG is larger than
the raster original (which is common for photos and complex images), the SVG
variant is discarded. The `svg_bytes_saved` stat shows cumulative savings for
SVGs that did pass the gate.

### SVG Path Count Exceeded

**Symptom:** Debug logs show `svg_path_count_rejected` counter increasing.

Complex images produce SVGs with many `<path>` elements, which can slow down
browser rendering. The `--svg-max-paths` flag (default 500) limits the maximum
path count. If you want to allow more complex SVGs:

```bash
factory_worker --svg-max-paths 1000
```

Be cautious: SVGs with thousands of paths can cause rendering jank on mobile
devices.

### LCP Images Not Vectorized

**Symptom:** The LCP hero image qualifies for SVG but no SVG variant is produced.

By default, `--svg-exclude-lcp true` skips vectorization for images identified
as the Largest Contentful Paint candidate. SVG path tessellation in the browser
can be slower than decoding a raster image, potentially regressing LCP.

If your LCP image is a simple logo or icon that renders quickly as SVG:

```bash
factory_worker --svg-exclude-lcp false
```

## Vary Header and Cache Poisoning

**Symptom:** All responses are cache misses. The cache never populates even
though nginx and the worker are running correctly.

**Cause:** The interceptor adds a `Vary: User-Agent` token to mark device-aware
responses. In builds before **2.0.16**, that token could be evaluated as part of
the response's own cacheability check, causing every response to be treated as
uncacheable.

**Fix:** Upgrade to **2.0.16 or newer**, where this is resolved — confirm with
the `X-PageSpeed` response header. If the symptom persists after upgrading,
[contact support](/contact/) with your worker version and a sample response's
headers.

## Next Steps

- [Configuration Reference](/docs/configuration/) — All worker flags and tuning
  options
- [API Reference](/docs/api-reference/) — Protocol details for management socket
  and IPC
- [HTTP API Reference](/docs/http-api/) — REST and WebSocket endpoints for
  programmatic access
- [Web Console](/docs/workbench/) — Visual cache inspector, debug console, and
  real-time metrics
- [Deployment Guide](/docs/deployment/) — Production setup and monitoring
