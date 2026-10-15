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
| `CI_HUB_SSH` | Linux, Windows | ssh target (`user@host`) of the shared artifact hub. Together with `CI_HUB_VENDOR_DIR` the vendor job publishes its tarball there and consumers on other runners fetch it | the vendored tree passes between jobs through `actions/cache` |
| `CI_HUB_VENDOR_DIR` | Linux, Windows | directory on the hub for vendor tarballs (needs `CI_HUB_SSH`) | as above |
| `CI_HUB_ARTIFACTS_ROOT` | Linux | directory on the hub for build bundles, read by `tools/ci/stage-artifacts.sh` when a local bundle is missing (needs `CI_HUB_SSH`) | bundles pass through `actions/upload-artifact` / `download-artifact` only |
| `CI_VENDOR_DIR` | Linux | runner-local directory for vendor tarballs; consumers on the same host extract from it without a network fetch | no local fast path |
| `CI_WIN_VENDOR_DIR` | Windows | the same, for Windows runners: a directory that may hold the vendor tarball the Linux jobs of the same machine published; the Windows jobs read it and never prune it | no local fast path (a hub fetch goes to a per-profile download directory) |
| `CI_WIN_TOOLS_DIR` | Windows | `;`-separated list of directories added to `PATH` ahead of everything else (for example one for `bazelisk`, one for the CMake `bin`, one for the MSYS2 `usr/bin`); together they may provide `bazelisk` (or `bazel`), `bash.exe`, `cmake`, `ninja` and `zstd`. Visual Studio is found with `vswhere` | tools come from `PATH`, and bash from MSYS2 or Git for Windows in their usual locations |
| `BAZEL_CACHE_HOST` | Linux | address of the remote Bazel cache; jobs add `--add-host=bazel-remote-cache:<value>` to their build containers and build with `--config=ci` | builds use `--config=docker` with no remote cache |
| `BAZEL_REMOTE_CACHE_URL` | Windows, macOS | full remote cache URL, passed as `--remote_cache` | no remote cache |
| `BAZEL_SH` | Windows | the bash Bazel runs genrules with (release lane Windows jobs) | MSYS2's bash, else Git for Windows' bash |
| `CI_WIN_BAZEL_OUTPUT_ROOT` | Windows | Bazel `--output_user_root` for every Windows job (a short path on a volume with room) | `C:/b` |
| `CI_DEV_IMAGE_LOCK_DIR` | Linux | directory for the per-host lock files `tools/ci/ensure-dev-image.sh` takes around image builds (useful when several runner instances share one Docker daemon) | locks go to the temporary directory |

Tools the Windows jobs expect (on `PATH` or in `CI_WIN_TOOLS_DIR`): `bazelisk`
or `bazel`, `zstd` (hub channel only), `scp` (hub fetch only), Visual Studio
2022 with the C++ workload, and MSYS2 or Git for Windows for Bazel's shell.
Hosted `windows-2022` runners provide all of them except the hub tools.

Tools the Linux fleet jobs expect on `PATH`: `docker`, `bash`, `git`, `tar` with
zstd support, `rsync` and `ssh` (hub only), and `bazel` or `npm` (the vendor
job installs bazelisk through npm when `bazel` is missing). Hosted runners
provide all of them.

## Windows runner: git and PATH requirements

On a self-hosted Windows runner the jobs assume the following.

1. **Git configuration.** The git the runner service uses (the first `git.exe`
   on the machine `PATH`, as seen by the service account) has
   `core.autocrlf=false` and `core.longPaths=true` in its own system
   configuration. Release jobs check out a tag's tree as it was at the time, so
   repository attributes added later do not apply; the gperf generator input
   and the bash scripts must be LF, and the vendored tree nests deeply enough
   to need long paths. Set both with the full path to that binary:

       "C:\Program Files\Git\cmd\git.exe" config --system core.autocrlf false
       "C:\Program Files\Git\cmd\git.exe" config --system core.longPaths true

   Plain `git config --system` from an interactive shell may configure a
   different install than the one the service uses. Adjust the path to the
   install the service resolves.
2. **Machine `PATH`.** The tools are on the machine `PATH`, not a user's
   `PATH`: Git for Windows `cmd` and `bin` (so `shell: bash` resolves to Git
   bash or MSYS2 bash, never the WSL launcher in System32), MSYS2 `usr\bin`
   before System32 when MSYS2 provides bash, and the bazelisk, nasm and CMake
   directories. Alternatively list those tool directories in `CI_WIN_TOOLS_DIR`;
   only the Bazel build steps honour it. Bash steps and the release jobs' tool
   checks read `PATH` alone.
3. **No user-profile path on `PATH`.** No entry of the form
   `C:\Users\<name>\...` may appear on the machine `PATH`. Job logs print
   `PATH`, and a public log must not carry a user name.
4. **Bazel output root.** Set `CI_WIN_BAZEL_OUTPUT_ROOT` (see the table above)
   to a short path on a volume with room.

## Adding a variable

1. Give it a neutral name that says what it is, not where it lives.
2. Read it with a default (`${NAME:-}`) and branch on it being set; state the
   degraded behaviour in the job or the script next to the branch.
3. Never print its value; print what it enables ("using the remote cache").
4. Add a row to the table above.
