<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Release lanes

`release.yml` is a thin caller (see `docs/ci/pipelines.md`). A tag selects
one of two lanes, each a reusable workflow pinned to `main`:

| Tag | Lane | Publishes |
|---|---|---|
| `optimizer-v<version>` | `pipeline-release-optimizer.yml` | a GitHub release with the host packages: deb and rpm for amd64 and arm64, a Windows x64 zip, `SHA256SUMS` |
| `v<version>` | `pipeline-release-nuget.yml` | five NuGet packages on nuget.org (WeAmp.PageSpeed, WeAmp.PageSpeed.AspNetCore, WeAmp.PageSpeed.NativeAssets.Linux / .Windows / .macOS) and a draft GitHub release with the packages, SBOM, VEX and `SHA256SUMS` |

`optimizer-v*` never matches `v*`. `release-smoke.yml` runs
`pipeline-release-smoke.yml` when a `v*` release is published: it installs
the packages from nuget.org on linux-x64, linux-arm64, win-x64 and
osx-arm64 (all hosted) and checks the console, the worker health and the
console pages with Playwright. Releases of `optimizer-v*` tags do not
trigger it.

## Runs

| Trigger | Run |
|---|---|
| push of a tag | real |
| `workflow_dispatch` with `tag` and `dry_run: true` (the default) | dry run |
| `workflow_dispatch` with `tag` and `dry_run: false` | real |

**Dry run.** Builds, tests and validates everything and keeps the result as
workflow artifacts. It creates no release and no tag, pushes nothing to
nuget.org, sends no notification and never enters the `release`
environment, so it needs no approval.

- Optimizer lane: the five packages and `SHA256SUMS`, validated fail-closed
  (artifact `optimizer-release-dist`). The tag may not exist yet; the lane
  then builds the dispatched ref. The ref guard reports its verdict and
  does not stop a dry run.
- NuGet lane: every build and test and the five `.nupkg` files (artifact
  `nupkg-<sha>`). The Windows binaries in a dry run are unsigned, because
  signing happens only inside the environment. The tag must exist and be
  reachable from `main`: the ref guard is enforced on NuGet dry runs, so the
  first dry run needs an existing `v*` tag (pushing one starts a real run,
  which waits at its first environment job for approval).

**Real run.** Requires the tag to exist (the lanes never create tags) and to
pass the ref guard, then publishes from jobs inside the `release`
environment.

## The ref guard

`tools/ci/ref_guard.sh` (with `.github/actions/ref-guard` and the test
`tools/ci/test_ref_guard.sh`) runs first in both lanes, on a hosted runner,
from a checkout of `main`, so the guard code is never the tree being
released. It refuses:

- any event other than a tag push or a dispatch;
- a ref other than `refs/heads/main` or a tag of the lane's prefix;
- a tag whose commit is not reachable from `main` (a tag name proves
  nothing; anyone who can push a tag can point it anywhere);
- a dispatch whose `tag` input does not exist, is not on `main`, or differs
  from the tag the dispatch ran on.

It also resolves what every later job checks out: a dispatch builds the
dispatched tag, never the head of the branch it ran on.

## The version guards

Both lanes hold the tag to `VERSION.txt` of the commit they build, the
number the worker reports:

- NuGet lane: `v<X>` must equal `VERSION.txt` (inline step in the `vendor`
  job).
- Optimizer lane: from 2.5.0 on, `optimizer-v<X>` must equal `VERSION.txt`
  (`tools/ci/check_optimizer_tag_version.sh`, in the `version` job, run from
  `main`; self-test in `pipeline-pr.yml`). The host packages up to
  `optimizer-v1.19.0` were numbered on the module's 1.x line (built from
  `VERSION.txt` 2.4.0); those tags are below 2.5.0 and pass with a notice. A
  release candidate may match on its `X.Y.Z` core, and so may a pushed
  `-pre` tag, which runs as a real run like any other tag. Only a dry run of
  a tag that does not exist yet turns a mismatch into a warning (an
  annotation and a line in the job summary) and skips `-pre` tags; once the
  tag exists, a mismatch fails the dry run too.

From 2.5.0 the module release pairs with `optimizer-v` plus the same version,
so the cut runs in this order: bump `VERSION.txt` to the release version on
`main`, tag `optimizer-v<X>` on that commit, record the pin in the module
repository (`install/debian/optimizer-pin.json`), then tag `module-v<X>`
there (its `docs/ci/release.md` has the module side). Once the module
packages are published, `MODULE_PACKAGE_VERSION` in
`website/src/data/product-facts.mjs` (1.19.0 today) moves to the same
version: the scan's lifecycle sentence (`website/src/lib/scan/lifecycle.ts`)
compares a detected `X-Mod-Pagespeed` header against it, so until it moves a
1.19.0 install is never told that a newer release exists, and
`module-package-version.test.ts` compares it with the live package index.
Known stale wording, left for the website copy pass: the 2.0.x sentence
there says a setup "moves to 2.1 today", which reads wrong once 2.5.0 is the
current release.

## The `release` environment

| Setting | Value |
|---|---|
| Name | `release` |
| Required reviewer | the release owner |
| Deployment tags | `v*` and `optimizer-v*` |

The environment is configured in the repository settings, not here. Jobs
that run in it:

