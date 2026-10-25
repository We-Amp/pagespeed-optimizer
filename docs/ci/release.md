<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Release lanes

`release.yml` is a thin caller (see `docs/ci/pipelines.md`). A tag selects
one of two lanes, each a reusable workflow pinned to `main`:

| Tag | Lane | Publishes |
|---|---|---|
| `optimizer-v<version>` | `pipeline-release-optimizer.yml` | a GitHub release with the host packages: deb and rpm for amd64 and arm64, a Windows x64 zip, `SHA256SUMS` and `SHA256SUMS.asc` |
| `v<version>` | `pipeline-release-nuget.yml` | seven NuGet packages on nuget.org (WeAmp.PageSpeed, WeAmp.PageSpeed.AspNetCore, WeAmp.PageSpeed.NativeAssets.Linux / .Windows / .macOS, and the sidecar packages WeAmp.PageSpeed.Sidecar / WeAmp.PageSpeed.Sidecar.NativeAssets.Linux) and a draft GitHub release with the packages, SBOM, VEX, `SHA256SUMS` and `SHA256SUMS.asc` |

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

- Optimizer lane: the four packages, the win-x64 zip and `SHA256SUMS`, validated fail-closed
  (artifact `optimizer-release-dist`). The tag may not exist yet; the lane
  then builds the dispatched ref. The ref guard reports its verdict and
  does not stop a dry run.
