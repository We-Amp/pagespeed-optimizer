<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# CI pipelines

This repository's CI is split into thin **caller** workflows and
**reusable** workflows. The heavy jobs run on a fleet of build runners that
are reachable only through this repository's reusable workflows at
`refs/heads/main` (and `refs/heads/ci-dev`). A pull request therefore cannot
put its own workflow code on a fleet runner: whatever it changes in
`.github/workflows/`, the jobs that reach the fleet are the ones already on
`main`.

## The shape

| Kind | Triggers | Holds | Example |
|---|---|---|---|
| Caller | `pull_request`, `push`, `schedule`, `workflow_dispatch` | triggers, `concurrency`, `permissions`, jobs that only `uses:` a reusable workflow `@main` or `@ci-dev`, and at most an aggregate gate job on a hosted label | `nightly.yml`, `sweep-queued.yml`, `ci-dev.yml`, `ci.caller.yml.example` |
| Reusable | `workflow_call` only | every job, with its `runs-on` | `pipeline-pr.yml`, `pipeline-full.yml`, `pipeline-nightly.yml`, `pipeline-sweep.yml` |

Rules that follow from the shape:

- Concurrency groups live in the callers. A group inside a called workflow
  evaluates the caller's context and can collide or deadlock.
- A caller never names a fleet label. The aggregate gate job (`CI gate`)
  runs inline because it reads `needs.*.result`, which a called workflow
  cannot see; it is pinned to a hosted label.
- Reusable workflows declare `permissions: contents: read`. A job that needs
  more asks for it on the job, and anything that writes to GitHub (issues,
  releases, comments) runs on a hosted runner fed by an artifact, never on a
  fleet runner (see "Report jobs" below). A called workflow can narrow the
  permissions its caller grants but never widen them, so a caller whose
  workflow has a report job grants `issues: write` on the job that calls it.
- Jobs pass files to each other with `actions/upload-artifact` and
  `actions/download-artifact`, or `actions/cache`, never through a path on a
  particular machine. A runner-local fast path is allowed only through the
  runner-environment contract (`docs/ci/runner-environment.md`).

## Tiers

| Tier | Workflow | Runs on | Holds |
|---|---|---|---|
| PR | `pipeline-pr.yml` | every pull request and push to `main` | the hosted checks (daemon build and unit-test slice, Apache RAT, license headers, SBOM drift, release-note gate and the chart, probe and entrypoint checks, the public-tree hygiene gate, the blocking and the report-only dependency scans, the ABI check, the JS/TS unit tests, the release packaging container), and the fleet legs: vendor, Linux build, Linux unit tests, lint, the JS and HTML kernel differentials, the release-config test compile, the Windows build and unit tests, and the path-filtered CSS and JS fuzz smokes. Later changes add the Linux arm64 build and tests |
| Full | `pipeline-full.yml` | push to `main`, `workflow_dispatch` with `full: true`, or a pull request labelled `ci:full` | its own vendor and Linux x64 build; sanitizers (ASan, TSan); package smokes (Linux x64, Linux arm64, Windows, with a Windows build of the two shipped binaries to feed it); the daemon privilege rigs (three distros on the fleet, one on hosted arm64); HTTP compliance; the async CSS probe markup check; container cache sharing; the Linux arm64 build and unit tests |
| Nightly | `pipeline-nightly.yml` | `nightly.yml`: daily at 03:00 UTC and on dispatch | the macOS build, unit tests and package smoke; the periodic vendor and optimized builds (x64 on the fleet, arm64 hosted); the stress, production, real-world and end-to-end lanes on both architectures; the x64 privilege-rig soak; the rendered async CSS probe; CSS fuzzing (nightly, and a manual shadow lane); the nightly image leg of the dependency scan |
| Sweep | `pipeline-sweep.yml` | `sweep-queued.yml`: hourly and on dispatch | cancels runs that wait for a runner nobody serves (see "The queued-run sweep") |

The `ci:full` label is read when a pull request run starts (open, push,
reopen). After adding the label, push or re-run the workflow to get the
full tier.

## Runner routing

Every fleet job chooses its runner from the event, never from inputs. A pull
request from a fork, or from Dependabot, runs on a GitHub-hosted runner;
everything else runs on a fleet class label. The expression, copied
verbatim into every fleet job:

```yaml
runs-on: ${{ (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) && 'ubuntu-latest' || 'fleet-heavy' }}
```

