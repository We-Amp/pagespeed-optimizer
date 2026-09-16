# CI infrastructure variables

The CI helper scripts in `tools/ci/` take their environment-specific
coordinates — hosts, addresses, filesystem roots and runner labels — from
environment variables, never from values hardcoded in this tree, so the tree
carries no environment-specific configuration. The project's own CI supplies
them as GitHub repository variables; anyone running the same scripts in
another environment sets the same names themselves. None of them are
credentials: they are addressing/config only, and authentication comes from
the runner's own SSH key material.

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
| `CI_HEAVY_HOST_LABEL` | runner label carried by exactly one host (the dependency-scan image gate) | that job's `runs-on:` |
| `CI_HEAVY_HOST` | hostname of that host (a defense-in-depth assertion in the same job) | the dependency-scan job |

### Derived names composed from the variables above

The helper scripts do not read repository variables themselves — they read
plain environment variables. The caller composes these from the variables
above (in CI, in each hub-talking workflow's top-level `env:` block); they
are not repository variables themselves.

| Derived name | Composed as | Required by |
|---|---|---|
| `CI_HUB_SSH` | `CI_HUB_USER` + `@` + `CI_HUB_HOST` | `tools/ci/stage-artifacts.sh` (fetch branch) |
| `CI_HUB_ARTIFACTS_ROOT` | `CI_HUB_SHARED_DIR` + `/artifacts/pagespeed-optimizer` | `tools/ci/stage-artifacts.sh` (fetch branch) |
| `CI_HUB_VENDOR_DIR` | `CI_HUB_SHARED_DIR` + `/vendor` | `tools/ci/extract-vendor-tarball.sh` (fetch path only) |
| `CI_HUB_ADDR` | not composed — resolved at call time by `tools/ci/resolve-cache-host.sh` from `CI_HUB_HOST`/`CI_HUB_IP`; set it directly to pin a runner from its `.env` | `tools/ci/extract-vendor-tarball.sh` (fetch path only) |

Both scripts require their inputs **only on the branch that reaches the
network**: a run that already has a good local tarball or staged artifacts
never touches the hub and never touches these variables.

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
