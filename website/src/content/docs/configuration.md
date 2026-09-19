---
title: 'ModPageSpeed 2.0 configuration reference'
description: 'Reference for ModPageSpeed 2.0 nginx directives, worker flags, and cache tuning. Looking for the mod_pagespeed 1.15 reference? See the native module configuration directives section below.'
order: 20
group: 'Configure'
lastUpdated: 2026-09-19
faq:
  - q: 'What is the difference between safe and aggressive cache modes?'
    a: 'Safe mode adds `must-revalidate` to non-HTML responses, suppresses stale-while-revalidate synthesis, and strips `immutable`. Aggressive mode adds `public` and `stale-if-error=86400` and allows SWR synthesis. HTML always gets `no-cache` in both modes.'
  - q: 'How do I disable PageSpeed for specific URLs?'
    a: 'Use `pagespeed off` inside a `location` block to disable the module for that path, or `pagespeed_disallow` to skip individual URL patterns (prefix, suffix, or substring) while keeping the module active elsewhere.'
  - q: 'How do I share the cache file between nginx and the worker?'
    a: 'Point `pagespeed_cache_path` (nginx) and the worker cache at the same file (the packaged daemon defaults to `--cache-dir /var/cache/pagespeed-optimizer/v1`). The daemon owns the directory and every file in it (0660 `pagespeed:pagespeed`); the nginx worker user reaches them through membership in group `pagespeed`. Memory-mapped directory sharing is enabled automatically.'
  - q: 'What does the capability mask do?'
    a: 'The 32-bit mask encodes the client image format, viewport, pixel density, Save-Data, and transfer encoding into the cache key, so different optimized variants are served to different clients. Nginx derives the mask from request headers and client hints.'
  - q: 'How large should the Cyclone cache be?'
    a: 'Small blog or portfolio sites need 256 MB–1 GB (the `--cache-size` default is 1 GB); medium sites (~1,000 pages) need 1–2 GB; large sites (10,000+ pages) need 2–4 GB; and image-heavy sites need 4–8 GB, because each source image can produce up to 37 variants when all proactive flags are enabled. See the sizing table for details.'
---

ModPageSpeed 2.0 has a minimal configuration surface: two required nginx
directives (`pagespeed on;` and `pagespeed_cache_path`), a shared config file,
and worker command-line flags. This page is the full reference for all of them,
plus cache tuning and the capability mask format. Everything past the two
required directives is optional tuning.

> **New to ModPageSpeed?** This is the configuration reference. If you are just
> getting oriented, start with the [product overview](/) or
> [Getting Started](/docs/getting-started/), then come back here once it is
> installed. To install first, see [Install with Docker](/docs/installation-docker/)
> or [Install the nginx module](/docs/installation-module/).

To work out which of these directives your own pages actually need, run them
through a [PageSpeed Insights test](/analyze/): every failing audit is mapped to
the transform that fixes it, so the report doubles as a configuration checklist.

## Nginx Directives

These directives are available in `http`, `server`, and `location` contexts.
Values set at a higher level are inherited by nested blocks.

### `pagespeed`

Enables or disables the PageSpeed module.

```nginx
pagespeed on;   # Enable
pagespeed off;  # Disable (default)
```

**Context:** `http`, `server`, `location`
**Default:** `off`

When enabled, the module intercepts responses, checks the cache for optimized
variants, and adds the `X-PageSpeed` response header (`HIT` or `MISS`).

### `pagespeed_cache_mode`

Controls how Cache-Control headers are assembled on optimized responses.

```nginx
pagespeed_cache_mode safe;        # default — short TTLs, must-revalidate
pagespeed_cache_mode aggressive;  # long TTLs, public, stale-if-error
```

**Context:** `http`, `server`, `location`
**Default:** `safe`

In safe mode, `must-revalidate` is added to all non-HTML responses, SWR
synthesis is suppressed, and `immutable` is stripped from transformed content.
In aggressive mode, `public` and `stale-if-error=86400` are added, and SWR
synthesis is allowed. HTML always gets `no-cache` in both modes.

The per-type max-age defaults change based on the active mode: CSS/JS defaults
to 300s (safe) or 86400s (aggressive); images default to 1800s (safe) or
86400s (aggressive).

See [Cache Modes](/docs/cache-modes/) for the full reference.

### `pagespeed_cache_path`

Path to the Cyclone cache volume file.

```nginx
pagespeed_cache_path /var/lib/pagespeed/cache.vol;
```

**Context:** `http`, `server`, `location`
**Default:** (empty — caching disabled if not set)

This must point to the same file used by the worker's `--cache-path`
flag. Both processes open the file with memory-mapped directory sharing enabled,
so writes from either process are immediately visible to the other.