The fleet class is one of `fleet-heavy`, `fleet-light`, `fleet-windows`,
`fleet-iis`. Windows jobs use `windows-2022` as the hosted side.

### The kill switch

`FLEET_ENABLED` is a repository variable. Unset or `true` means the fleet is
in use; `false` is the kill switch.

The PR, full and nightly tiers use the switch routing expression, copied
verbatim into every fleet job that has a hosted twin. It sends the job to the
hosted side for a fork or Dependabot pull request and also while the switch
is off:

```yaml
runs-on: ${{ (vars.FLEET_ENABLED == 'false' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]'))) && 'ubuntu-latest' || 'fleet-heavy' }}
```

Such a job carries no `if:` guard (it always runs, on one side or the other),
except a path filter alone (`needs.changes.outputs.<name> == 'true'`). Jobs
with no hosted twin keep the plain routing expression and a fleet-only guard
whose `FLEET_ENABLED` term is `!= 'false'`: they are skipped, not failed,
while the switch is off, and a `fleet-off-notice` job in each tier says so in
an annotation and the job summary.

To flip it: Settings, Secrets and variables, Actions, Variables, set
`FLEET_ENABLED` to `false` (or `gh variable set FLEET_ENABLED --body false
-R We-Amp/pagespeed-optimizer`); set it back to `true` to go back (do not delete it: the release lanes still test for `true`). It
is read when a run starts, so re-run a run that began before the change. The
hosted legs run without the hub and the remote cache and are slower; the
fleet-only legs are skipped, and the CI gate accepts those skips.

The release lanes do not use the switch routing expression. They keep the
plain routing expression, the original guards (`FLEET_ENABLED == 'true'`) and
their hosted twins; see "Release lanes: hosted twins".

#### The plain guards

A fleet job routed by the plain expression (the release lanes, and the jobs
without a hosted twin) carries one of these guards, verbatim, as its `if:`:

```yaml
if: ${{ vars.FLEET_ENABLED == 'true' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}
```

While the variable is unset (or anything but `true`), these jobs are skipped
for same-repository events and still run on hosted runners for fork and
Dependabot pull requests, which need no fleet. Clearing it is the kill switch
for the release lanes' fleet jobs, which then run their hosted twins.

Two more guards exist for jobs that cannot fall back to a hosted runner, and
the gate accepts exactly these three. **Fleet only** is for a job that mounts
the container socket, runs privileged containers or needs a real Linux
kernel: a fork or Dependabot pull request skips it.

```yaml
if: ${{ vars.FLEET_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}
```

The manual shadow lanes keep the dispatch-only guard below with `== 'true'`, so they
are skipped while the variable is unset or `false`.

**Dispatch only** is for a manual lane; a dispatch is never a fork pull
request.

```yaml
if: ${{ github.event_name == 'workflow_dispatch' && vars.FLEET_ENABLED == 'true' }}
```

The PR, full and nightly tiers write the fleet-only guard with the first term
`vars.FLEET_ENABLED != 'false'` (unset means on); the gate accepts both
spellings. A job that needs more conditions (for example a path filter) appends them in
a later change together with a matching gate rule; the gate rejects any
fleet job whose `if:` is not exactly one of the three guards. A job that runs
on a hosted label only (hosted arm64, macOS, report jobs) carries no guard.

### Hosted side: degraded mode

Every fleet job must also run on a hosted runner: no hub, no remote cache,
no host paths. Anything host-specific is read from the runner environment
(`docs/ci/runner-environment.md`) and used only when set. When none of it is
set the job runs slower, never fails for that reason. In `pipeline-pr.yml`:

- **vendor** decides a distribution channel: `hub` when the runner provides
  a local vendor directory or a hub, `cache` otherwise. On the cache channel
  `vendor/` passes to the consumer jobs through `actions/cache`, keyed on the
  vendor manifest (`MODULE.bazel`, `.bazelversion`, `bazel/**`,
  `third_party/**`); a dependency bump re-vendors once.
- **Linux Build** and **Linux Unit Tests** use the remote Bazel cache only
  when `BAZEL_CACHE_HOST` is set (`--config=ci`); otherwise they build with
  `--config=docker` and no cache. The build bundle always goes to
  `actions/upload-artifact` (retention 3 days) and the unit tests download it.
- **Kernel differentials**, the **release-config test compile** and the
  **fuzz smokes** follow the same two rules: the remote cache only when
  `BAZEL_CACHE_HOST` is set, and the dev image built in the job when the
  runner has none. The differentials' floor ratchet reads the public
  `We-Amp/mod_pagespeed` repository, and only when a pin floor moves.
