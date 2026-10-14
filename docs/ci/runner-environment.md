<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Runner environment contract

Fleet jobs (see `docs/ci/pipelines.md`) may use facilities that exist only on
a particular runner: a shared artifact hub, a remote Bazel cache, a fast
local vendor directory. None of that is written into a workflow or a script
in this repository. A runner offers it through environment variables in its
own environment (for the GitHub Actions runner, its `.env` file), and the
jobs use each one only when it is set.

**The rule: every variable is optional. With none of them set, a job must
still run, slower.** A job that needs a variable to succeed is a bug. Values
are never echoed to the job log, and no value appears in this repository.

| Variable | Platform | What it enables when set | Without it |
|---|---|---|---|
| `CI_HUB_SSH` | Linux | ssh target (`user@host`) of the shared artifact hub. Together with `CI_HUB_VENDOR_DIR` the vendor job publishes its tarball there and consumers on other runners fetch it | the vendored tree passes between jobs through `actions/cache` |
| `CI_HUB_VENDOR_DIR` | Linux | directory on the hub for vendor tarballs (needs `CI_HUB_SSH`) | as above |
| `CI_HUB_ARTIFACTS_ROOT` | Linux | directory on the hub for build bundles, read by `tools/ci/stage-artifacts.sh` when a local bundle is missing (needs `CI_HUB_SSH`) | bundles pass through `actions/upload-artifact` / `download-artifact` only |
| `CI_VENDOR_DIR` | Linux | runner-local directory for vendor tarballs; consumers on the same host extract from it without a network fetch | no local fast path |
| `CI_WIN_VENDOR_DIR` | Windows | the same, for Windows runners | no local fast path |
| `BAZEL_CACHE_HOST` | Linux | address of the remote Bazel cache; jobs add `--add-host=bazel-remote-cache:<value>` to their build containers and build with `--config=ci` | builds use `--config=docker` with no remote cache |
| `BAZEL_REMOTE_CACHE_URL` | Windows, macOS | full remote cache URL, passed as `--remote_cache` | no remote cache |
| `BAZEL_SH` | Windows | the bash Bazel runs genrules with (release lane Windows jobs) | MSYS2's bash, else Git for Windows' bash |
| `CI_WIN_BAZEL_OUTPUT_ROOT` | Windows | Bazel `--output_user_root` for the release lane's Windows jobs (a short path on a volume with room) | `C:/b` |
| `CI_DEV_IMAGE_LOCK_DIR` | Linux | directory for the per-host lock files `tools/ci/ensure-dev-image.sh` takes around image builds (useful when several runner instances share one Docker daemon) | locks go to the temporary directory |

Tools the fleet jobs expect on `PATH`: `docker`, `bash`, `git`, `tar` with
zstd support, `rsync` and `ssh` (hub only), and `bazel` or `npm` (the vendor
job installs bazelisk through npm when `bazel` is missing). Hosted runners
provide all of them.

## Adding a variable

1. Give it a neutral name that says what it is, not where it lives.
2. Read it with a default (`${NAME:-}`) and branch on it being set; state the
   degraded behaviour in the job or the script next to the branch.
3. Never print its value; print what it enables ("using the remote cache").
4. Add a row to the table above.