The cache file is created automatically (by the worker) if it doesn't exist.
For cross-process sharing to work, the file must be readable and writable by
both nginx worker processes and the worker. The packaged daemon takes care of
this: it runs as the unprivileged `pagespeed` user and sets every shared file
explicitly to 0660 owner+group, so the only setup the web side needs is
membership in group `pagespeed` (the module package's postinst adds it).

### `pagespeed_disallow`

Exclude URLs from caching and optimization. Multiple directives allowed; first
match wins. Supports prefix, suffix, and substring patterns.

```nginx
pagespeed_disallow /api/;       # Prefix match
pagespeed_disallow *.woff2;     # Suffix match
pagespeed_disallow admin;       # Substring match
```

**Context:** `http`, `server`, `location`

### `pagespeed_hot_threshold`

Number of fallback-hits before a URL is considered "hot" and triggers a warmup
notification to the worker. Only relevant when `--enable-warmup` is set on the
worker.

```nginx
pagespeed_hot_threshold 5;   # default
```

**Context:** `http`, `server`, `location`
**Default:** `5`

### `pagespeed_trust_x_forwarded_proto`

Tells PageSpeed to read the `X-Forwarded-Proto` header to determine the request
scheme (`http` or `https`) instead of relying on connection-level SSL detection.

```nginx
pagespeed_trust_x_forwarded_proto on;
```

**Context:** `http`, `server`, `location`
**Default:** `off`

Use this when nginx is behind a TLS-terminating reverse proxy, load balancer, or
CDN that sets `X-Forwarded-Proto`. Only lowercase `http` and `https` header
values are accepted; any other value falls back to connection-level SSL
detection.

:::caution
Only enable this when the upstream proxy strips and re-sets the
`X-Forwarded-Proto` header. If clients can set this header directly, they can
manipulate scheme detection.
:::

**Example — upstream proxy that terminates TLS:**

```nginx
# Upstream nginx (TLS termination)
server {
    listen 443 ssl;
    location / {
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_pass http://backend;
    }
}

# Backend nginx (PageSpeed)
server {
    listen 80;
    pagespeed on;
    pagespeed_trust_x_forwarded_proto on;
    pagespeed_cache_path /var/lib/pagespeed/cache.vol;
}
```

### Cache-Control Directives

The following directives control how the interceptor sets `Cache-Control` and
`Age` headers on cache-served responses. See the
[Cache Control guide](/docs/cache-control/) for detailed behavior.

| Directive                            | Default                              | Description                                                                                                                           |
| ------------------------------------ | ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------- |
| `pagespeed_max_age`                  | `86400`                              | Cap on origin max-age (seconds)                                                                                                       |
| `pagespeed_immutable_max_age`        | `604800`                             | Cap for `Cache-Control: immutable` content                                                                                            |
| `pagespeed_html_max_age`             | `0`                                  | Default max-age for HTML when origin sends no Cache-Control                                                                           |
| `pagespeed_css_max_age`              | `300`                                | Default max-age for CSS/JS when origin sends no Cache-Control                                                                         |
| `pagespeed_image_max_age`            | `1800` (safe) / `86400` (aggressive) | Default max-age for images when origin sends no Cache-Control. Default changes based on [`pagespeed_cache_mode`](/docs/cache-modes/). |
| `pagespeed_synthesize_swr`           | `on`                                 | Synthesize `stale-while-revalidate` on HIT responses                                                                                  |
| `pagespeed_conditional_revalidation` | `on`                                 | Enable conditional revalidation (If-None-Match / If-Modified-Since)                                                                   |
| `pagespeed_force_refresh_html`       | `on`                                 | Force revalidation on browser force-refresh (Ctrl+F5) for HTML                                                                        |
| `pagespeed_force_refresh`            | `off`                                | Force revalidation on browser force-refresh (Ctrl+F5) for all non-HTML types                                                          |

## Shared Configuration File

The worker writes a `pagespeed-shared.conf` file next to the cache file
(in the parent directory of `--cache-path`). Nginx reads this file automatically
using `pagespeed_cache_path` to locate it. The file is polled every ~1 second
for changes.

**Format:** `key=value` (one per line)

**Fields:**

| Key            | Description                                            | Worker flag         |
| -------------- | ------------------------------------------------------ | ------------------- |
| `socket_path`           | Unix socket path for nginx-to-worker notifications   | `--socket PATH`     |
| `disable_html`          | Whether HTML optimization is disabled (`true`/`false`) | `--disable-html`  |
| `cache_dir_generation`  | Cache-directory generation N (skew = loud handshake failure) | (compiled in) |

The file is written atomically at mode 0640 `pagespeed:pagespeed`: readable
by the worker and its group-`pagespeed` peers, not by the world.

The shared config file is written on worker startup and whenever settings change
via `PATCH /v1/config`. This eliminates the need to duplicate configuration
between the nginx config and worker flags.

On a cache miss, nginx reads the `socket_path` from the shared config and sends
a fire-and-forget notification to the worker. If the socket is not available
(worker not running, socket not yet created), the notification is silently
dropped. The original content continues to be served from the cache.

## Worker Command-Line Flags

The `factory_worker` binary accepts these flags:

### `--cache-dir DIR`

Directory the cache volume, shared config, and serve-stats live in.

```bash
factory_worker --cache-dir /var/cache/pagespeed-optimizer/v1
```

**Default:** `/var/cache/pagespeed-optimizer/v<N>` where N is the compiled-in
cache-directory generation (currently 1). The directory must exist, be
writable by the worker, and contain only files owned by the worker's user —
the worker refuses to start (loudly, naming the cause) otherwise. The
packaged tmpfiles.d drop-in creates the default as 3770
`pagespeed:pagespeed` (setgid so the group is inherited, sticky so one group
member cannot remove another's files).

### `--cache-path PATH`

Expert override: full path to the Cyclone cache volume file stem
(deliberately extensionless). Wins over `--cache-dir` when both are given.

```bash
factory_worker --cache-path /var/cache/pagespeed-optimizer/v1/cache
```

Must match the `pagespeed_cache_path` nginx directive. The worker reads original
content from this file and writes optimized variants back to it.

### `--socket PATH`

Unix socket path for receiving notifications from nginx.

```bash
factory_worker --socket /run/pagespeed-optimizer/notify.sock
```

**Default:** `/run/pagespeed-optimizer/notify.sock`

The socket path is shared with nginx automatically via `pagespeed-shared.conf`
(no nginx directive needed). The socket is created by the worker on startup
with an explicit 0660 owner+group mode — never umask-derived — so the web
side reaches it through group `pagespeed` membership, not world permissions.
Ensure the directory exists and is writable by the worker's user (the
packaged unit uses a systemd `RuntimeDirectory`, 0750 `pagespeed:pagespeed`).

### `--cache-size BYTES`

Maximum cache size in bytes.

```bash
factory_worker --cache-size 536870912  # 512 MB
```

**Default:** `1073741824` (1 GB)

This controls the total size of the Cyclone cache volume. The cache uses LRU
eviction when full — least recently accessed entries are removed to make room
for new content.

### `--read-lease-duration MS`

Duration of the per-region read lease stamped by cache reads, in milliseconds.
While a lease is live, the cache defers overwriting that region so in-flight
zero-copy responses are never served corrupted bytes.

```bash
factory_worker --read-lease-duration 0  # disable read leases
```

**Default:** `5000`

Set to `0` to disable read leases entirely (writes are never deferred). On a
full cache under sustained read traffic, leases can delay new writes to a hot
region; lowering this value (or disabling it) favors write throughput over
zero-copy read protection.

### `--lease-wrap-ceiling MS`

Upper bound on how long consecutive writes to a region can be deferred by read
leases before a write is forced through, in milliseconds.

```bash
factory_worker --lease-wrap-ceiling 30000
```

**Default:** `60000`

Only relevant when read leases are enabled. Lower values favor write admission
at a full cache; responses that hold cache content longer than this ceiling
fall back to copying.

### `--help`

Print usage information and exit.

### `--version`

Print the ModPageSpeed version and exit.

### `--max-connections N`

Maximum number of simultaneous client connections.

```bash
factory_worker --max-connections 256
```

**Default:** `128`

When the limit is reached, new connections are accepted and immediately closed.
Monitor the health check endpoint to track connection usage.

### `--max-buffer-size BYTES`

Maximum per-client buffer size for incoming notifications.

```bash
factory_worker --max-buffer-size 2097152  # 2 MB
```

**Default:** `1048576` (1 MB)

Connections exceeding this buffer size are disconnected to prevent memory
exhaustion from malformed or excessively large messages.

### `--connection-timeout MS`

Idle connection timeout in milliseconds.

```bash
factory_worker --connection-timeout 60000  # 60 seconds
```

**Default:** `30000` (30 seconds)

Connections that don't send data within this timeout are closed.

### `--shutdown-timeout MS`

Graceful shutdown timeout in milliseconds.

```bash
factory_worker --shutdown-timeout 10000  # 10 seconds
```

**Default:** `5000` (5 seconds)

On SIGTERM/SIGINT, the worker stops accepting new connections and waits up to
this duration for active connections to drain before force-closing them.

### `--disable-html`

Disable HTML optimization (critical CSS injection). HTML notifications from
nginx are silently ignored.

### `--disable-css`

Disable CSS minification. CSS notifications are silently ignored.

### `--disable-js`

Disable JavaScript minification. JS notifications are silently ignored.

### `--disable-image`

Disable image transcoding. Image notifications are silently ignored.

### `--max-url-length BYTES`

Maximum URL length accepted in notifications.

**Default:** `8192`

Notifications with URLs longer than this are rejected.

### `--max-html-size BYTES`

Maximum HTML content size for processing.

**Default:** `5242880` (5 MB)

HTML documents larger than this are skipped.

### `--max-css-size BYTES`

Maximum CSS content size for processing.

**Default:** `2097152` (2 MB)

### `--max-js-size BYTES`

Maximum JavaScript content size for processing.

**Default:** `2097152` (2 MB)

### `--max-image-size BYTES`

Maximum image content size for processing.

**Default:** `10485760` (10 MB)

### `--log-level LEVEL`

Minimum log level. Messages below this level are suppressed.

```bash
factory_worker --log-level warning
```

**Default:** `info`

**Values:** `debug`, `info`, `warning`, `error`

### `--log-format FORMAT`

Log output format.

```bash
factory_worker --log-format json
```

**Default:** `text`

**Values:**

- `text` — Human-readable: `[INFO] message text`
- `json` — Machine-parseable: `{"timestamp":"...","level":"INFO","message":"..."}`

JSON format is recommended for production environments with log aggregation.

## Proactive Variant Generation

When the worker receives an image notification, it can generate multiple
variants from a single decode pass. This avoids decoding the same image
repeatedly for different client types. Each dimension can be independently
enabled or disabled.

### `--proactive-image-variants`

Enable multi-format generation. When set, a single notification generates
WebP, AVIF, and an optimized original from one decode pass. Without this flag
(the default), the worker produces only the single format requested by the
notification (e.g., only WebP).

### `--no-proactive-viewport-variants`

Disable viewport sibling generation. When set, the worker only generates
variants for the viewport in the notification (e.g., only Mobile). Without
this flag (the default), variants for all three viewport classes
(Mobile/Tablet/Desktop) are generated from one decode, resized to each
viewport's target width.

### `--no-proactive-savedata-variants`

Disable Save-Data sibling generation. When set, the worker only generates
variants matching the notification's Save-Data preference. Without this flag
(the default), both normal-quality and Save-Data (lower-quality) variants are
generated from each decode pass.

Save-Data variants use lower compression quality to reduce bandwidth:

| Format | Normal Quality | Save-Data Quality                    |
| ------ | -------------- | ------------------------------------ |
| JPEG   | 85             | 60                                   |
| WebP   | 75             | 50                                   |
| AVIF   | 60             | 45                                   |

These quality levels can be overridden at runtime with CLI flags (see
[Image Quality Flags](#image-quality-flags) below).

### `--no-proactive-density-variants`

Disable pixel density sibling generation. When set, the worker only generates
variants for the pixel density in the notification (1x or 2x+). Without this
flag (the default), variants for both 1x and 2x+ density are generated from
each decode pass.

For 2x+ density, the viewport target width is doubled before resizing. For
example, a Mobile/2x variant uses a target width of 960px instead of 480px,
providing sharper images for Retina displays.

### Variant Matrix

With all proactive flags enabled (the default), a single image notification
can produce up to:

- 3 formats (WebP, AVIF, optimized original)
- 3 viewports (Mobile, Tablet, Desktop)
- 2 Save-Data states (off, on)
- 2 pixel densities (1x, 2x+)

That is up to **37 variants** (36 raster plus 1 SVG for eligible images) from a single decode pass. In practice, many of
these already exist in the cache and are skipped, so the actual number of new
writes is typically much smaller.

### `--enable-warmup`

Enable hot URL variant warmup. When nginx detects a URL exceeding the hot
threshold (`pagespeed_hot_threshold`), it sends a warmup sentinel to the worker.
With warmup enabled, the worker pre-generates all missing variants for that URL.
Without this flag (the default), warmup notifications are ignored.

### Viewport Resize Widths

Control the target width for viewport-based image resizing. Images wider than
the target are downscaled (preserving aspect ratio). Images already smaller
than the target are not resized.

```bash
factory_worker --mobile-width 480 --tablet-width 768 --desktop-width 0
```

| Flag              | Default | Description                                     |
| ----------------- | ------- | ----------------------------------------------- |
| `--mobile-width`  | `480`   | Target width for mobile viewport (0=disable)    |
| `--tablet-width`  | `768`   | Target width for tablet viewport (0=disable)    |
| `--desktop-width` | `0`     | Target width for desktop viewport (0=no resize) |

## URL Normalization

### `--strip-query-extensions LIST`

Strip query strings from URLs with specified file extensions. Comma-separated
list. This helps consolidate cache entries for static assets that vary only
by cache-busting query parameters.

```bash
factory_worker --strip-query-extensions css,js,png,jpg,webp
```

### `--strip-query-groups LIST`

Strip query strings by group name. Comma-separated list.

### `--strip-query-params LIST`

Strip specific named query parameters from URLs. Comma-separated list.

```bash
factory_worker --strip-query-params utm_source,utm_medium,fbclid
```

### `--host-alias SRC=DST`

Map one hostname to another for cache key purposes. Requests for `SRC` are
treated as `DST` in cache lookups. Multiple `--host-alias` flags allowed.

```bash
factory_worker --host-alias www.example.com=example.com
```

### `--allow-private-urls`

Allow capture and waterfall requests for private/loopback URLs. Off by default
for security. Enable only in development or when the worker needs to analyze
sites on private networks.

## Image Quality Flags

Override the default image encoding quality at runtime. These flags apply to
all image transcoding performed by the worker.

### Normal Quality

| Flag             | Default | Range | Description                          |
| ---------------- | ------- | ----- | ------------------------------------ |
| `--jpeg-quality` | `85`    | 1-100 | JPEG output quality                  |
| `--webp-quality` | `75`    | 0-100 | WebP output quality                  |
| `--avif-quality` | `60`    | 0-100 | AVIF output quality                  |

### Save-Data Quality

These are used when the request includes `Save-Data: on`:

| Flag                      | Default | Range | Description            |
| ------------------------- | ------- | ----- | ---------------------- |
| `--savedata-jpeg-quality` | `60`    | 1-100 | Save-Data JPEG quality |
| `--savedata-webp-quality` | `50`    | 0-100 | Save-Data WebP quality |
| `--savedata-avif-quality` | `45`    | 0-100 | Save-Data AVIF quality |

### Example

```bash
factory_worker --cache-path /data/cache.vol \
  --jpeg-quality 80 --webp-quality 70 --avif-quality 55 \
  --savedata-jpeg-quality 55 --savedata-webp-quality 40 --savedata-avif-quality 40
```

## Learned Quality Prediction

Per-format ML models predict the optimal encoder quality parameter for a target
SSIMULACRA2 score. The models are trained LightGBM decision trees compiled to C
at build time — zero runtime dependencies, ~5 microsecond inference per format.

When enabled, the worker extracts 16 image features (edge density, noise level,
color entropy, spatial frequency, luminance, etc.) from the decoded pixels and
predicts the encoder quality that hits the configured `target_ssimulacra2` score.
SSIMULACRA2 verification still runs as a safety net: if the predicted quality
produces a score outside the tolerance band, the worker re-encodes.

When disabled or when a model returns an invalid prediction (e.g., the AVIF model
is not yet trained), the worker falls back to the existing heuristic quality
calculation based on content class and base quality.

| Flag                         | Default   | Description                                      |
| ---------------------------- | --------- | ------------------------------------------------ |
| `--no-learned-quality`       | (enabled) | Disable learned quality prediction (all formats) |
| `--no-learned-quality-jpeg`  | (enabled) | Disable for JPEG only                            |
| `--no-learned-quality-webp`  | (enabled) | Disable for WebP only                            |
| `--no-learned-quality-avif`  | (enabled) | Disable for AVIF only                            |
| `--savedata-score-reduction` | `15`      | SSIMULACRA2 points to subtract for Save-Data     |

### Example

```bash
# Disable learned quality for AVIF (stub model), keep JPEG and WebP
factory_worker --cache-path /data/cache.vol --no-learned-quality-avif

# Disable all learned quality, fall back to heuristic
factory_worker --cache-path /data/cache.vol --no-learned-quality
```

## SSIMULACRA2 Quality Tuning

The worker uses SSIMULACRA2 perceptual quality scoring to verify that encoded
images meet a target visual quality. These flags control the target score and
the tolerance band around it.

| Flag                      | Default | Range | Description                                 |
| ------------------------- | ------- | ----- | ------------------------------------------- |
| `--target-ssimulacra2`    | `70`    | 0-100 | Target SSIMULACRA2 score for encoded images |
| `--ssimulacra2-tolerance` | `5.0`   | float | Acceptable deviation from target            |
| `--no-quality-verify`     | (on)    | flag  | Disable SSIMULACRA2 verification entirely   |

When verification is enabled (the default), the worker encodes at the predicted
or configured quality level, then measures the actual SSIMULACRA2 score. If the
score falls outside the asymmetric tolerance band
`[target - 0.6*tolerance, target + 1.6*tolerance]`, the worker re-encodes with
adjusted quality.

Disabling verification (`--no-quality-verify`) skips the SSIMULACRA2 measurement
pass entirely, reducing CPU cost at the risk of inconsistent visual quality.

## Content Analysis and Denoising

The worker classifies image content (photo, screenshot, illustration, noisy) and
applies content-aware encoding presets. A bilateral filter denoises images that
exceed the noise threshold before encoding, improving compression efficiency.

| Flag                      | Default | Range    | Description                                 |
| ------------------------- | ------- | -------- | ------------------------------------------- |
| `--no-content-analysis`   | (on)    | flag     | Disable content classification              |
| `--denoise-threshold`     | `0.3`   | 0.0-1.0  | Noise level threshold (0=disable denoising) |
| `--denoise-sigma-spatial` | `3.0`   | 0.0-10.0 | Bilateral filter spatial sigma              |
| `--denoise-sigma-range`   | `25.0`  | float    | Bilateral filter intensity range sigma      |

Content analysis is fast (runs on the already-decoded pixel buffer) and feeds
into both the learned quality model and the denoising decision. Disabling it
falls back to the `Unknown` content class for all images.

## HTML Optimization Flags

These flags control individual HTML transforms applied by the worker. All are
enabled by default.

| Flag                         | Default   | Description                                               |
| ---------------------------- | --------- | --------------------------------------------------------- |
| `--no-lazy-load-images`      | (enabled) | Disable `loading="lazy"` injection on images/iframes      |
| `--no-image-dimensions`      | (enabled) | Disable `width`/`height` injection from cached headers    |
| `--no-lcp-preload`           | (enabled) | Disable `<link rel="preload">` for LCP images             |
| `--no-preconnect-injection`  | (enabled) | Disable preconnect hints for third-party origins          |
| `--enable-speculation-rules` | (off)     | Enable speculation rules injection                        |
| `--no-async-css`             | (enabled, per-page conditions apply) | Disable async CSS loading outright. Enabled does not mean applied: see [Async CSS](#async-css) for the per-page conditions deferral requires |
| `--no-script-deferral`       | (enabled) | Disable automatic script deferral                         |
| `--no-css-import-flattening` | (enabled) | Disable CSS `@import` inlining during processing          |

Lazy loading skips the LCP candidate image (which gets `fetchpriority="high"`
instead), the first 3 body images when an LCP candidate is identified, and the
first iframe in the document body (typically an above-the-fold video or media
embed). Invisible images — 1x1 tracking pixels, `hidden` or `display:none`
elements — are left untouched entirely: never promoted, never lazy-loaded.

LCP preload injects a `<link rel="preload" as="image" fetchpriority="high">`
tag in the `<head>` and writes the URL to the Early Hints cache sentinel, so
nginx can emit `103 Early Hints` or `Link` response headers on subsequent
requests.

Preconnect injection detects third-party origins from external resources in the
HTML and writes `preconnect:` hints to the Early Hints sentinel. The hint
carries `crossorigin` exactly when the resource that motivated it is fetched in
CORS mode (fonts, `crossorigin`-marked resources, ES modules), so the warmed
connection is the one the browser actually reuses.

Async CSS loading makes render-blocking `<link rel="stylesheet">` tags
non-blocking. Each link is switched to `rel="preload" as="style"` so it
downloads at normal priority without blocking first paint; a small same-origin
loader script turns it back into a stylesheet, with its original media, once
the sheet has loaded. Because it is a standard preload, the deferred stylesheet
is also announced in early hints. The loader is served at a content-hashed path under
`/pagespeed_static/` (for example `/pagespeed_static/async_css.<hash>.js`; the
hash changes whenever the loader script changes, so the front-end serves it with
an immutable, year-long cache and a loader update lands as a fresh URL). It is
referenced with an external `<script defer src>`, not an inline `onload`
handler, so it works under a strict `script-src 'self'` Content-Security-Policy.
A `<noscript>` copy
preserves the stylesheet for clients without JavaScript. The inlined critical CSS
renders the page above the fold while the full sheet loads.

Because a stylesheet deferred behind an above-the-fold block that does not
actually cover the fold would flash unstyled content, this transform activates
only for a page whose above-the-fold appearance has been confirmed unchanged
with the stylesheet deferred, and only while that page still serves the exact
stylesheet the confirmation was made against — publish new styles and the check
runs again before deferral resumes. Everywhere the confirmation does not apply,
including a page whose browser analysis has not completed, the stylesheet stays
render-blocking and the above-the-fold CSS is still inlined. Disable the
transform entirely with `--no-async-css`.

Under a nonce-only or `strict-dynamic` CSP, host allowlists including `'self'`
are ignored, so the loader is blocked and deferred sheets stay non-applied for
JavaScript-enabled clients — turn async CSS off (`--no-async-css`) on such sites.

Script deferral adds the `defer` attribute to `<script src="...">` tags that
browser script analysis has identified as safe to defer. Scripts already marked
with `async`, `defer`, or `type="module"` are left unchanged. The worker uses
path-boundary suffix matching to map analysis results to script URLs across
pages. Disable with `--no-script-deferral`.

## Transforms

Per-transform reference. Each entry names the 2.0 control surface and the
matching 1.15 filter(s), so operators migrating from 1.15 can find the new
knob and `/analyze` filter chips can deep-link to a specific transform.

Toggleable transforms expose a `factory_worker --no-<name>` flag and are
enabled by default. Always-on transforms run from the master switch
(`pagespeed on;`) and cannot be turned off individually; the only way to
suppress them is the coarse `--disable-html` / `--disable-css` /
`--disable-js` / `--disable-image` family or per-location `pagespeed off;`.

### Image pipeline {#image-pipeline}

Decodes source images, applies content-aware quality and denoising, and
re-encodes to modern formats (WebP, AVIF). Emits viewport, density, and
Save-Data variants. Metadata (EXIF, ICC profiles other than sRGB) is
stripped. Always-on baseline under `pagespeed on;`; there is no
per-transform off switch. Use `--disable-image` to skip the image pipeline
entirely, or [`pagespeed_disallow`](#pagespeed_disallow) to exclude
specific URL patterns. The proactive variant family is tuned via the
[Proactive Variant Generation](#proactive-variant-generation) flags.
(1.15 equivalent: `rewrite_images`, `convert_jpeg_to_webp`,
`convert_to_webp_lossless`, `recompress_jpeg`, `recompress_png`,
`recompress_webp`, `resize_images`, `resize_rendered_image_dimensions`,
`responsive_images`, `jpeg_sampling`, `strip_image_meta_data`.)

### Image dimensions {#image-dimensions}

Adds `width` and `height` attributes to `<img>` tags that are missing
them, reserving layout space before the image loads and improving CLS.
Dimensions are read from the worker's cached image headers. Toggleable
via [`--no-image-dimensions`](#html-optimization-flags). When
[browser analysis](#browser-analysis) is enabled, rendered dimensions
from headless Chrome take precedence over decoded pixel dimensions.
(1.15 equivalent: `insert_image_dimensions`.)

### Lazy load images {#lazy-load-images}

Adds `loading="lazy"` to images and iframes that are below the
predicted fold. The LCP candidate is excluded (it gets
`fetchpriority="high"` instead), the first three body images are
skipped when an LCP candidate is identified, and the first iframe in
the document body is skipped, to avoid demoting above-the-fold
content. Invisible images (1x1 tracking pixels, hidden elements) are
never promoted and never lazy-loaded. Toggleable via
[`--no-lazy-load-images`](#html-optimization-flags). (1.15 equivalent:
`lazyload_images`.)

### LCP preload {#lcp-preload}

Injects `<link rel="preload" as="image" fetchpriority="high">` in
`<head>` for the predicted LCP image and writes the URL to the Early
Hints cache sentinel, so nginx can emit `103 Early Hints` or `Link`
response headers on subsequent requests. Toggleable via
[`--no-lcp-preload`](#html-optimization-flags). (1.15 equivalent:
`hint_preload_subresources`.)

### Preconnect injection {#preconnect-injection}

Detects third-party origins referenced from external resources in the
HTML and writes `preconnect:` hints to the Early Hints sentinel, so
browsers can open TCP and TLS connections in parallel with the HTML
download. The hint carries `crossorigin` when the motivating resource
fetches in CORS mode (fonts, `crossorigin`-marked resources, ES
modules), matching the connection pool the browser will reuse.
Toggleable via
[`--no-preconnect-injection`](#html-optimization-flags). (1.15
equivalent: `insert_dns_prefetch`.)

### Critical CSS {#critical-css}

Extracts the CSS rules used by above-the-fold content and inlines them
in a `<style>` tag in `<head>`, so the browser can render the visible
viewport without waiting for external stylesheets. Always-on under
`pagespeed on;` (it is part of the HTML optimization pipeline disabled
only by `--disable-html`). When [browser analysis](#browser-analysis)
is enabled the critical set is derived from the CSS Coverage API at
three viewport sizes; otherwise a heuristic computes it from cached
stylesheet structure. Used together with [async CSS](#async-css) so the
remaining stylesheets stop blocking render. (1.15 equivalent:
`prioritize_critical_css`.)

> **Gotcha — critical-CSS inlining can break dark mode.** Critical-CSS
> extraction analyses the page in its rendered state at extraction time. That
> render happens without a `.dark` class on `<html>` (or whatever toggles your
> dark theme), so the extractor never sees the `.dark` / `dark:` selectors and
> never inlines them as critical CSS. Any dark-mode style on an above-the-fold
> element then flashes the light value (or a missing element) until the full
> stylesheet finishes loading. The fix is to add an inline dark-mode override in
> the page `<head>` — a small `<style>` block that hard-sets the dark values for
> critical-render-path elements, for example:
>
> ```html
> <style>
>   html.dark {
>     background: #0c0a09;
>   }
> </style>
> ```
>
> Add one override per dark-mode utility that lands on a critical element
> (background, header/nav colors, logo `dark:hidden`/`dark:block` swaps, card
> backgrounds). This is required whenever you ship both critical-CSS inlining and
> a class-based dark theme.

### Async CSS {#async-css}

Makes render-blocking `<link rel="stylesheet">` tags non-blocking by switching
each to `rel="preload" as="style"` and turning it back into a stylesheet, with
its real media, once it loads — driven by a
CSP-safe external loader served at a content-hashed path under
`/pagespeed_static/` (for example `/pagespeed_static/async_css.<hash>.js`, no
inline `onload` handler, so it works under `script-src 'self'`). A `<noscript>`
fallback preserves the stylesheet for clients without JavaScript. The inlined
critical CSS paints above the fold while the full sheet loads. Activates only
for a page whose above-the-fold appearance has been confirmed unchanged with the
stylesheet deferred, against the stylesheet it is currently serving; otherwise
the stylesheet stays render-blocking and the above-the-fold CSS is still
inlined. Toggleable via
[`--no-async-css`](#html-optimization-flags). (1.15 equivalent:
`move_css_to_head`, `move_css_above_scripts`.)

### CSS import flattening {#css-import-flattening}

Inlines `@import` chains so the browser does not have to discover and
fetch each stylesheet sequentially. Flattening happens during CSS
processing and the resulting stylesheet is cached and served as a
single resource. Toggleable via
[`--no-css-import-flattening`](#html-optimization-flags). (1.15
equivalent: `flatten_css_imports`.)

### Script deferral {#script-deferral}

Adds the `defer` attribute to `<script src="...">` tags that script
coverage analysis has identified as safe to defer, so they execute
after HTML parsing instead of blocking it. Scripts already marked
`async`, `defer`, or `type="module"` are left unchanged. Toggleable via
[`--no-script-deferral`](#html-optimization-flags). (1.15 equivalent:
`defer_javascript`.)

### CSS minification {#css-minification}

Parses CSS and emits a minified form (whitespace and comment removal,
shorthand collapsing where safe). The output is content-hashed and
served via [cache extension](#cache-extension). Always-on under
`pagespeed on;`; use `--disable-css` to skip CSS rewriting entirely.
(1.15 equivalent: `rewrite_css`.)

### JS minification {#js-minification}

Parses JavaScript and emits a minified form (whitespace, identifier
shortening within safe scopes, dead-code removal where statically
provable). The output is content-hashed and served via
[cache extension](#cache-extension). Always-on under `pagespeed on;`;
use `--disable-js` to skip JS rewriting entirely. (1.15 equivalent:
`rewrite_javascript`.)

### Cache extension {#cache-extension}

Rewrites references to static assets (CSS, JS, images, fonts) so they
point at content-hashed URLs the module serves with a long
`Cache-Control: max-age`. Because the URL changes when the byte content
changes, the long TTL is safe: a new deploy invalidates the old URL by
producing a new hash, and there is no need to purge intermediate
caches. Always-on under `pagespeed on;`. The TTL cap is controlled by
[`pagespeed_max_age`](#cache-control-directives) and the per-type
`pagespeed_*_max_age` directives. (1.15 equivalent: `extend_cache`,
`extend_cache_css`, `extend_cache_images`, `extend_cache_scripts`,
`extend_cache_pdfs`.)

### HTML caching {#html-caching}

Caches HTML responses in the Cyclone shared-memory cache and serves
them zero-copy from the memory-mapped file on cache HIT. This masks
slow origin TTFB on repeat visits. The first request to a URL still
hits origin, and the origin's `Cache-Control` headers decide whether
HTML is eligible. `no-store` is always respected (the response
is not cached at all). For origins that send no `Cache-Control` on
HTML, set [`pagespeed_html_max_age N;`](#cache-control-directives) to
opt that origin in; for origins that do send `Cache-Control: public,
max-age=N`, HTML caching honours the origin's max-age (capped by
[`pagespeed_max_age`](#cache-control-directives)).

The `Cache-Control` header on the optimized HTML response is always
`no-cache` regardless of cache mode, so downstream browsers and CDNs
revalidate on every navigation. Conditional revalidation
([`pagespeed_conditional_revalidation on`](#cache-control-directives),
the default) keeps that cheap by answering with `304 Not Modified` when
the cached body still matches. HTML caching is unique to 2.0; 1.15
always passes HTML through to the origin and rewrites in flight.

## Browser Analysis

Browser analysis uses headless Chrome to generate per-template optimization
profiles. It replaces heuristic-based critical CSS extraction with
browser-validated results from the CSS Coverage API, and produces accurate
above-the-fold detection, LCP identification, and image dimension data.

Browser analysis is disabled by default. Enable it with `--enable-browser-analysis`.
Chrome (or `chrome-headless-shell`) must be available at the configured binary path.
The flags below are the config-reference summary; the
[Browser Analysis guide](/docs/browser-analysis/) covers the CDP pipeline,
template profiling, and tuning in depth.

### Browser Analysis Flags

| Flag                           | Default                          | Description                                              |
| ------------------------------ | -------------------------------- | -------------------------------------------------------- |
| `--enable-browser-analysis`    | (off)                            | Enable headless Chrome analysis                          |
| `--chrome-binary`              | `/usr/bin/chrome-headless-shell` | Path to Chrome or chrome-headless-shell binary           |
| `--chrome-recycle-interval`    | `100`                            | Pages processed before recycling Chrome (memory control) |
| `--chrome-page-timeout`        | `60000`                          | Per-page CDP timeout in milliseconds                     |
| `--chrome-max-memory`          | `512`                            | Chrome RSS threshold in MB (auto-kill above this)        |
| `--chrome-startup-timeout`     | `10000`                          | Chrome startup timeout in milliseconds                   |
| `--no-browser-critical-css`    | (enabled)                        | Disable browser-validated critical CSS (use heuristic)   |
| `--no-browser-lazy-loading`    | (enabled)                        | Disable browser fold detection for lazy loading          |
| `--no-browser-lcp-preload`     | (enabled)                        | Disable browser-based LCP detection                      |
| `--no-browser-image-sizing`    | (enabled)                        | Disable browser-based image dimension detection          |
| `--no-browser-script-analysis` | (enabled)                        | Disable browser-based script coverage analysis           |
| `--browser-queue-size`         | `1000`                           | Maximum pending analysis queue items                     |
| `--browser-profile-ttl`        | `86400`                          | Profile cache expiry in seconds (24 hours default)       |

### How It Works

When browser analysis is enabled, the worker spawns headless Chrome with
`--remote-debugging-pipe` and communicates over CDP (Chrome DevTools Protocol).
For each unique HTML template (identified by DOM structure hash), the worker:

1. Inlines cached stylesheets into the HTML (so Chrome can compute real CSS coverage)
2. Runs CSS Coverage API at three viewport sizes (Mobile 375x667, Tablet 768x1024, Desktop 1440x900)
3. Extracts above-the-fold detection, LCP candidates, and rendered image dimensions
4. Stores the result as an optimization profile in the cache

Subsequent HTML processing for pages matching the same template structure uses
the cached profile instead of heuristics. Profiles expire after
`--browser-profile-ttl` seconds.

### Chrome Memory Management

Chrome is recycled after `--chrome-recycle-interval` pages to prevent memory
growth. On Linux, the worker monitors Chrome's RSS via `/proc/pid/status` and
kills the process if it exceeds `--chrome-max-memory` MB. On other platforms,
RSS monitoring is not available and only page-count recycling applies.

### Fallback Behavior

All browser analysis failures fall back to the heuristic path. If Chrome fails
to start, crashes, or times out, the worker logs the error and continues with
heuristic-based optimization. Browser analysis is strictly additive -- it never
blocks or degrades the base optimization pipeline.

### Example

```bash
# Enable browser analysis with a custom Chrome path
factory_worker --cache-path /data/cache.vol \
  --enable-browser-analysis \
  --chrome-binary /usr/bin/chromium \
  --chrome-max-memory 768 \
  --chrome-page-timeout 30000
```

## HTTP Management API

The worker embeds an HTTP/1.1 server for programmatic access and the web console.
It is **off** until you enable a transport.

| Flag                 | Default                                 | Description                                                        |
| -------------------- | --------------------------------------- | ------------------------------------------------------------------ |
| `--api-socket [PATH]`| (off; path `/run/pagespeed-optimizer/api.sock`) | Serve the API over a unix socket, mode 0660 — the recommended local transport |
| `--api-port`         | `0`                                     | HTTP API TCP port (0 = disabled)                                    |
| `--api-bind`         | `127.0.0.1`                             | Bind address for the TCP API                                        |
| `--api-allow-remote` | `false`                                 | Confirm a deliberate non-loopback bind                              |
| `--api-no-auth`      | `false`                                 | Confirm a deliberate tokenless API on loopback or the unix socket   |
| `--api-token`        | (none)                                  | Bearer token for auth (prefer the `PAGESPEED_API_TOKEN` env var)    |
| `--api-read-open`    | `false`                                 | Allow unauthenticated GET + WebSocket access                        |
| `--console-dir`      | (none)                                  | Path to the web console SPA directory                               |

### One invariant, enforced at startup

**Remote is never unauthenticated, and unauthenticated is never remote.** The
worker checks this while parsing its configuration and refuses to start on any
combination that breaks it — with a message naming the flag that would make the
requested posture legal.

| Transport            | Token   | Extra flag           | Result                             |
| -------------------- | ------- | -------------------- | ---------------------------------- |
| unix socket          | either  | —                    | normal — no token is asked for     |
| TCP loopback         | present | —                    | normal                             |
| TCP loopback         | absent  | `--api-no-auth`      | allowed, WARNING banner at startup |
| TCP loopback         | absent  | —                    | **refuses to start**               |
| TCP non-loopback     | present | `--api-allow-remote` | allowed, WARNING banner            |
| TCP non-loopback     | present | —                    | **refuses to start**               |
| TCP non-loopback     | absent  | any                  | **refuses to start**               |

Two more refusals, both at startup: `--api-socket` and `--api-port` together
(the worker serves exactly one transport), and an `--api-bind` that is not an
IPv4 literal — the API binds IPv4 only, so `localhost`, `::1` and other IPv6
forms are rejected by name rather than left to fail the bind later.

With a token configured, every endpoint except `/v1/health` requires an
`Authorization: Bearer <token>` header. `--api-read-open` opens GET requests and
WebSocket streams while keeping mutating operations behind the token — useful
for a public demo dashboard. It is an explicit choice: an absent token no longer
opens reads for you.

### The unix socket is the recommended local transport

`--api-socket` serves the same HTTP API over
`/run/pagespeed-optimizer/api.sock`, mode 0660 `pagespeed:pagespeed`. Filesystem
permission is the credential for local peers, so nothing has to be provisioned
to your web server and nothing has to be rotated — and the worker binds no TCP
port at all, so "listens on localhost only" is something `ss -ltn` can prove.

**A connection accepted on that socket is an authenticated peer.** Reaching it
means already being in group `pagespeed` — the same boundary that governs the
cache volume — so the worker asks for no bearer token on top, including for
cache purge. A token configured alongside it (the packaged installer always
provisions one, for the TCP case) changes nothing here. That removal is the
entire reason to prefer the socket over `127.0.0.1` plus a token.

```bash
# Local: no port, no credential to distribute.
factory_worker --cache-dir /var/cache/pagespeed-optimizer/v1 \
  --api-socket \
  --console-dir /opt/pagespeed/console

# Remote, deliberately: both flags AND a token, or it will not start.
PAGESPEED_API_TOKEN="$(cat /run/secrets/api-token)" \
factory_worker --cache-dir /var/cache/pagespeed-optimizer/v1 \
  --api-port 9880 --api-bind 0.0.0.0 --api-allow-remote \
  --console-dir /opt/pagespeed/console
```

Prefer `PAGESPEED_API_TOKEN` over `--api-token`: anything on the command line is
readable by any local user through `/proc/<pid>/cmdline`. Package installs
generate a token into `/etc/pagespeed-optimizer/daemon.env` (mode 0640
`root:pagespeed`) for you, along with `PAGESPEED_PURGE_TOKEN` for cache
invalidation over the management socket.

## Headless browser sandbox

Browser analysis runs headless Chrome over untrusted page content, so Chrome's
own sandbox must be on. Chrome's namespace sandbox needs an unprivileged user
namespace and a non-root user; `--browser-sandbox` decides what happens when it
cannot have one.

| Value               | Behaviour                                                                                                                                                  |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `require` (default) | The worker probes at startup. If the sandbox is available, Chrome runs sandboxed. If it is not, **browser analysis refuses to start**, logs one message naming the cause and the remedy, and the worker keeps serving without it. It never falls back to an unsandboxed browser. |
| `off`               | Deliberate opt-out. Chrome runs unsandboxed, a warning is logged at startup and on every browser start.                                                     |

There is no `auto`. `GET /v1/health` reports the resolved state as
`browser_sandbox`: `"on"`, `"unavailable"`, `"off"` or `"disabled"` — so a
monitoring check can see an unsandboxed browser without reading logs. The
setting is also readable from the environment as `PAGESPEED_BROWSER_SANDBOX`.

Container images currently run as root, where Chrome declines to sandbox
itself; set `PAGESPEED_BROWSER_SANDBOX=off` if you want browser analysis there,
or run the container as a non-root user.

If the sandbox probe is refused by a kernel syscall filter rather than by the
kernel's user-namespace policy, the message says so, so you can tell "this host
denies user namespaces" from "the syscall filter this service runs under is too
tight for Chrome".

### Syscall filter visibility

`GET /v1/health` and `GET /v1/stats` also report `syscall_filter`, read from
the kernel's view of the running process:

| Value        | Meaning                                                            |
| ------------ | ------------------------------------------------------------------ |
| `"filtered"` | A kernel syscall filter is attached to the worker process.         |
| `"none"`     | No filter is attached.                                             |
| `"unknown"`  | The kernel does not report a filter state this worker can read.    |

**`"filtered"` means *a* kernel syscall filter is attached — not that any
particular hardening profile is in force.** A container runtime's default
seccomp profile counts, so a container started with stock settings reports
`"filtered"` without you configuring anything; a package install on a host
reports `"none"` today. Read the field as "is this process confined at all",
not as "is my intended profile applied".

This is read-only reporting, not a setting: the worker does not install the
filter. On a host the service manager applies whatever the unit specifies,
before the worker starts; in a container the container runtime's profile
applies; started by hand, the worker is unfiltered and says `"none"`. There is
no flag, because a flag naming a control the process does not own would be a
flag that lies.

## Threading

| Flag               | Default | Description                                                    |
| ------------------ | ------- | -------------------------------------------------------------- |
| `--num-threads`    | auto    | Thread pool size for notification processing (2-128)           |
| `--ram-cache-size` | auto    | RAM cache size in bytes (default: 6% of cache-size, 16-256 MB) |

The default thread count is based on available CPU cores. The RAM cache uses
write-around semantics: writes go directly to disk, reads populate the RAM
cache on miss. This prevents cross-process staling between nginx and the worker.

## Pre-Compressed Text Variants

The worker produces gzip and brotli pre-compressed alternates for all text
resources (HTML, CSS, JS) after optimization. This eliminates dynamic
compression CPU cost on cache HITs -- nginx serves the pre-compressed
alternate directly with the correct `Content-Encoding` header.

Bits 6-7 of the capability mask encode the transfer encoding:

| Value | Encoding | Description                |
| ----- | -------- | -------------------------- |
| `00`  | Identity | Uncompressed (fallback)    |
| `01`  | Gzip     | Pre-compressed with gzip   |
| `10`  | Brotli   | Pre-compressed with brotli |
| `11`  | Reserved | Reserved for future use    |

The default capability mask is `0x08` (Desktop, Identity encoding).

| Flag             | Default | Range | Description              |
| ---------------- | ------- | ----- | ------------------------ |
| `--gzip-level`   | `6`     | 1-9   | Gzip compression level   |
| `--brotli-level` | `6`     | 1-11  | Brotli compression level |

Images are always stored with identity encoding (compressed image formats
do not benefit from additional gzip/brotli compression).

### Example

```bash
factory_worker --cache-path /data/cache.vol \
  --gzip-level 6 --brotli-level 6
```

## SVG Auto-Vectorization

Format slot `11` (in the capability mask) is used for SVG auto-vectorization.

When the worker processes an image notification, it evaluates the raster source
for SVG candidacy before running the standard raster transcode loop. Suitable
images (simple graphics, logos, icons) are vectorized using VTracer, a Rust-based
raster-to-SVG engine integrated via FFI. A size gate ensures the resulting SVG
is smaller than the raster original; if it is not, the SVG variant is discarded.

SVG variants are resolution-independent -- a single variant serves all viewport
classes and pixel densities. The worker stores the SVG at a fixed capability mask
(`kSvg`, Desktop, 1x, Save-Data off, Identity encoding) and the cache selector
gives SVG a universal format bonus during alternate selection.

If a browser sends `image/jxl` in the `Accept` header, it maps to Original
format (no special handling). The JXL format slot in the capability mask is
fully repurposed for SVG and is never set from client headers.

### SVG Operational Modes

The `--svg-mode` flag controls how far the SVG pipeline runs:

| Mode      | Behavior                                                           |
| --------- | ------------------------------------------------------------------ |
| `detect`  | Evaluate candidacy and log scores. No vectorization. **(default)** |
| `preview` | Evaluate, vectorize, and store SVG. Not served to clients.         |
| `auto`    | Full pipeline: evaluate, vectorize, store, and serve.              |

Start with `detect` to see which images qualify, then move to `preview` for
inspection, and finally `auto` for production serving.

### SVG Flags

| Flag                        | Default  | Range     | Description                                          |
| --------------------------- | -------- | --------- | ---------------------------------------------------- |
| `--svg-mode`                | `detect` | see above | Operational mode                                     |
| `--svg-candidacy-threshold` | `50`     | 0-100     | Minimum candidacy score for vectorization            |
| `--svg-max-pixels`          | `65536`  | 1-16M     | Max decoded pixel count (width x height)             |
| `--svg-max-paths`           | `500`    | 1-100000  | Max `<path>` elements in output SVG                  |
| `--svg-max-svg-bytes`       | `262144` | 1K-16M    | Max uncompressed SVG output size in bytes            |
| `--svg-fidelity-threshold`  | `55.0`   | 0-100     | Minimum SSIMULACRA2 fidelity score                   |
| `--svg-exclude-lcp`         | `true`   | bool      | Skip vectorization for LCP candidate images          |
| `--svg-timeout-ms`          | `500`    | 10-60000  | Vectorization timeout per image                      |
| `--svg-preset`              | `1`      | 0-2       | VTracer preset (0=bw, 1=poster, 2=photo)             |
| `--svg-color-precision`     | `0`      | 0-8       | Color quantization (0=adaptive, 1-8=fixed bit depth) |
| `--svg-filter-speckle`      | `4`      | 0-1000    | Minimum cluster area in pixels (noise filter)        |

LCP exclusion (`--svg-exclude-lcp true`) is enabled by default because SVG
render cost (path tessellation in the browser) can regress LCP timing for
hero images. Override with `--svg-exclude-lcp false` if your LCP images are
simple graphics that benefit from vectorization.

All SVG settings are hot-reloadable via `PATCH /v1/config` on the HTTP API.

### Example

```bash
# Enable full SVG pipeline with stricter candidacy
factory_worker --cache-path /data/cache.vol \
  --svg-mode auto --svg-candidacy-threshold 65 --svg-max-paths 300
```

## Management Socket

The worker exposes a management socket at `{socket_path}.mgmt` (e.g.,
`/var/lib/pagespeed/pagespeed.sock.mgmt`). This socket accepts newline-terminated
text commands and returns a response before closing the connection.

### Connecting to the Management Socket

```bash
# Using socat
echo "STATS" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt

# Using Python
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX)
s.connect('/var/lib/pagespeed/pagespeed.sock.mgmt')
s.sendall(b'STATS\n')
print(s.recv(4096).decode())
s.close()
"
```

### `STATS` Command

Returns a JSON object with worker statistics:

```json
{
  "status": "ok",
  "connections": { "active": 2, "max": 128 },
  "notifications": { "received": 1542, "skipped_dedup": 380 },
  "variants": { "written": 986, "proactive": 724 },
  "errors": 3,
  "cache": { "entries": 2048, "size": 134217728 },
  "by_type": {
    "html": { "n": 45, "us": 120000 },
    "css": { "n": 112, "us": 85000 },
    "js": { "n": 98, "us": 72000 },
    "images": { "n": 731, "us": 9500000 }
  },
  "by_format": { "webp": 320, "avif": 280, "jpeg": 85, "png": 46 },
  "timing_us": { "total": 9777000 }
}
```

The `us` fields are cumulative processing time in microseconds. Divide by
the count (`n`) to get the average processing time per item.

### `PURGE` Command

Invalidates all cached variants for a URL. The command format is `PURGE`
followed by the hostname and URL:

```bash
echo "PURGE example.com /images/photo.jpg" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
```

**Response:** `OK N entries deleted\n` where N is the number of cache entries
removed. The PURGE command iterates all possible capability mask combinations
for the URL and deletes every matching entry, including sentinel entries used
for Early Hints and warmup.

After a PURGE, the next request for that URL will be a cache miss. Nginx will
proxy to the origin, re-cache the response, and send a new notification to the
worker.

### Cache Invalidation

The PURGE command is the primary mechanism for cache invalidation. Common
use cases:

- **Content update:** When you deploy new CSS, JS, or images, purge the
  affected URLs so the worker re-optimizes from the updated originals.
- **Debugging:** Purge a URL to force a fresh optimization pass and observe
  the worker's processing logs.
- **Selective cache clearing:** Unlike restarting the worker or deleting the
  cache file (which loses everything), PURGE targets individual URLs.

For bulk invalidation, send multiple PURGE commands. Each command opens a
new connection to the management socket:

```bash
for url in /style.css /app.js /hero.jpg; do
  echo "PURGE example.com $url" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
done
```

## Capability Mask

ModPageSpeed classifies each request into a 32-bit capability mask based on
the client's capabilities. This mask is used as part of the cache key, allowing
different optimized variants to be served to different clients.

### Bitmask Layout

| Bits | Field          | Values                                               |
| ---- | -------------- | ---------------------------------------------------- |
| 0-1  | Image Format   | `00` Original, `01` WebP, `10` AVIF, `11` SVG        |
| 2-3  | Viewport Class | `00` Mobile, `01` Tablet, `10` Desktop               |
| 4    | Pixel Density  | `0` 1x, `1` 2x+ (Retina)                             |
| 5    | Save-Data      | `0` off, `1` on                                      |
| 6-7  | Transfer Enc.  | `00` Identity, `01` Gzip, `10` Brotli, `11` Reserved |

### How Classification Works

The nginx module determines these values from request headers:

- **Image Format:** Parsed from the `Accept` header. If the client advertises
  `image/webp`, WebP variants are preferred. Similarly for `image/avif`.
- **Viewport Class:** Derived from the `Sec-CH-Viewport-Width` client hint or
  `User-Agent` heuristics (mobile vs. desktop).
- **Pixel Density:** From `Sec-CH-DPR` client hint or device heuristics.
- **Save-Data:** From the `Save-Data: on` request header.
- **Transfer Encoding:** From the `Accept-Encoding` header (br > gzip > identity).

### Cache Key Format

Variants are stored as alternates within a `SHA-256(URL, hostname)` cache key.
The capability mask determines which alternate a client receives. For example,
a WebP-capable desktop client with brotli requesting `/style.css` gets a different
variant than a mobile client with gzip requesting the same URL.

### Variant Fallback

When an exact match isn't found in the cache, nginx tries progressively
degraded fallback masks before falling back to mask `0x08` (the default for
original content stored at Desktop/Identity). This means a client always
gets a response — either the optimized variant, a close variant, or the
original.

## Content Types

The worker processes these content types when notified by nginx:

| Type           | Optimizations Applied                                                                             |
| -------------- | ------------------------------------------------------------------------------------------------- |
| **Images**     | Format transcoding (JPEG/PNG/GIF to WebP/AVIF), lossless PNG reduction, quality-aware compression |
| **CSS**        | Minification (whitespace, comments, redundant syntax removal)                                     |
| **JavaScript** | Minification (whitespace, comments)                                                               |
| **HTML**       | Critical CSS extraction and inline injection                                                      |

The worker only writes an optimized variant if it is smaller than the original.
If minification doesn't reduce the size, the original is kept and no variant
is written.

## Cross-Process Cache Sharing

Both nginx and the worker access the same Cyclone cache file. For this
to work correctly:

1. **Same file path** — `pagespeed_cache_path` and `--cache-path` must point to
   the same file.
2. **Permissions** — The worker owns the cache directory and sets every
   shared file explicitly to 0660 owner+group; nginx workers must be in group
   `pagespeed` (the module package's postinst adds them). Nothing is
   world-accessible anymore, and no correct setup depends on umask.
3. **Memory-mapped directory** — Both processes open the cache with mmap
   directory sharing enabled (this is automatic). Without it, each process would
   have its own in-memory directory and writes would be invisible to the other.

## Example: Complete Configuration

### nginx.conf

```nginx
worker_processes auto;
error_log /var/log/nginx/error.log warn;
pid /var/run/nginx.pid;

load_module /usr/lib/nginx/modules/ngx_pagespeed_module.so;

events {
    worker_connections 1024;
}

http {
    include       /etc/nginx/mime.types;
    default_type  application/octet-stream;

    server {
        listen 80;
        server_name example.com;

        pagespeed on;
        pagespeed_cache_path /var/lib/pagespeed/cache.vol;
        # Worker socket and HTML toggle are read
        # automatically from pagespeed-shared.conf

        location / {
            proxy_pass http://127.0.0.1:8081;
            proxy_set_header Host $host;
            proxy_set_header X-Real-IP $remote_addr;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        }
    }
}
```

### Worker systemd unit

```ini
[Unit]
Description=ModPageSpeed Factory Worker
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/factory_worker \
  --socket /run/pagespeed-optimizer/notify.sock \
  --cache-dir /var/cache/pagespeed-optimizer/v1 \
  --cache-size 536870912 \
  --log-format json \
  --log-level info
User=pagespeed
Group=pagespeed
UMask=0007
RuntimeDirectory=pagespeed-optimizer
RuntimeDirectoryMode=0750
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

(The packaged unit — `deploy/pagespeed-optimizer.service` — adds the full
hardening set: empty `CapabilityBoundingSet`, `ProtectSystem=strict`,
`ProtectHome=yes`, `PrivateTmp=yes`, `NoNewPrivileges=yes`, and a secrets
`EnvironmentFile`. Use it as the reference.)

## Response Headers

ModPageSpeed adds the following response header:

| Header        | Value  | Meaning                                                  |
| ------------- | ------ | -------------------------------------------------------- |
| `X-PageSpeed` | `HIT`  | Content served from cache (may be original or optimized) |
| `X-PageSpeed` | `MISS` | Cache miss — proxied to origin, response cached          |

A `HIT` response may contain original content if the worker
hasn't processed it yet. The worker runs asynchronously, so there's a brief
window after the first request where the cache contains the original. Subsequent
requests will get the optimized version once the worker has written it.

## Sizing the Cache

The cache size depends on your site's content:

| Site Type                  | Recommended Cache Size |
| -------------------------- | ---------------------- |
| Small blog / portfolio     | 256 MB - 1 GB          |
| Medium site (1,000 pages)  | 1 - 2 GB               |
| Large site (10,000+ pages) | 2 - 4 GB               |
| Image-heavy site           | 4 - 8 GB               |

The `--cache-size` default is 1 GB, which suits most small and medium sites.

The cache stores both original and optimized variants. Image-heavy sites need
more space because each image may have multiple variants (WebP, AVIF, different
viewport sizes, Save-Data quality levels, and pixel density variants).

With all proactive variant generation enabled, a single image can produce up to
37 distinct optimized variants (36 raster plus 1 SVG for eligible images). The
HTTP API's PURGE command iterates a larger capability-mask space (up to 192
combinations, since several masks — including the transfer-encoding axis — map
to the same stored entry); the 37 figure counts the distinct cached outputs.
Size the cache generously for image-heavy sites.

Monitor cache effectiveness by checking the ratio of `HIT` to `MISS` responses
in your nginx access logs, or use the management socket `STATS` command to
inspect cache entry counts and size.

## Disabling PageSpeed Per-Location

You can enable PageSpeed at the server level and disable it for specific paths:

```nginx
server {
    pagespeed on;
    pagespeed_cache_path /var/lib/pagespeed/cache.vol;

    location / {
        proxy_pass http://127.0.0.1:8081;
    }

    # Don't optimize API responses
    location /api/ {
        pagespeed off;
        proxy_pass http://127.0.0.1:8081;
    }

    # Don't optimize admin panel
    location /admin/ {
        pagespeed off;
        proxy_pass http://127.0.0.1:8081;
    }
}
```

## URL Pattern Exclusions

The `pagespeed_disallow` directive excludes URL patterns from optimization.
Unlike `pagespeed off` (which disables the entire module for a location block),
`pagespeed_disallow` selectively skips individual URL patterns while keeping
the module active.

```nginx
server {
    pagespeed on;
    pagespeed_cache_path /var/lib/pagespeed/cache.vol;

    # Skip font files
    pagespeed_disallow *.woff2;
    pagespeed_disallow *.woff;

    # Skip API endpoints
    pagespeed_disallow /api/;

    # Skip admin panel
    pagespeed_disallow /admin/;

    location / {
        proxy_pass http://127.0.0.1:8081;
    }
}
```

**Context:** `http`, `server`, `location`

**Pattern matching:**

- Patterns starting with `/` match URL prefixes: `/api/` matches `/api/v1/users`
- Patterns starting with `*` match URL suffixes: `*.woff2` matches `/fonts/main.woff2`
- Other patterns match as substrings: `admin` matches `/site/admin/panel`

Multiple `pagespeed_disallow` directives can be specified. They are checked in
order; the first match causes the request to bypass PageSpeed.

## Enabling the native module

These steps load and enable the in-process module (Apache, nginx, IIS); the reverse-proxy worker documented above needs none of them.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

Load the module and enable it in your server block:

```nginx
load_module modules/ngx_pagespeed_module.so;

http {
    server {
        pagespeed on;

        # Required location blocks
        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
        }

        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon$" { }
    }
}
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

The distribution package loads the module automatically on install. Enable it in `pagespeed.conf` or your virtual host configuration:

```apache
ModPagespeed on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

The installer registers the module in IIS automatically. Configure it through `pagespeed.config`, a flat-text configuration file with one directive per line.

The module looks for configuration files in this order:

1. **Site-level:** `pagespeed.config` in the website root directory
2. **Server-level:** `%ProgramData%\We-Amp\PageSpeed\pagespeed.config`

Site-level settings override server-level settings. If `pagespeed.config` is not found, the module falls back to `iiswebspeed.config`, the filename used by IISpeed installs.

Enable optimization by adding to your `pagespeed.config`:

```
pagespeed on
```

</div>

### Envoy

The native module is available as an HTTP filter for Envoy. This integration is experimental. [Contact us](/contact/) for configuration guidance.

## Native module states

The in-process module (Apache, nginx, IIS) supports three operational states:

| State       | Behavior                                                                                                                                                                                                  |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `on`        | Full optimization. The module rewrites HTML and serves optimized resources.                                                                                                                               |
| `standby`   | Serves previously optimized `.pagespeed.` resources and responds to query-parameter requests, but does not optimize new traffic. Use this to drain optimized resources before fully disabling the module. |
| `unplugged` | Fully disabled. The module does not intercept any requests. This state can only be set at the top level or within a virtual host block, not in directory-level configuration.                             |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed standby;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeed standby
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed standby
```

</div>

<a id="configuration-directives"></a>

## Native module configuration directives {#native-module-configuration-directives}

These directives configure the in-process module (Apache, nginx, IIS) rather
than the reverse-proxy worker documented above. For the complete list, see the [directive index](/docs/directive-index/).

In v1.15.0+r18 and later, configuration validation is stricter: out-of-range values for bounded options (image quality levels, progressive JPEG scan counts, `HttpCacheCompressionLevel`, `RewriteRandomDropPercentage`, `CentralControllerPort`) fail configuration load instead of being silently accepted, and an invalid filter name in `?PageSpeedFilters=` rejects the whole query. In addition, the `AddResourceHeader` limit of 20 headers is now enforced exactly, and directive and option-scope matching is case-consistent. A configuration that loaded on an earlier revision may need its values corrected when upgrading.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

In nginx, prefix each directive with `pagespeed` and terminate with a semicolon.

```nginx
pagespeed HonorCsp on;
pagespeed RespectVary on;
pagespeed DisableRewriteOnNoTransform on;
pagespeed LowercaseHtmlNames off;
pagespeed ModifyCachingHeaders on;
pagespeed XHeaderValue "Powered by mod_pagespeed";
pagespeed PreserveUrlRelativity off;
pagespeed StaticAssetPrefix "/pagespeed_static/";
pagespeed AddResourceHeader "X-Custom" "value";
pagespeed ListOutstandingUrlsOnError off;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

In Apache, prefix each directive with `ModPagespeed`.

```apache
ModPagespeedHonorCsp on
ModPagespeedRespectVary on
ModPagespeedDisableRewriteOnNoTransform on
ModPagespeedLowercaseHtmlNames off
ModPagespeedModifyCachingHeaders on
ModPagespeedXHeaderValue "Powered by mod_pagespeed"
ModPagespeedPreserveUrlRelativity off
ModPagespeedStaticAssetPrefix "/pagespeed_static/"
ModPagespeedAddResourceHeader "X-Custom" "value"
ModPagespeedListOutstandingUrlsOnError off
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

In IIS, use `pagespeed.config` with one directive per line. Prefix each directive with `pagespeed`.

```
pagespeed HonorCsp on
pagespeed RespectVary on
pagespeed DisableRewriteOnNoTransform on
pagespeed LowercaseHtmlNames off
pagespeed ModifyCachingHeaders on
pagespeed XHeaderValue "Powered by mod_pagespeed"
pagespeed PreserveUrlRelativity off
pagespeed StaticAssetPrefix "/pagespeed_static/"
pagespeed AddResourceHeader "X-Custom" "value"
pagespeed ListOutstandingUrlsOnError off
```

The `pagespeed.config` format also accepts `ModPagespeed` and `iispeed` as directive prefixes for compatibility. The `pagespeed` prefix is recommended.

</div>

| Directive                     | Default              | Description                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ----------------------------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `HonorCsp`                    | `on`                 | Enabled by default. mod_pagespeed reads `Content-Security-Policy` response headers and meta tags and suppresses optimizations the policy would block, such as inlining resources or injecting scripts. Set to `off` to optimize without consulting CSP. Verify pages that rely on strict policies. In v1.15.0+r18 and later, filters that inject inline scripts also back off cleanly when the policy disallows inline script. |
| `RespectVary`                 | `off`                | When enabled, mod_pagespeed respects `Vary` headers on resources. Resources with `Vary` headers that indicate per-request variation are not rewritten.                                                                                                                                                                                                                                                                         |
| `DisableRewriteOnNoTransform` | `on`                 | When enabled, mod_pagespeed does not optimize resources served with `Cache-Control: no-transform`.                                                                                                                                                                                                                                                                                                                             |
| `LowercaseHtmlNames`          | `off`                | When enabled, mod_pagespeed lowercases all HTML tag and attribute names during parsing.                                                                                                                                                                                                                                                                                                                                        |
| `ModifyCachingHeaders`        | `on`                 | Controls whether mod_pagespeed sets caching headers on HTML responses. Do not disable this unless you fully understand the interaction with downstream caches. Disabling it can cause stale optimized content to be served.                                                                                                                                                                                                    |
| `XHeaderValue`                | `(version string)`   | Sets the value of the `X-Mod-Pagespeed` (Apache) or `X-Page-Speed` (nginx/IIS) [response header](/pagespeed-markers/#x-page-speed).                                                                                                                                                                                                                                                                                            |
| `PreserveUrlRelativity`       | `on`                 | When enabled, mod_pagespeed preserves the relativity of URLs in rewritten HTML. Relative URLs remain relative rather than being converted to absolute URLs.                                                                                                                                                                                                                                                                    |
| `StaticAssetPrefix`           | `/pagespeed_static/` | Sets the URL prefix for mod_pagespeed's static assets (JavaScript libraries, images used by filters).                                                                                                                                                                                                                                                                                                                          |
| `AddResourceHeader`           | _(none)_             | Adds a custom HTTP header to all optimized resources. This directive is repeatable: specify it multiple times to add multiple headers (up to 20).                                                                                                                                                                                                                                                                              |
| `ListOutstandingUrlsOnError`  | `off`                | When enabled, mod_pagespeed includes a list of outstanding resource fetch URLs in error responses. Enable this only for debugging; do not use in production.                                                                                                                                                                                                                                                                   |

## Native module optimization threads {#optimization-threads}

Optimization work runs on two thread pools, separate from the threads that serve
requests. The _rewrite_ pool handles short, latency-sensitive bookkeeping; the
_expensive rewrite_ pool handles heavy CPU work such as image transcoding, so a
large image cannot hold up everything else. This is the in-process module's own
thread configuration — see [Threading](#threading) above for the reverse-proxy
worker's `--num-threads` and `--ram-cache-size` flags, a different setting.

In v1.15.0+r21 and later both pools size themselves. `NumRewriteThreads` and
`NumExpensiveRewriteThreads` default to `auto`, and mod_pagespeed resolves them
at startup by:

1. Taking the CPUs the process is **actually permitted to use** — not the
   host's core count. A CPU quota (a container limit or a systemd unit) and a
   CPU affinity mask both count. A 2-CPU container on a 64-core host sees 2.
2. Allowing optimization at most **half** of those.
3. Dividing by the number of **peer processes** the server is configured to run,
   because each process builds its own pools. Without this step a server with
   many children would multiply its thread count by the number of children.
4. Giving the result to **each** pool, never fewer than one thread.

The resolved counts, and the numbers they were derived from, are written to the
error log at startup.

### What this means per server

<div data-platform="apache" data-platform-label="Apache">

Apache reports its configured child-process ceiling, so the divisor is real:
`MaxRequestWorkers / ThreadsPerChild` on `worker` and `event`, and
`MaxRequestWorkers` on `prefork` — in both cases capped by `ServerLimit`. If you
have lowered `ServerLimit`, that is the number in effect, not the division.

A stock `event` configuration allows 400 workers at 25 threads per child, so the
divisor is 16 and a server resolves to **one thread per pool** unless it has a
great many cores. That is the intended answer: the ceiling counts children the
server is _allowed_ to start, and sizing each child as though it were alone on
the machine is what oversubscribes it. A server deliberately configured with few
children on a many-core machine gets proportionally more.

`prefork` is not threaded and resolves to one thread per pool, as it always has.

</div>

<div data-platform="nginx" data-platform-label="nginx">

nginx does not yet report a process count to the module, so it resolves to
**one thread per pool** and logs that it fell back. It never assumes a larger
divisor: guessing high would silently oversubscribe the machine, which is worse
than being conservative.

This is unchanged from earlier releases. If you have set `NumRewriteThreads` or
`NumExpensiveRewriteThreads` explicitly, your values continue to apply exactly
as before.

</div>

<div data-platform="iis" data-platform-label="IIS">

The IIS module sizes its own pools and does not yet consult these directives.
Its behaviour is unchanged in this release. See
[IIS tuning](/docs/iis-configuration/#iis-tuning).

</div>

### Setting the counts explicitly

An explicit positive value overrides the computed one entirely.

| Value            | Effect                                                               |
| ---------------- | -------------------------------------------------------------------- |
| `auto`           | Resolve as described above. This is the default.                     |
| `0`              | An alias for `auto`.                                                 |
| positive integer | Use exactly this many threads per process, ignoring the computation. |
| negative         | Rejected when the configuration is read.                             |

A negative value is rejected at configuration-read time rather than being
clamped, which on Apache means the server does not start. Very large explicit
values are capped, with a warning naming both the value you asked for and the
value in effect.

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedNumRewriteThreads 4
ModPagespeedNumExpensiveRewriteThreads 4
```

</div>

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed NumRewriteThreads 4;
pagespeed NumExpensiveRewriteThreads 4;
```

</div>

Both directives are **global**. They cannot be set per virtual host or per
location: the pools are built once per process, before any virtual host is
consulted.

### When to change them

Most servers should leave these alone. Reach for them when the log line at
startup shows a resolved count that does not match your deployment — most often
a single-process server on a many-core machine, or a server whose configured
child ceiling is far above the number of children it will really run.

If you raise them, watch request latency rather than only optimization
throughput. The reason optimization is capped at half the machine is that the
remaining half is serving traffic.

## URL Segment Length Limits {#max-url-segments}

Set the maximum length (in characters) of any single URL segment — the
text between two `/` separators — that the in-process module (Apache, nginx,
IIS) will produce when it combines or rewrites resources. The default is
`1024`.

Apache servers historically capped URL segments at about 250 characters per
segment. mod_pagespeed circumvents that limit when running under Apache, but
intermediate proxies or CDNs in front of your origin may re-impose it. If a
downstream component rejects long `.pagespeed.` URLs, lower this value so
mod_pagespeed produces shorter combined URLs.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MaxSegmentLength 250;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMaxSegmentLength 250
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```
pagespeed MaxSegmentLength 250
```

</div>

This directive applies only to the URLs mod_pagespeed generates (for example,
when combining CSS or JavaScript). It does not limit which inbound request
URLs the module will rewrite.

## Native module location scoping {#location-specific-configuration}

This section scopes the in-process module (Apache, nginx, IIS) rather than the reverse-proxy worker documented above.

mod_pagespeed directives can be scoped to specific parts of your site. The available scoping mechanisms vary by platform.

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

Apache supports configuration at several levels:

- **`pagespeed.conf`** — Global defaults that apply to all virtual hosts.
- **`<VirtualHost>`** — Per-site overrides.
- **`<Directory>` and `<Location>`** — Target specific filesystem paths or URL paths.
- **`.htaccess`** — Per-directory configuration placed in the document root or subdirectories.

`.htaccess` configuration is re-read on every request, which adds per-request overhead. For high-traffic sites, prefer `<Directory>` or `<Location>` blocks in the server configuration.

Example using `<Location>`:

```apache
<Location /images/>
    ModPagespeedDisableFilters convert_jpeg_to_webp
</Location>
```

</div>

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

In nginx, use `server` and `location` blocks to scope directives:

```nginx
server {
    pagespeed on;

    location /static/ {
        pagespeed off;
    }
}
```

Directives set in a `server` block apply to all locations within that server unless overridden by a more specific `location` block.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

See [IIS configuration](/docs/iis-configuration/#path-based-matching-with-regex) for IIS path- and hostname-based scoping.

</div>

## Native module virtual hosts {#virtual-hosts}

This section covers the in-process module (Apache, nginx, IIS) rather than the reverse-proxy worker documented above.

Each virtual host can carry its own mod_pagespeed configuration. Directives set at the global level serve as defaults; virtual host configuration overrides them.

One requirement: HTML pages and the optimized resources they reference must share the same configuration options. If a virtual host serves HTML that references resources optimized under a different set of options, the resources may not be found or may be re-optimized unnecessarily.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
server {
    server_name site-a.example.com;
    pagespeed on;
    pagespeed EnableFilters rewrite_images;
}

server {
    server_name site-b.example.com;
    pagespeed on;
    pagespeed EnableFilters collapse_whitespace;
}
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
<VirtualHost *:80>
    ServerName site-a.example.com
    ModPagespeed on
    ModPagespeedEnableFilters rewrite_images
</VirtualHost>

<VirtualHost *:80>
    ServerName site-b.example.com
    ModPagespeed on
    ModPagespeedEnableFilters collapse_whitespace
</VirtualHost>
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

Place a `pagespeed.config` file in each website's root directory. Each file can carry its own set of directives:

**Site A** (`C:\inetpub\site-a\pagespeed.config`):

```
pagespeed on
pagespeed EnableFilters rewrite_images
```

**Site B** (`C:\inetpub\site-b\pagespeed.config`):

```
pagespeed on
pagespeed EnableFilters collapse_whitespace
```

Alternatively, use match rules in the server-level config to scope directives by hostname:

```
hostname: ^site-a\.example\.com$
pagespeed EnableFilters rewrite_images

hostname: ^site-b\.example\.com$
pagespeed EnableFilters collapse_whitespace
```

</div>

## Native module behind a reverse proxy

This section covers the in-process module (Apache, nginx, IIS) running behind a reverse proxy, not the reverse-proxy worker documented above.

When mod_pagespeed runs behind a reverse proxy (such as nginx, Varnish, or a CDN), keep the following in mind:

- The proxy must forward the original `Host` header to the backend so that mod_pagespeed generates correct URLs for optimized resources.
- If the proxy terminates TLS, configure mod_pagespeed to recognize the `X-Forwarded-Proto` header so that it generates `https://` URLs for optimized resources. See [HTTPS configuration](/docs/https-configuration/) for the details.
- Ensure that `.pagespeed.` resource URLs are routed to the backend running mod_pagespeed. The proxy must not cache these resources independently unless you configure cache lifetimes carefully.
- If the proxy strips or modifies response headers, verify that mod_pagespeed's `X-Mod-Pagespeed` or `X-Page-Speed` header and `Cache-Control` directives pass through intact.

See also [CDN integration](/docs/cdn-integration/).

## Notification Deduplication

The worker automatically skips processing when the target cache variant
already exists. This prevents redundant work when multiple requests arrive for
the same URL before the worker has finished optimizing.

For example, if 10 requests for `/photo.jpg` with WebP capability arrive in
quick succession, only the first notification triggers image transcoding. The
remaining 9 are skipped because the WebP variant is already in the cache.

## Health Check

The worker exposes a health check socket at `{socket_path}.health`.
Connect to it to get the current status:

```bash
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX)
s.connect('/var/lib/pagespeed/pagespeed.sock.health')
print(s.recv(256).decode())
s.close()
"
```

**Response format:**
`OK {active}/{max} notifs=N variants=N proactive=N errors=N cache_entries=N\n`

Example: `OK 5/128 notifs=1542 variants=986 proactive=724 errors=3 cache_entries=2048`