- **Windows Build** and **Windows Unit Tests** take their tools from `PATH`
  or from `CI_WIN_TOOLS_DIR`. On the hub channel a runner with a vendor
  tarball (`CI_WIN_VENDOR_DIR`, or a fetch from `CI_HUB_SSH`) extracts it
  over the checkout; otherwise Bazel fetches the dependencies itself. The
  remote cache is used only when `BAZEL_REMOTE_CACHE_URL` is set. The build
  bundle goes to `actions/upload-artifact` (`ci-artifacts-win-x64`). The
  hosted Windows build time has not been measured; if it proves too slow, the
  fallback is to run these two jobs on the fleet only.
- **Lint** keeps its Bazel state in a per-runner cache directory under the
  runner's home, which persists on a fleet runner and is empty on a hosted
  one. No credential enters the lint containers.

### Release lanes: hosted twins

A release cannot wait for the fleet. In the release pipelines
(`pipeline-release-optimizer.yml`, `pipeline-release-nuget.yml`) every
fleet job has a hosted twin: the same steps (shared through a YAML anchor),
a hosted label, and `if: ${{ vars.FLEET_ENABLED != 'true' }}`. Exactly one
of each pair runs, so clearing `FLEET_ENABLED` moves a release to hosted
runners instead of skipping it. A job that consumes the output of a pair
names both in `needs:` and runs when either succeeded. Release lanes pass
every file between jobs as workflow artifacts; they use no hub.

### Path-filtered jobs

The `changes` job (hosted) diffs a pull request against its base and
publishes one boolean per filter (`css`, `js`). A fleet job that depends on a
filter narrows the guard with `&& needs.changes.outputs.<name> == 'true'`:

```yaml
if: ${{ (vars.FLEET_ENABLED == 'true' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]'))) && needs.changes.outputs.css == 'true' }}
```

Outside a pull request every filter is false. The workflow-shape gate accepts
the guard either verbatim or in exactly this narrowed form.

## Artifacts

Jobs pass files through `actions/upload-artifact` and
`actions/download-artifact`. Artifact names are unique within a workflow run,
and the caller runs the PR tier and the full tier in one run, so each tier
prefixes its names: the PR tier uses none, the full tier `full-`, the nightly
tier `nightly-`. A build bundle is the directory `.ci-artifacts` (binaries,
`metadata.json`, `checksums.sha256`); a consumer downloads it to a directory
and hands the parent to `tools/ci/stage-artifacts.sh`, which validates it.
Retention is three days for bundles and evidence.

## Report jobs

A job that files or closes an issue never runs on the fleet. The fleet job
uploads a small artifact (the findings text, plus a `lane-ran` marker file
that says the lane reached a verdict) and a hosted `report` job with
`issues: write` downloads it and runs `tools/sbom/post-findings-issue.sh`.
Without the marker, or without the artifact, the report job leaves the issue
alone, so a failure before the lane ran cannot close a live issue. Only a run
on the default branch changes the issue; a branch dispatch is a dry run.
The gate enforces the split (rule f): every reusable workflow declares a
top-level `permissions:` block with no write access, and a write permission
(quoted or not) is allowed only on a job with a hosted `runs-on`. The report
jobs check out `main` for the tools they run, because they hold a write token.

## The queued-run sweep

A job whose runner label nothing serves (a retired label, or a fleet class
whose runners are not registered while `FLEET_ENABLED` is on) queues for days
instead of failing. With `FLEET_ENABLED` off the fleet guard skips the job, so
nothing queues. `sweep-queued.yml` runs hourly on a hosted runner and calls
`pipeline-sweep.yml`, which runs `tools/ci/sweep-queued-runs.sh`: it lists runs
in `queued` or `waiting` status whose latest attempt started more than 30
minutes ago (`run_started_at`, so a re-run of an old run is not stale; the
script falls back to `created_at` only when that field is empty), keeps any
run with a job that has a runner and any run of a workflow on the skip list
(`Release` by default, because a run that waits for a manual approval looks the
same), and cancels the rest with one log line each. `--dry-run` only logs.

The threshold is a repository variable: set `SWEEP_MAX_AGE_MINUTES` (a whole
number of minutes) to override the 30 minute default. Raise it when a busy
runner class legitimately keeps whole runs queued for longer, for example
behind the long nightly stress legs. The logic is tested against a fake `gh` by
`tools/ci/test_sweep_queued_runs.sh`, which the sweep job runs first.

