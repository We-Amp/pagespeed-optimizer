---
title: 'Install with Docker'
description: 'Run mod_pagespeed 2.1 in Docker with Docker Compose: the nginx module, the worker, and a shared Cyclone cache. A one-container quick try, then a three-service production stack.'
order: 10
group: 'Install'
lastUpdated: 2026-09-19
---

Deploy mod_pagespeed 2.1 with Docker Compose. You run three containers — nginx
with the pagespeed module, the worker, and your origin server — sharing a
Cyclone cache volume. To try it against your own site in seconds first, start
with the single combined container below.

## Quick try (one container)

The fastest way to see mod_pagespeed against your own site is the combined
image, which runs the worker and nginx together in a single container. Point it
at your origin and publish port 80:

```bash
docker run --rm -p 80:80 \
  -e BACKEND_HOST=host.docker.internal -e BACKEND_PORT=8081 \
  -e ACCEPT_EULA=Y \
  ghcr.io/we-amp/pagespeed-combined:latest
```

Replace `BACKEND_HOST`/`BACKEND_PORT` with your origin. The combined image suits
evaluation and small single-host deployments; for production, run the worker and
nginx as separate services (below) so you can scale and update them
independently — see the [production deployment guide](/docs/deployment/) for the
hardened setup. `:latest` is published only on the combined image — the worker
and nginx images ship immutable version tags (for example `:2.1.0`).

