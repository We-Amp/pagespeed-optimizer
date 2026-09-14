# CI infrastructure variables

The CI hub coordinates — hostnames, addresses, filesystem roots and
runner labels — are **environment-specific**. They are supplied to workflows as
**GitHub repository variables** and are deliberately not stored anywhere in this
tree, so the tree stays publishable as-is.

Set them once per repository (`Settings → Secrets and variables → Actions →
Variables`, or `gh variable set <NAME> --repo <owner>/<repo>`). None of them are
credentials; they are addressing/config only. Authentication is unchanged and
still comes from the runner's own SSH key material.

| Variable | What it addresses | Used by |
|---|---|---|
| `CI_HUB_USER` | ssh user on the artifact/cache hub | ssh/scp/rsync steps, `tools/ci/extract-vendor-tarball.sh` |
| `CI_HUB_HOST` | hub hostname | ssh/scp/rsync steps, `tools/ci/resolve-cache-host.sh`, Windows `--remote_cache` |
| `CI_HUB_IP` | hub fallback address (LAN DNS can serve stale A-records) | `docker --add-host`, `tools/ci/resolve-cache-host.sh` |
| `CI_HUB_SHARED_DIR` | shared vendor + artifact root on the hub | vendor fetch, artifact staging |
| `CI_MAC_HOME` | home root on the macOS runners | workspace/cache paths |
| `CI_LINUX_HOME` | home root on the Linux runners | workspace/cache/mirror paths |
| `CI_WIN_USER_HOME` | Windows user-profile root | Bazelisk/Git/gcloud paths in Windows jobs |
| `CI_HEAVY_LABEL` | runner label for the heavy x64 build pool | `runs-on:` |
| `CI_HEAVY_HOST_LABEL` | runner label carried by exactly one host (image gate) | `dep-scan.yml` `runs-on:` |
| `CI_HEAVY_HOST` | hostname of that host (defense-in-depth assertion) | `dep-scan.yml` |

### Derived names the workflows synthesize

The helper scripts do not read `vars.*` themselves — they read plain environment
variables. Each workflow that talks to the hub composes these in its top-level
`env:` block from the repository variables above. They are **not** repository
variables; do not set them in repo settings.

| Derived name | Composed as | Required by |
|---|---|---|
| `CI_HUB_SSH` | `CI_HUB_USER` + `@` + `CI_HUB_HOST` | `tools/ci/stage-artifacts.sh` (fetch branch) |
| `CI_HUB_ARTIFACTS_ROOT` | `CI_HUB_SHARED_DIR` + `/artifacts/pagespeed-optimizer` | `tools/ci/stage-artifacts.sh` (fetch branch) |
| `CI_HUB_VENDOR_DIR` | `CI_HUB_SHARED_DIR` + `/vendor` | `tools/ci/extract-vendor-tarball.sh` (fetch path only) |
| `CI_HUB_ADDR` | not composed — resolved at call time by `tools/ci/resolve-cache-host.sh` from `CI_HUB_HOST`/`CI_HUB_IP`; set it directly to pin a runner from its `.env` | `tools/ci/extract-vendor-tarball.sh` (fetch path only) |

Both scripts require their inputs **only on the branch that reaches the
network**: a run that already has a good local tarball or staged artifacts
never touches the hub and never touches these variables.

### Forks

These workflows cannot run in a fork. Repository variables do not carry across,
so `vars.*` expand to the empty string: `runs-on:` loses its runner label and
the job queues forever, and `docker --add-host=bazel-remote-cache:` fails on an
empty address. This is intentional — the CI hub is not reachable from a fork
anyway. `dep-scan.yml`'s host assertion fails closed with an explicit error
rather than silently passing.

## Bazel remote cache

`.bazelrc` addresses the shared bazel-remote through the environment-neutral
alias **`bazel-remote-cache`**:

* Linux CI builds run in Docker and map the alias with
  `--add-host=bazel-remote-cache:${BAZEL_CACHE_HOST}`, where `BAZEL_CACHE_HOST`
  is either pinned in the runner's `.env` or resolved by
  `tools/ci/resolve-cache-host.sh`.
* Windows CI builds are native and pass `--remote_cache` explicitly.
* Locally, either add a `bazel-remote-cache` entry to `/etc/hosts`, or drop
  `build:remote-cache --remote_cache=http://<host>:9090` into `.bazelrc.user`
  (gitignored, imported at the very END of `.bazelrc` so it wins). Use the
  `build:remote-cache` prefix, not bare `build`: the CI configs pull in
  `--config=remote-cache`, and for a given config the last matching option wins,
  so an override on the same config is what actually takes effect.

Without any of those the remote cache is simply unused; builds still succeed
against the local `--disk_cache`.

## Adding a runner

A new runner needs the labels the workflows ask for. Because the heavy-pool
label is read from `CI_HEAVY_LABEL`, the label can be renamed fleet-wide by
re-registering the runners with the new label and updating the variable — no
workflow edit required.
