# Docker: install and verify mod_pagespeed 2.1 (nginx reverse proxy)

Source: https://modpagespeed.com/docs/installation-docker/. Header: `X-PageSpeed`.
Scope: the containers in front of an existing HTTP origin. Section 2 is the
one-container evaluation path; production runs the worker and nginx as
separate services (section 3).

## 1. Prerequisites

Docker 24 or newer, Docker Compose v2, and an origin the container can reach.

```bash
docker version --format '{{.Server.Version}}'
docker compose version
curl -s -o /dev/null -w '%{http_code}\n' 'http://<origin-host>:<origin-port>/'   # the origin answers
```

## 2. Install (one container)

```bash
docker run -d --name pagespeed -p 80:80 \
  --add-host=host.docker.internal:host-gateway \
  -e BACKEND_HOST=host.docker.internal -e BACKEND_PORT=<origin-port> \
  -e ACCEPT_EULA=Y \
  ghcr.io/we-amp/pagespeed-combined:latest
```

`BACKEND_HOST` and `BACKEND_PORT` point at the origin; `host.docker.internal`
is the Docker host. `ACCEPT_EULA=Y` acknowledges the terms of service at
https://modpagespeed.com/terms/.

## 3. Minimal configuration (production)

Create `docker-compose.yml`, `nginx.conf` and `entrypoint-worker.sh` as given
in the source document, which carries the current image tags, then:

```bash
docker compose up -d && docker compose ps   # worker and nginx running
```

The nginx directives that matter are `pagespeed on;` and
`pagespeed_cache_path /shared/cache.vol;`; the worker and nginx share the cache
volume and one PID namespace, as that compose file sets up.

## 4. Verify

```bash
curl -s -o /dev/null -D - 'http://localhost/?mps-verify=agent' | grep -i '^x-pagespeed:'
curl -s -o /dev/null -D - 'http://localhost/?mps-verify=agent' | grep -i '^x-pagespeed:'
```

Port 80 for the single container, 8080 for the compose stack. Pass:
`X-PageSpeed: MISS` on the first request, `HIT` on a later one; optimization is
asynchronous, so a second `MISS` moments later means the worker is not done
yet. No header: https://modpagespeed.com/docs/installation-docker/#troubleshooting

## 5. Rollback

```bash
docker rm -f pagespeed   # the single container
docker compose down      # the stack; add -v to drop the cache volume too
docker rmi ghcr.io/we-amp/pagespeed-combined:latest
```

Image tags and rolling back a release: https://modpagespeed.com/docs/uninstall/#docker
