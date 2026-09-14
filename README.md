# PageSpeed 2.0

A modern successor to mod_pagespeed for nginx, rebuilt from scratch with a
three-component architecture for zero-copy cache serving and asynchronous
content optimization.

## Architecture

```
                    ┌─────────────┐
    HTTP Request───>│    Nginx    │
                    │  Interceptor│
                    └──────┬──────┘
                           │
                    ┌──────▼──────┐     ┌──────────────┐
                    │   Cyclone   │◄───►│   Factory    │
                    │    Cache    │     │   Worker     │
                    └─────────────┘     └──────────────┘
```

**Nginx Interceptor** — C++ nginx module that classifies requests into
capability-based cache keys, serves optimized variants via zero-copy mmap,
and records cache misses for asynchronous processing.

**Cyclone Cache** — Variant-aware disk cache with memory-mapped directory
sharing. Both nginx and the worker access the same cache file for instant
cross-process visibility.

**Factory Worker** — Lightweight C++ daemon that reads original content from
cache, applies optimizations (image transcoding, CSS/JS minification,
critical CSS extraction), and writes optimized variants back.

## Features

- **Image Optimization** — JPEG/PNG/GIF to WebP and AVIF transcoding,
  lossless PNG reduction, quality-aware compression
- **CSS Minification** — Whitespace, comment, and redundant syntax removal
- **JavaScript Minification** — Comment and whitespace removal
- **Critical CSS Injection** — Extracts above-the-fold CSS and injects it
  inline into HTML
- **Early Hints (103)** — Sends preload hints for stylesheets while waiting
  for upstream
- **Capability-Based Variants** — Different optimized versions for WebP vs
  AVIF clients, mobile vs desktop, Save-Data, connection quality
- **Zero-Copy Serving** — Cache hits served via mmap without copying data
- **Graceful Degradation** — If the worker is down, nginx continues serving
  original content
- **Notification Retry** — Exponential backoff for worker notifications
- **Per-URL Policies** — `pagespeed_disallow` directive for excluding paths
- **Structured Logging** — JSON log output with configurable log levels
- **Security Limits** — Configurable max URL length, content size limits,
  connection limits

## Quick Start

### Build

```bash
# Build all (excludes nginx module)
bazel build //...

# Run all tests
bazel test //...

# Build nginx module (requires nginx source headers)
NGINX_PATH=/path/to/nginx bazel build //src/nginx:ngx_pagespeed_module.so
```

### Deploy

```bash
# Docker Compose (see deploy/ directory)
cd deploy
cp nginx.conf.example nginx.conf
# Edit nginx.conf for your upstream
docker compose up -d

# Or systemd (see deploy/pagespeed-worker.service)
sudo cp deploy/pagespeed-worker.service /etc/systemd/system/
sudo systemctl enable --now pagespeed-worker
```

### Verify

```bash
# First request — cache miss, proxied to origin
curl -I http://localhost/style.css
# X-PageSpeed: MISS

# Subsequent request — served from cache
curl -I http://localhost/style.css
# X-PageSpeed: HIT
```

## Configuration

### Nginx Directives

| Directive | Default | Description |
|-----------|---------|-------------|
| `pagespeed on\|off` | `off` | Enable/disable the module |
| `pagespeed_cache_path PATH` | (required) | Path to Cyclone cache file |
| `pagespeed_disallow PATTERN` | (none) | Exclude URL patterns from optimization |

The worker socket path and HTML processing toggle are configured on
the worker side and shared with nginx automatically via `pagespeed-shared.conf`
(written next to the cache file). See the [Configuration Reference](/docs/configuration/).

### Worker Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--cache-dir DIR` | `/var/cache/pagespeed-optimizer/v1` | Directory holding the cache volume, shared config and serve-stats |
| `--cache-path PATH` | (derived from `--cache-dir`) | Expert override: full cache volume file stem |
| `--socket PATH` | `/run/pagespeed-optimizer/notify.sock` | Unix socket path |
| `--cache-size BYTES` | `104857600` | Cache size (100MB) |
| `--max-connections N` | `128` | Max simultaneous connections |
| `--max-buffer-size BYTES` | `1048576` | Max per-client buffer (1MB) |
| `--connection-timeout MS` | `30000` | Idle connection timeout |
| `--shutdown-timeout MS` | `5000` | Graceful shutdown timeout |
| `--disable-html` | `false` | Disable HTML optimization |
| `--disable-css` | `false` | Disable CSS minification |
| `--disable-js` | `false` | Disable JS minification |
| `--disable-image` | `false` | Disable image transcoding |
| `--max-url-length BYTES` | `8192` | Max URL length |
| `--max-html-size BYTES` | `5242880` | Max HTML size (5MB) |
| `--max-css-size BYTES` | `2097152` | Max CSS size (2MB) |
| `--max-js-size BYTES` | `2097152` | Max JS size (2MB) |
| `--max-image-size BYTES` | `10485760` | Max image size (10MB) |
| `--log-level LEVEL` | `info` | debug\|info\|warning\|error |
| `--log-format FORMAT` | `text` | text\|json |

## Development

### Prerequisites

- Bazel 7+
- C++23 compiler (GCC 13+ or Clang 17+)
- Docker (for sanitizer tests and E2E)
- pre-commit (`pip install pre-commit && pre-commit install`)

### Testing

```bash
# Unit tests
bazel test //...

# Sanitizers (ASan + UBSan + LeakSan + TSan)
./tools/docker-sanitizers.sh

# E2E tests (full stack in Docker Compose)
./tools/e2e/run_e2e.sh
```

### Code Style

- C++23, Google style, 80-character line limit
- Formatting enforced by pre-commit hooks (clang-format, buildifier)
- Run `clang-format -i <files>` before staging C++ changes

## Project Structure

```
lib/           Curated code from mod_pagespeed
  base/        Base utilities (string_util, message_handler, arena)
  html/        HTML parser
  css/         CSS minifier
  js/          JS minifier
  image/       Image codecs and optimization
  cache/       Cyclone cache C++ wrapper
src/           New PageSpeed 2.0 code
  cache/       Capability mask, variant keys
  nginx/       Nginx module
  proto/       Worker IPC protocol
  worker/      Factory worker daemon
test/          Unit and integration tests
tools/         Docker, sanitizers, E2E, formatting scripts
deploy/        Production deployment configs
website/       Documentation website (Astro)
```

## License

Apache License 2.0, see [LICENSE](LICENSE).

Developed by [We-Amp B.V.](https://we-amp.com)