- NuGet lane: every build and test and the seven `.nupkg` files (artifact
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
  tag exists, a mismatch fails the dry run too, and such a dry run says so
  in a `::notice::` that names the way to rehearse an unfinished release: a
  dry-run dispatch of a tag not yet on the remote (`optimizer-v2.5.0-pre.N`).

From 2.5.0 the module release pairs with `optimizer-v` plus the same version,
so the cut runs in this order: bump `VERSION.txt` to the release version on
`main`, tag `optimizer-v<X>` on that commit, record the pin in the module
repository (`install/debian/optimizer-pin.json`), then tag `module-v<X>`
there (its `docs/ci/release.md` has the module side). Before `v<X>` is
tagged (or released from an existing tag), record that module release in
`tools/ci/sidecar-pin.json` on `main` (see "The sidecar pin"). Once the module
packages are published, `MODULE_PACKAGE_VERSION` in
`website/src/data/product-facts.mjs` (1.19.0 today) moves to the same
version: the scan's lifecycle sentence (`website/src/lib/scan/lifecycle.ts`)
compares a detected `X-Mod-Pagespeed` header against it, so until it moves a
1.19.0 install is never told that a newer release exists, and
`module-package-version.test.ts` compares it with the live package index.
Known stale wording, left for the website copy pass: the 2.0.x sentence
there says a setup "moves to 2.1 today", which reads wrong once 2.5.0 is the
current release.

## The CI gate

Both lanes run a hosted job, `CI run of the release commit`
(`ci-release-gate`), that executes `tools/ci/ci_release_gate.sh` from a
sparse checkout of `main`. The optimizer `manifest` job and the NuGet
`package` and `package-dry-run` jobs need it, so nothing is assembled
or published without it. It passes when a `ci.yml` run on `main` with
`head_sha` equal to the release commit (event `push` or `workflow_dispatch`)
concluded `success` and these jobs of the latest attempt are all `success`:

- `pr / Linux Build`, `pr / Linux Unit Tests`, `pr / Windows Build`,
  `pr / Windows Unit Tests`, `pr / Lint (clang-tidy + clang-format)`;
- `full / Linux Build`, `full / Windows Build`, `full / Linux ASan + UBSan`,
  `full / Linux TSan`, `full / Lint (whole tree)`.

No other job of that run may be `failure`, `cancelled` or `timed_out`, except
`full / Daemon Privilege Rig (arm64)`, which is `continue-on-error`. A
`full=true` dispatch of `CI` on the release commit satisfies this. A push run
does not: a push to `main` skips `full / Lint (whole tree)` (the whole-tree lint
for `main` runs in the nightly), so a release always needs the dispatch.

The gate polls a run that is still in flight (up to 60 minutes), retries
transient API errors, and refuses after two consecutive checks that find
completed runs and no passing one, or when the API keeps failing. The log ends
with `ci gate passed in ci.yml run <id> (<event>)` or
`ci gate refuses this release: <reason>`. On a dry run a refusal that the
gate reaches is a `::warning::` and the job stays green, so read the log line,
not the job colour. A dry run of an optimizer tag that is not on the remote yet
checks the commit of the dispatched ref. The 75-minute job timeout still turns
a dry run red. The self-test is `tools/ci/test_ci_release_gate.sh`.

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
| optimizer | `sign-checksums` | signs `SHA256SUMS` with the release signing key (`RELEASE_GPG_*`) |
| optimizer | `release` | creates the GitHub release |
| NuGet | `sign-windows` | Authenticode signing with the six `AZURE_*` secrets |
| NuGet | `package` | draft GitHub release, the two notifications (`CORP_DISPATCH_TOKEN`) |
| NuGet | `sign-checksums` | signs `SHA256SUMS` with the release signing key (`RELEASE_GPG_*`) |
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

Environment secrets of `release`. The lane that uses a secret declares it
(none required), and `release.yml` passes each one explicitly (never
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
| `RELEASE_GPG_KEY` | `sign-checksums` (both lanes) | the release signing key, fingerprint `411DEFDB571351D2B25AEFCC9C02B5BDCCE75313`: the armored secret key, base64 encoded, a single secret key |
| `RELEASE_GPG_PASSPHRASE` | `sign-checksums` (both lanes) | its passphrase |

Until the owner adds `RELEASE_GPG_KEY` and `RELEASE_GPG_PASSPHRASE`, a real
run of either lane fails at `sign-checksums` with a message that names them,
and nothing is published: the optimizer lane creates no release, and the
NuGet lane leaves its draft unsigned and pushes nothing to nuget.org.

## Signed checksums

`SHA256SUMS` of every release is signed with the We-Amp release signing key,
the same key that signs the mod_pagespeed module releases. Its public half is
`RELEASE-SIGNING-KEY.asc` at the repository root. `sign-checksums` runs
`tools/ci/release_checksums_sig.sh` (self-test:
`tools/ci/test_release_checksums_sig.sh`) from a checkout of `main`:

- `sign` refuses unless the secret holds exactly one secret key whose full
  fingerprint is `411DEFDB571351D2B25AEFCC9C02B5BDCCE75313`;
- `verify` imports only `RELEASE-SIGNING-KEY.asc` (which must hold exactly
  that one key) into a fresh keyring and requires exactly one good signature
  (`GOODSIG`), no revoked or expired key or signature (`REVKEYSIG`,
  `EXPKEYSIG`, `EXPSIG`, `KEYREVOKED`, `ERRSIG`, `BADSIG`), and the `VALIDSIG`
  primary-key fingerprint of `SHA256SUMS.asc` to be exactly that fingerprint.

The pin is a consistency check between the signing secret and the committed
key, not an independent trust root: whoever can change `main` can change
both. The script honours a different fingerprint only in its self-test.

In the NuGet lane the draft already exists when the checksums are signed:
`attach-signature` downloads every asset of the draft, checks that its
`SHA256SUMS` is the signed file, that the draft holds exactly the files it
lists and that they pass `sha256sum -c`, then adds `SHA256SUMS.asc`;
`publish-nuget` waits for it. A draft with extra assets (for example one
an operator created by hand) therefore fails this check.
A new key changes the pin in the script, `RELEASE-SIGNING-KEY.asc` and this
page together.

To verify a download:

```bash
gpg --show-keys --with-fingerprint RELEASE-SIGNING-KEY.asc   # 411D EFDB 5713 51D2 B25A  EFCC 9C02 B5BD CCE7 5313
gpg --import RELEASE-SIGNING-KEY.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum --check --ignore-missing SHA256SUMS
```

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

## The sidecar pin

The NuGet lane also publishes the ASP.NET Core sidecar packages,
`WeAmp.PageSpeed.Sidecar` and `WeAmp.PageSpeed.Sidecar.NativeAssets.Linux`.
Their native pair (nginx, the module built against that nginx and the
launch shim, for linux-x64 and linux-arm64) is built, tested (live nginx
tests included) and signed in the mod_pagespeed module release, as
`pagespeed-sidecar-native-<X>-linux-<arch>.tar.gz` described by
`sidecar-pair.json`; their managed code is `aspnetcore/` of that release's
source tarball, `mod_pagespeed-<X>.tar.gz`. The lane builds neither from a
branch of the module repository: the `sidecar-pack` job takes both from the
module release named by `tools/ci/sidecar-pin.json`:

| Field | Value |
|---|---|
| `tag` | the module release tag, `module-v<X>` |
| `sha256sums_sha256` | sha256 of that release's `SHA256SUMS` |
| `sidecar_pair_sha256` | sha256 of that release's `sidecar-pair.json` |
| `release_key_fingerprint` | `411DEFDB571351D2B25AEFCC9C02B5BDCCE75313`, which must equal the fingerprint pinned in `tools/ci/release_checksums_sig.sh` |

`tools/ci/sidecar_fetch.sh fetch` downloads `SHA256SUMS`, `SHA256SUMS.asc`,
`sidecar-pair.json`, the two pair tarballs and the source tarball of exactly
that tag (never "latest"), and refuses unless the `SHA256SUMS` digest is the
pinned one, `SHA256SUMS.asc` verifies against `RELEASE-SIGNING-KEY.asc` with
`release_checksums_sig.sh verify`, the `sidecar-pair.json` digest is the
pinned one and `SHA256SUMS` lists it, the manifest names the pinned tag and
version and both RIDs, every pair tarball matches the name, size and sha256
in the manifest and in `SHA256SUMS` and holds exactly the files the
manifest lists with their digests, and the source tarball has the digest
`SHA256SUMS` lists. `stage` then writes `aspnetcore/` of the source tarball
(regular files and directories only) with the pair of each RID (`nginx`,
`ngx_pagespeed_module.so`, `pagespeed-nginx-launch`, `build-info.json`) in
`WeAmp.PageSpeed.Sidecar.NativeAssets.Linux/runtimes/<rid>/native`. In the
same job, so no verified input crosses a job boundary, `sidecar-pack` runs
the sidecar test suite under .NET 8 and .NET 10 (locked restore; every test
must run and pass). The live nginx tests run against the verified pair of
the runner's RID, never a host nginx: the pair is copied to
`runtimes/<rid>/native` next to the test assembly, where the sidecar finds
it before any system path, `PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX=1` makes
the tests require it, `libpcre3` is installed only if the runner lacks it,
and every live test must report the bundled nginx (at least eight per
framework). The job then packs both packages with `-p:IncludeNet10=true` at the
version of the `v<X>` tag (no fourth digit), and `check-packages` refuses
unless the native package carries exactly the four files per RID at the
digests of `sidecar-pair.json` plus its `buildTransitive` targets and the
license files, and the managed package carries `lib/net8.0` and
`lib/net10.0` and depends on the native package at the same version.
`package` waits for `sidecar-pack`, adds the two packages to its five and
requires seven; from there they take the path of the others: the
source-control metadata guard, `SHA256SUMS` and its signature, the draft
release and `publish-nuget`. A dry run runs the same path and pushes
nothing. When a dry run's pin names another version than its tag, the
sidecar packages carry the tag's version but contain the pinned module
release: the run says so in its step summary, in the artifact name
(`sidecar-nupkg-<sha>-contains-module-v<pinned>`) and in a
`README-SIDECAR-PIN.txt` that travels with the packages into `nupkg-<sha>`.

The pin, the script and the key are read from `main`, like the signing
script, because the module release of a version is cut after the `v<X>`
commit. A real run of `v<X>` refuses a pin other than `module-v<X>`; a dry
run warns and uses the pinned release. Two consequences:

- A real `v<X>` requires a published `module-v<X>` pinned on `main`. A
  version the module release skips (for example 2.5.1) gets no NuGet
  release.
- Because the pin is read from `main`, a real re-run of an older `v<X>`
  after the pin has moved on fails at the pin check: restore the pin to
  `module-v<X>` on `main` first, or rehearse it as a dry run.

To record the pin after the module release is published:

```bash
gh release download module-v<X> --repo We-Amp/mod_pagespeed --dir sidecar \
  -p SHA256SUMS -p SHA256SUMS.asc -p sidecar-pair.json \
  -p 'pagespeed-sidecar-native-<X>-linux-*.tar.gz' -p 'mod_pagespeed-<X>.tar.gz'
sha256sum sidecar/SHA256SUMS sidecar/sidecar-pair.json   # the two pinned digests
# edit tools/ci/sidecar-pin.json, then:
tools/ci/sidecar_fetch.sh verify tools/ci/sidecar-pin.json sidecar RELEASE-SIGNING-KEY.asc
```

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

nuget.org scopes the policy to the package owner, not to package ids. Before
the first real run that publishes the sidecar packages, the owner:

- confirms in the nuget.org UI that this policy covers
  `WeAmp.PageSpeed.Sidecar` and `WeAmp.PageSpeed.Sidecar.NativeAssets.Linux`
  (both owned by the same account as the other five packages);
- deletes the retired policy for the former private repository's sidecar
  workflow (`sidecar-nuget.yml`, environment `1.15 sidecar nuget`): it would
  still let a workflow in a retired repository push any package the account
  owns, the sidecar packages included.

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
| `sidecar-nupkg-<sha>` (dry run with another pin: `sidecar-nupkg-<sha>-contains-module-v<pinned>`) | `sidecar-pack` | `package` |
| `sidecar-test-results-<sha>` | `sidecar-pack` | kept for inspection |
| `nupkg-<sha>` | `package` | `publish-nuget` |
| `release-sums-<sha>` | `package` (real run) | `sign-checksums`, `attach-signature` |
| `release-signature-<sha>` | `sign-checksums` | `attach-signature` |
| `optimizer-*`, `optimizer-release-dist` | optimizer builds, `manifest` | `manifest`, `sign-checksums`, `release` |
| `optimizer-release-signature` | `sign-checksums` | `release` |