`ACCEPT_EULA=Y` acknowledges the
[Terms of Service](https://modpagespeed.com/terms/), which govern your use of the
image. The container prints the terms URL on startup if you omit the flag.

To confirm the image is optimizing, follow [Verify It Works](#verify-it-works)
below — use port 80 in those commands, since the single container publishes on 80.

## Prerequisites

- Docker Engine 20.10+
- Docker Compose v2

## Directory Structure

Create a project directory with the following layout:

```
modpagespeed/
├── docker-compose.yml
├── nginx.conf
└── entrypoint-worker.sh
```

## Docker Compose Configuration

Create `docker-compose.yml`:

```yaml
services:
  # Your origin server — replace with your actual upstream
  origin:
    image: nginx:stable
    volumes:
      - ./your-site:/usr/share/nginx/html:ro
    expose:
      - '8081'

  # Factory Worker — optimizes cached content
  worker:
    image: ghcr.io/we-amp/pagespeed-worker:2.1.0
    entrypoint: /entrypoint-worker.sh
    environment:
      # Acknowledges the Terms of Service: https://modpagespeed.com/terms/
      ACCEPT_EULA: 'Y'
    volumes:
      - shared:/shared
      - ./entrypoint-worker.sh:/entrypoint-worker.sh:ro
    depends_on:
      - origin

  # Nginx with PageSpeed module
  nginx:
    image: ghcr.io/we-amp/pagespeed-nginx:2.1.0
    ports:
      - '8080:8080'
    volumes:
      - shared:/shared
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
    depends_on:
      - worker

volumes:
  shared:
```

The `shared` volume is where the Cyclone cache file and Unix socket live. Both
the nginx and worker containers mount it at `/shared`.

## Nginx Configuration

Create `nginx.conf`:

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
    access_log    /var/log/nginx/access.log;

    server {
        listen 8080;
        server_name _;

        # Enable PageSpeed
        pagespeed on;
        pagespeed_cache_path /shared/cache.vol;
        # Worker socket path is read from pagespeed-shared.conf automatically

        location / {
            proxy_pass http://origin:8081;
            proxy_set_header Host $host;
            proxy_set_header X-Real-IP $remote_addr;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_set_header X-Forwarded-Proto $scheme;
        }
    }
}
```

The two `pagespeed_*` directives are all you need:

- `pagespeed on` — enables the module for this server block
- `pagespeed_cache_path` — path to the shared Cyclone cache file

The worker socket path and other shared settings are read
automatically from `pagespeed-shared.conf`, which the worker writes next to
the cache file.

## Worker Entrypoint

Create `entrypoint-worker.sh` and make it executable (`chmod +x`). Delegate to
the image's own entrypoint rather than starting `factory_worker` yourself — it
is what establishes the shared-group ownership described below:

```bash
#!/bin/bash
set -e

# Point the packaged entrypoint at this stack's shared directory; it prepares
# ownership and permissions, then starts the optimizer unprivileged.
export DATA_DIR=/shared
exec /docker/entrypoint-worker.sh
```

## How the cache volume is shared

The optimizer and nginx reach the same cache file and Unix sockets through a
**group**, not through world-writable permissions.

Both images create the group `pagespeed` at the fixed **GID 918**. The optimizer
runs as user `pagespeed` (UID 918) and creates its cache volume and sockets mode
`0660`, its shared config `0640`, all owned by `pagespeed:pagespeed`; the shared
directory is `2750` (setgid, group read and traverse). Nginx's worker processes
are members of group 918, so they can map the volume read-write and connect to
the sockets. Nothing on the volume is world-readable or world-writable.

The directory is deliberately **not** group-writable: the serving module only
ever opens files the optimizer authored — it never creates the cache volume —
so nothing on the peer path needs to write into the directory itself.

The GID is fixed and part of the image contract precisely because the two images
are built separately — an automatically allocated group id would land on a
different number in each, and the two would not be sharing anything.

What this means for your compose file:

- **Nothing, in the common case.** The worker container still starts as root,
  prepares the volume, and drops to `pagespeed` before starting the optimizer.
- **If you set `user:` or `--user` on the worker**, use `918:918`, and make sure
  a bind-mounted host directory is owned by `918:918` beforehand — a bind mount
  never inherits the image's ownership.
- **If a third container reads the cache**, give it group `918`
  (`group_add: ["918"]` in compose).
- **Don't publish the management API on a port below 1024.** The optimizer is no
  longer root and cannot bind one; publish a high port instead
  (`PAGESPEED_API_PORT=9880` with `ports: ["9880:9880"]`).
- **On Kubernetes**, set `securityContext.fsGroup: 918` on the pod so the mounted
  volume arrives group-owned by 918. Leave `runAsUser` unset if you can: the
  entrypoint prepares the volume as root and drops to 918 itself, which is the
  only way it can migrate a volume created by an older release. If your policy
  forbids that, set `runAsUser: 918` and `runAsNonRoot: true` — but then the
  volume must already be owned by `918:918`, and a default `emptyDir` (mode
  0777) will draw a startup warning, because a non-root container cannot tighten
  its own mount.

### Upgrading a volume created before 2.1

A shared volume from an earlier release has files owned by whichever process
created them first — usually root, sometimes the nginx user. On first start the
entrypoint takes ownership of the files the optimizer authors, logs each one,
and keeps your existing cache:

```
NOTE: adopted 0-byte cache stem /shared/cache.vol (was uid 999); no content was migrated.
NOTE: adopted existing cache volume /shared/cache-6-2f1c….vol (536870912 bytes, was uid 0) into pagespeed:pagespeed mode 0660.
```

Only state owned by **root** is migrated — that is the pre-2.1 optimizer, the
one identity whose authorship is not in question. That covers the cache volume
and also everything the optimizer reads back at startup, `pagespeed.json`
included: a copy of that file written by anything else could set the key
directories the optimizer fetches its signature-verification keys from, so it
is refused rather than adopted. The two
exceptions are a zero-length cache stem, which is a name and cannot carry a
directive, and the generation counter, which is checked — as a whole file — to
contain nothing but a single decimal number, or nothing at all.

Adopted files keep the optimizer's own permissions rather than a blanket
group-shared mode: the instance id stays owner-only, the shared
config, `pagespeed.json` and the host aliases are group-readable, and only the
cache volume and the serve-stats file are group-writable. The web server can
read what it needs and nothing else.

If it finds something it cannot safely take — a cache file of unknown
authorship, or anything on the volume that is not a plain file the optimizer
wrote — it stops **before** starting the optimizer, exits **78**, names the
path, and prints the fix. The failure is then legible in `docker compose logs
worker` instead of a restart loop whose only visible symptom is that nginx never
starts. The data directory is left at mode `2700`, so nothing can reach the
volume while it is in that state. Files already taken over before the refusal
keep their new ownership — the printed remedy is safe to run over them. Set `PAGESPEED_ADOPT_VOLUME=off` if you
would rather be told what to fix than have the entrypoint migrate the volume for
you.

Exit code `78` always means "the configuration or the volume, not a transient
fault" — it is worth distinguishing from the optimizer's own exit `1` in any
restart alerting you have.

## Start the Stack

```bash
docker compose up -d
```

Check that all three containers are running:

```bash
docker compose ps
```

You should see `origin`, `worker`, and `nginx` all in a running state.

## Verify It Works

Test with a simple request:

```bash
# First request — cache miss, proxied to origin
curl -I http://localhost:8080/
```

Look for the `X-PageSpeed: MISS` header. This means the module is active and the
response was proxied to your origin and cached.

```bash
# Second request — cache hit, served from cache
curl -I http://localhost:8080/
```

You should now see `X-PageSpeed: HIT`.

Optimization is asynchronous. The first request caches and serves the **original**
bytes with `X-PageSpeed: MISS` and notifies the worker, which optimizes in the
background. Later requests serve the optimized variant with `X-PageSpeed: HIT`.

### Verify image optimization

A `HIT` on the HTML confirms the cache is live, but it does not by itself prove
that images are being transcoded. mod_pagespeed keeps the original URL and
serves a smaller WebP or AVIF body through content negotiation on the `Accept`
header, so there are no `.pagespeed.` URLs to look for. Request the same image
three ways and compare:

```bash
curl -s -o /dev/null -D - http://localhost:8080/your-image.jpg -H 'Accept: image/jpeg'
# Content-Type: image/jpeg   — original

curl -s -o /dev/null -D - http://localhost:8080/your-image.jpg -H 'Accept: image/webp'
# Content-Type: image/webp   — smaller

curl -s -o /dev/null -D - http://localhost:8080/your-image.jpg -H 'Accept: image/avif'
# Content-Type: image/avif   — smaller
```

WebP and AVIF are typically smaller than the original; the exact `Content-Length`
depends on the image. Every response carries `Vary: Accept, Save-Data, User-Agent`
so caches keep the variants apart. If the WebP and AVIF responses still match the
original size, the worker may not have finished — wait a moment and retry. If they
stay the same, see
[Worker Not Processing Content](/docs/troubleshooting/#worker-not-processing-content)
and [Images Not Converting to WebP/AVIF](/docs/troubleshooting/#images-not-converting-to-webpavif).

CSS and JavaScript are minified in place at their original URLs — mod_pagespeed does not
combine or rewrite them into new URLs, so the page source stays clean.

## View Logs

```bash
# All services
docker compose logs -f

# Just the worker
docker compose logs -f worker

# Just nginx
docker compose logs -f nginx
```

The worker logs show optimization activity — you'll see messages when it
processes images, CSS, and JavaScript files.

## Cache Size

By default, the cache size is 1 GB. To change it, pass the `--cache-size`
flag to the worker (in bytes):

```bash
exec factory_worker \
  --socket /shared/pagespeed.sock \
  --cache-path /shared/cache.vol \
  --cache-size 536870912  # 512 MB
```

## Stopping and Restarting

```bash
# Stop all containers
docker compose down

# Stop and remove the cache volume (fresh start)
docker compose down -v
```

The cache is stored in a named Docker volume. Stopping containers preserves the
cache — optimized content is still available on restart. Use `down -v` only if
you want to clear the cache completely.

## Kubernetes

For Kubernetes deployments, run nginx and the worker as separate containers in
the same pod, sharing an `emptyDir` volume:

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: pagespeed
spec:
  containers:
    - name: nginx
      image: ghcr.io/we-amp/pagespeed-nginx:2.1.0
      ports:
        - containerPort: 8080
      volumeMounts:
        - name: shared
          mountPath: /shared

    - name: worker
      image: ghcr.io/we-amp/pagespeed-worker:2.1.0
      command: ['/entrypoint-worker.sh']
      env:
        # Acknowledges the Terms of Service: https://modpagespeed.com/terms/
        - name: ACCEPT_EULA
          value: 'Y'
      volumeMounts:
        - name: shared
          mountPath: /shared

  volumes:
    - name: shared
      emptyDir:
        sizeLimit: 512Mi
```

Both containers in the same pod share the same network namespace, so the Unix
socket is accessible without additional configuration.

## Verifying the images

The images are signed with [keyless cosign](https://docs.sigstore.dev/) and carry
an SBOM attestation and SLSA build provenance. The signing identity is the
`We-Amp/modpagespeed-images` publish workflow. To verify a pull:

```bash
# Signature (keyless — no public key to manage)
cosign verify \
  --certificate-identity-regexp '^https://github.com/We-Amp/modpagespeed-images/' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  ghcr.io/we-amp/pagespeed-combined:latest

# SBOM + build provenance
gh attestation verify oci://ghcr.io/we-amp/pagespeed-combined:latest \
  --repo We-Amp/modpagespeed-images
```

The same commands work for `pagespeed-worker` and `pagespeed-nginx` (use a
pinned tag such as `:2.1.0`).

## Troubleshooting

**No `X-PageSpeed` header:**
Check that `pagespeed on;` is set in your nginx config and the module is loaded.
Verify with `docker compose logs nginx`.

**`X-PageSpeed: MISS` on every request:**
The cache file may not be shared correctly. Ensure both containers mount the same
volume at `/shared`, and that the process reading the cache is in group `918` —
`docker compose exec nginx id nginx` should list it. The cache file and sockets are
mode `0660` owned by `pagespeed:pagespeed`; a peer outside the group gets
nothing.

**Worker exits 78 immediately with a block of `ERROR:` lines about ownership:**
The volume holds content from before 2.1 that the entrypoint would not take
ownership of. The message names the path and the remedy; see
[Upgrading a volume created before 2.1](#upgrading-a-volume-created-before-21).

**`X-PageSpeed: MISS` after changing the `user` directive in your nginx.conf:**
Group membership is resolved from the user nginx drops its workers to. If you
set `user nobody;` (or any user that is not in group `918`), the workers lose
access to the cache and every response is a MISS. Use `user nginx;`, or add your
chosen user to group `918` in a derived image.

**Worker not processing content:**
Check worker logs with `docker compose logs worker`. Verify the worker is writing
`pagespeed-shared.conf` next to the cache file (nginx reads the socket path from
this file automatically).

## Next Steps

- [Getting Started](/docs/getting-started/) — Architecture overview and how the
  worker, nginx, and origin fit together
- [Deploy to production (Docker / nginx)](/docs/deployment/) — Hardened
  multi-service setup, image tags, and rollout
- [Configuration Reference](/docs/configuration/) — Tune cache size, worker
  threads, and other options
- [Helm Deployment](/docs/helm-deployment/) — Deploy on Kubernetes with the
  official Helm chart
- [Troubleshooting](/docs/troubleshooting/) — Common issues and diagnostics