| Lane | Job | Why |
|---|---|---|
| optimizer | `release` | creates the GitHub release |
| NuGet | `sign-windows` | Authenticode signing with the six `AZURE_*` secrets |
| NuGet | `package` | draft GitHub release, the two notifications (`CORP_DISPATCH_TOKEN`) |
| NuGet | `publish-nuget` | nuget.org Trusted Publishing (OIDC, `id-token: write`) |
| NuGet | `promote-release` | draft to published, only with `auto_promote` |

Each of these jobs waits for its own approval. The tag policy checks the
ref the run started on: a tag push passes; a real run by dispatch must be
started on the tag itself, for example
`gh workflow run release.yml --ref v2.4.0 -f tag=v2.4.0 -f dry_run=false`.
A dispatch from `main` with `dry_run: false` reaches the environment and is
refused there.

No secret reaches a fleet runner. Fleet jobs build and test; everything
that signs, writes to GitHub or publishes runs on a hosted runner.

## Secrets

Environment secrets of `release`. `pipeline-release-nuget.yml` declares each
one (none required), and `release.yml` passes each one explicitly (never
`secrets: inherit`); a job in the environment reads the environment's
value.

| Secret | Used by | Purpose |
|---|---|---|
| `AZURE_TENANT_ID` | `sign-windows` | Trusted Signing: tenant of the signing app registration |
| `AZURE_CLIENT_ID` | `sign-windows` | Trusted Signing: client id |
| `AZURE_CLIENT_SECRET` | `sign-windows` | Trusted Signing: client secret |
| `AZURE_TRUSTED_SIGNING_ENDPOINT` | `sign-windows` | Trusted Signing endpoint |
| `AZURE_TRUSTED_SIGNING_ACCOUNT` | `sign-windows` | Trusted Signing account name |
| `AZURE_TRUSTED_SIGNING_PROFILE` | `sign-windows` | certificate profile name |
| `CORP_DISPATCH_TOKEN` | `package` | fine-grained token with `actions: write` on the two notification targets |

The optimizer lane needs no secret beyond the job token.

Repository variables read by the NuGet lane (a repository variable that
is unset skips its dispatch step with a notice; a variable that is set while
`CORP_DISPATCH_TOKEN` is empty fails the step with an error):

| Variable | Purpose |
|---|---|
| `RELEASE_BUMP_REPO` | `owner/repo` that receives the `release-bump` repository_dispatch |
| `RELEASE_IMAGES_REPO` | `owner/repo` that receives the `release-images` repository_dispatch |
| `FLEET_ENABLED` | `true` routes the fleet jobs to the fleet; otherwise their hosted twins run |

The `release-images` payload names this run (`artifacts_repository`,
`run_id`) and the artifacts `docker-artifacts-linux-x64` and
`docker-artifacts-linux-arm64` (factory_worker, the nginx module and its
nginx version), kept for 30 days.

The receiving side of `release-images` must download the
`docker-artifacts-*` artifacts of the run named in the payload
(`artifacts_repository`, `run_id`) instead of fetching an archive.
`source_tag` and `source_sha` now equal the release tag and its commit; they
no longer name a separate source tag.

`sign-windows` asks for its approval as soon as the Windows build finishes,
before the tests and security gates have a verdict. The approval can
therefore be given ahead of the test result; check the run before approving.

A release that `promote-release` flips to published with the job token does
not fire the `release: published` event. After an auto-promote, start the
smoke lane by hand with a manual dispatch.

A dispatch whose tag starts with neither `optimizer-v` nor `v` fails in the
`unsupported-tag` job of the caller instead of ending green with no jobs.

## nuget.org Trusted Publishing (owner steps)

`publish-nuget` exchanges the job's OIDC token for a short-lived nuget.org
API key with `NuGet/login`. nuget.org matches the token's
`job_workflow_ref`, which for a reusable workflow is the called file, not
the caller. On nuget.org, signed in as the account that owns the reserved
`WeAmp.*` prefix: Trusted Publishing, add a policy:

| Field | Value |
|---|---|
| Repository Owner | `We-Amp` |
| Repository | `pagespeed-optimizer` |
| Workflow File | `pipeline-release-nuget.yml` |
| Environment | `release` |

Until the policy exists `publish-nuget` fails at the token exchange; dry
runs are unaffected. The first successful publish locks the policy to the
repository's ids.

## Runners

Fleet jobs (`fleet-heavy`, `fleet-windows`) follow `docs/ci/pipelines.md`:
the routing expression, the `FLEET_ENABLED` guard and a hosted twin per job,
so a release never waits for the fleet. Optional runner-side facilities
(remote Bazel cache, Windows output root) come from the runner environment
(`docs/ci/runner-environment.md`). macOS and Linux arm64 build on hosted
runners. Jobs pass files only as workflow artifacts:

| Artifact | From | To |
|---|---|---|
| `release-source` | `vendor` | every build and test job (source + `vendor/` + `GIT_COMMIT`, one tarball) |
| `native-linux-x64`, `native-linux-arm64`, `native-osx-arm64` | the builds | `package` |
| `native-win-x64-unsigned` | Windows build | `sign-windows`; `package` in a dry run |
| `native-win-x64` | `sign-windows` | `package` in a real run |
| `nupkg-<sha>` | `package` | `publish-nuget` |
| `optimizer-*`, `optimizer-release-dist` | optimizer builds, `manifest` | `manifest`, `release` |