## Required checks

Until the caller switch, `ci.yml` keeps its four hosted jobs and their check
names. After the switch (`ci.caller.yml.example` renamed over `ci.yml`), a
check from a called workflow is named `<caller job> / <job name>`, so the
required checks become `pr / Build daemon & unit-test slice (ubuntu)`,
`pr / License audit (Apache RAT)`, `pr / License Headers + SBOM Drift`,
`pr / Public-tree hygiene gate`, `pr / Blocking Dep Scan (npm + cargo)`,
`pr / ABI Compatibility (lib/pagespeed C API)`, `pr / JS/TS Unit Tests`,
`pr / Release Packaging Container`, the fleet legs once `FLEET_ENABLED` is on
(`pr / Linux Build`, `pr / Linux Unit Tests`, `pr / Lint (clang-tidy +
clang-format)`, `pr / JS Kernel Differential (vendored goldens)`,
`pr / HTML Parse Differential (vendored goldens)`, `pr / Release-config test
compile`, `pr / Windows Build`, `pr / Windows Unit Tests`), and `CI gate`.

Not listed individually: `pr / Dependency Scan (report-only)` (it never
fails the build) and the two fuzz smokes (`pr / CSS fuzz smoke (deterministic
oracle replay)`, `pr / JS fuzz smoke (deterministic oracle replay)`), which
are skipped on a pull request that does not touch their paths. The aggregate
`CI gate` treats `skipped` as fine and fails on any other non-success
result, so it covers all three.

## Trying a pipeline change on the fleet: `ci-dev`

A change to a reusable workflow does not reach the fleet from its own pull
request (the callers pin `@main`). To exercise it before merge:

1. Push the change to the `ci-dev` branch:
   `git push --force origin HEAD:ci-dev`.
2. Dispatch the `CI (ci-dev)` workflow (`ci-dev.yml`) from the Actions tab
   or with `gh workflow run ci-dev.yml`. It calls `pipeline-pr.yml@ci-dev`.
3. When it is green, merge the pull request as usual.

The `ci-dev` branch is shared and disposable; whoever pushes it last owns
the next dispatch.

## The workflow-shape gate

`tools/ci/check-workflow-shape.sh` runs in the `Public-tree hygiene gate`
job and fails the build when the shape breaks:

- **(a)** no workflow uses `pull_request_target`;
- **(b)** a workflow with any trigger other than `workflow_call` is a caller:
  its jobs only `uses:` `We-Amp/pagespeed-optimizer/.github/workflows/<file>.yml@main`
  or `@ci-dev`, with no `runs-on` or `steps`, except an aggregate gate job
  on a hosted label. Files listed in `tools/ci/workflow-shape-allowlist.txt`
  (with a reason each) are exempt from this rule only;
- **(c)** no local reusable-workflow reference (`uses: ./.github/workflows/...`);
- **(d)** in a reusable workflow every `runs-on` is a hosted label
  (`ubuntu-latest`, `ubuntu-24.04`, `ubuntu-24.04-arm`, `windows-2022`,
  `macos-15`, `macos-latest`) or exactly the routing expression with a fleet
  class, and a job routed by the expression carries the fleet guard verbatim
  (or narrowed by a `changes` path filter, see above);

  class, and a job routed by the expression carries one of the three fleet
  guards verbatim;
- **(f)** in a reusable workflow a `write` permission (workflow or job level)
  is allowed only on a job with a hosted `runs-on`;
- **(e)** `.github/` and `tools/ci/` carry no LAN address literals, no
  `.home` names, no `:9090`, no `self-hosted` and none of the retired runner
  label words.

The gate is line-oriented: `runs-on` and the guard must each fit on one
line. Fixtures live in `tools/ci/testdata/workflow-shape/`; run
`tools/ci/test_check_workflow_shape.sh` after changing the gate.

## Runner group policy entries

The fleet runner group admits only the workflows listed in its
"selected workflows" policy; a job from any other workflow file or ref
queues and never starts. The entries, one per reusable workflow that can
reach the fleet:

```text
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-pr.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-full.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-nightly.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-optimizer.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-nuget.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-pr.yml@refs/heads/ci-dev
```

`pipeline-release-smoke.yml` runs on hosted runners only and needs no
entry. Callers (`ci.yml`, `nightly.yml`, `ci-dev.yml`, `release.yml`,
`release-smoke.yml`) never get one. Add an entry in the same change that
gives a reusable workflow its first fleet job.
