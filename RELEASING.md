# Releasing PageSpeed 2.0

## Every user-facing change carries its note (enforced)

Release notes are not written at release time. They accumulate, one pull
request at a time, and CI enforces that.

`tools/ci/check_release_note.sh` runs on every pull request as part of the
**License Headers + SBOM Drift + CVE Scan** job. If the diff touches `src/` or
`lib/` — excluding tests and build files — it requires that the same pull
request also touches `CHANGELOG.md`. Add the entry under the unreleased heading
at the top, written for someone deciding whether to upgrade: what they observed
before, what they observe now.

If the change genuinely cannot be observed by a user, say so in one line:

```
Release-Note: none — <why this is invisible to users>
```

Put it in a commit message (pushing re-runs the check) or in the pull request
description (editing a description does not re-trigger CI, so re-run the job).
The reason is mandatory — the waiver records a judgement, it is not a mute
button.

Run it yourself before pushing:

```bash
bash tools/ci/check_release_note.sh --base origin/main --head HEAD
bash tools/ci/check_release_note.sh --self-test   # the gate's own tests
```

The gate exists because the failure it prevents is silent. A fix that ships
without a note leaves everyone still working around it, and nothing anywhere
goes red to say so.

## Cutting a release

A release is a single **version-bump commit** on `main` followed by a
`v<X.Y.Z>` tag. **Tagging `origin/main` directly does NOT work**: `release.yml`'s
first job rejects any tag whose `VERSION.txt` differs from it, and
`VersionInvariantTests` + the SBOM `--verify` gate additionally require
`VERSION.txt`, the .NET package `<Version>`, the Helm `appVersion` and the SBOM
`versionInfo` to **all agree**. Each disagreement is a wasted CI cycle.

Use the helper — it bumps every version-carrying file in lockstep, regenerates
the SBOM, validates that they agree, then commits and tags:

```bash
# 1. Write this release's notes under a "## Unreleased" heading in CHANGELOG.md.
# 2. From a clean checkout of main:
tools/cut-release.sh 2.0.29              # commits + tags locally; review the diff.
# 3. `main` is protected by a `pull_request` ruleset — the bump commit CANNOT be
#    pushed directly. Land it via a squash-merged PR (0 approvals required):
git push origin HEAD:release/2.0.29
gh pr create --base main --head release/2.0.29 --title "release: bump to 2.0.29" --fill
gh pr merge --squash --delete-branch     # main now carries the bump
# 4. Push the tag separately to trigger release.yml. It builds from the tag
#    commit; the squashed main commit is content-identical (the off-main tag
#    commit is a cosmetic provenance wart — release.yml and deploy-all.sh both
#    build from the TAG, not from main):
git push origin v2.0.29
```

> **`cut-release.sh --push` and a direct `git push origin HEAD:main` are now
> REJECTED** by the `main` ruleset (added after 2.0.30; 2.0.29/2.0.30 were
> direct pushes). Always route the bump through a PR as above.

> **⚠️ There is NO no-side-effect "rehearsal" of a 2.0 release via CI.** The
> `publish-nuget` job pushes to nuget.org on **every** green build — tag push
> *or* `workflow_dispatch` — and is **not** gated by `auto_promote` (that input
> only controls the draft→published *GitHub Release* flip). A dispatched "test"
> run with a valid bumped tag **will** publish to nuget.org, which is immutable
> (you can only unlist afterward). To rehearse without side effects: run
> `tools/cut-release.sh X.Y.Z` (no `--push`) + `tools/generate-sbom.py --verify`
> **locally**, or dispatch with `docker_only=true` (builds/archives the Docker
> artifacts only — no release, no nuget push).

The script bumps, in lockstep:

| File | What |
|---|---|
| `VERSION.txt` | `X.Y.Z` — worker `/v1/health` and the release-tag gate read this |
| `CHANGELOG.md` | renames `## Unreleased` → `## X.Y.Z — DATE` |
| `deploy/helm/pagespeed/Chart.yaml` | `appVersion: "X.Y.Z"` (image tags track appVersion since #729) |
| `samples/aspnetcore/Directory.Build.props` | `<Version>` + a new `PackageReleaseNotes` line |
| `sbom/mod_pagespeed-2.1.spdx.json` | regenerated via `tools/generate-sbom.py` |

It does **not** bump the Helm chart's own `version:` (only `appVersion`) — bump
that by hand in `Chart.yaml` if the chart templates/values changed. It also does
not touch the per-release Docker-image-tag **examples** in
`website/src/content/docs/installation-docker.md` (and the docs HowTo JSON-LD in
`website/src/pages/docs/[slug].astro`) — those are bumped separately and guarded
by the CI drift check.

After the tag pushes, `release.yml` builds, signs and publishes (nuget, packages,
and a **draft** GitHub release). Then:

- Promote the draft: `gh release edit vX.Y.Z --draft=false`.
- The website deployment (fired on the release dispatch) bumps
  modpagespeed.com's manifest-driven version strings. **Verify it actually
  landed** — confirm the `website/` release manifest semver matches and the
  live site shows the new version; the inline sync has historically no-op'd
  silently.
- Our own hosting runs the new worker only after a deploy that **rebuilds from
  the tagged commit**.
- **Publish the public Docker images (REQUIRED — not automatic).** The
  release `repository_dispatch` only ever runs `publish-images.yml` in
  `dry-run`; you must publish ghcr explicitly:
  `gh workflow run publish-images.yml -R We-Amp/modpagespeed-images -f version=X.Y.Z -f sha=<full 40-char tag commit sha> -f mode=publish`,
  then **approve the `production` environment gate**. Verify
  `ghcr.io/we-amp/pagespeed-{nginx,worker,combined}:X.Y.Z` are pullable.
- **Publish the Helm chart (REQUIRED — not automatic).** `cut-release.sh`
  bumps `Chart.yaml` but does not package it. Run `deploy/helm/package-chart.sh`,
  commit the new `website/public/charts/pagespeed-<chart-version>.tgz` +
  regenerated `index.yaml` (via PR — main is ruleset-protected), then redeploy
  the site so it's served.
  Verify `https://modpagespeed.com/charts/index.yaml` lists the new appVersion.
- **Bump the install-docker ghcr tag examples** (cut-release does not):
  `website/src/content/docs/installation-docker.md` and
  `website/src/pages/docs/[slug].astro` → `ghcr.io/we-amp/...:X.Y.Z`, then redeploy
  the site. (These point users at ghcr tags that must exist — publish ghcr first.)

## Pre-release Checklist

Items marked **[CI]** are enforced automatically; see "CI gate coverage" below.
Items marked **[manual]** still require operator/legal action before tagging.

- [x] **[CI: ci.yml `license-and-sbom-hygiene` + release.yml `security-gates`]** `tools/check-license-headers.sh --all` passes
- [x] **[CI: ci.yml `rat` (`License audit (Apache RAT)`), routed through the `Required Checks Gate` aggregator]** `tools/ci/rat.sh` (Apache RAT 0.16.1) reports 0 Unknown Licenses: every file carries a recognised header or is listed with a reason in `.rat-excludes`
- [x] **[CI: release.yml `security-gates`]** `tools/generate-sbom.py` produces valid SPDX 2.3 output (`tools/sbom/check-spdx-structure.py`: version, root id, valid and unique element ids, resolvable relationships)
- [x] **[CI: ci.yml `license-and-sbom-hygiene` + release.yml `security-gates`]** `tools/generate-sbom.py --verify` shows no drift
- [x] **[CI: release.yml `security-gates`]** `grype` vulnerability scan: no unmitigated High/Critical (threshold configurable via `workflow_dispatch` input `grype_severity`)
- [x] **[CI: ci.yml + release.yml]** VEX document is well-formed OpenVEX; statements applied to grype to suppress documented false positives
- [x] **[CI: ci.yml `license-and-sbom-hygiene`]** THIRD-PARTY-NOTICES versions match the SBOM (`tools/sbom/check-third-party-notices.py`: an entry whose version differs from its SBOM package, or an SBOM package without an entry, fails the job)
- [ ] **[manual]** `python3 tools/sbom/check-third-party-notices.py --links` passes — HTTP HEAD on every license link in THIRD-PARTY-NOTICES; it needs the network, so it runs by hand before tagging rather than in CI. License identifiers and copyright holders are prose and still need a human read whenever a dependency is added or re-pinned.
- [ ] **[manual]** Legal sign-off obtained
- [ ] **[manual]** "Never phones home" invariant re-confirmed: the daemon opens no outbound connection of its own accord (no instance heartbeat, no telemetry); only operator-configured fetches (origin content, configured Web Bot Auth / RSL-CAP key directories) may egress. Any change touching outbound HTTP requires re-checking the published Terms "Data processing" clause.
- [x] **[CI: release.yml `linux-release-build` builds against `--vendor_dir=vendor` from the tarball]** Source tarball builds hermetically
- [x] **[CI: release.yml `windows-release-build`]** Embedded Windows binaries (`factory_worker.exe`, `pagespeed.dll`) Authenticode-signed via Microsoft Trusted Signing (Public Trust, client-secret auth — reuses 1.1's account)
- [ ] **[deferred to post-v1.0.0, see "Operator action" below]** Outer `.nupkg` files signed for nuget.org — Trusted Signing's 3-day cert rotation is incompatible with nuget.org's account-cert registration model (NuGet/NuGetGallery#10027, open as of 2026-05). Path: procure a CA-issued code-signing cert in Azure Key Vault, sign via `dotnet/sign code azure-key-vault`.
- [x] **[CI: release.yml `publish-nuget`]** `.nupkg` packages pushed to `nuget.org` via Trusted Publishing (OIDC; no long-lived `NUGET_API_KEY` secret). Account must own the reserved `WeAmp.*` prefix. Outer-package signatures absent at v1.0.0 — nuget.org accepts first-time submissions without one.
- [x] **[CI: release.yml `promote-release`, opt-in]** Draft → published flip (controlled by `workflow_dispatch` input `auto_promote`; default `false` leaves release in draft for human review)
- [ ] **[manual, out of scope for the GitHub release workflow — Docker image build/publish lives in `docker/build-release.sh` invoked at deploy time]** Docker images contain license + SBOM files
- [ ] **[manual, requires Sigstore/cosign secrets — see "Operator action" below]** Docker images signed with cosign
- [ ] **[manual, requires GPG release-signing-key secret — see "Operator action" below]** Source tarball signed with GPG

### CI gate coverage

| Gate | Where | Trigger | On failure |
|---|---|---|---|
| License headers (Apache-2.0 SPDX in every source file) | CI job `license-and-sbom-hygiene` | every PR + main push | blocks merge |
| Whole-tree license audit (Apache RAT: recognised header or reasoned `.rat-excludes` entry for every file; report uploaded as artifact) | CI job `rat` (`License audit (Apache RAT)`) | every PR + main push | blocks merge (in the `Required Checks Gate` aggregator's `needs:`) |
| SBOM drift (`tools/generate-sbom.py --verify`) | same | every PR + main push | blocks merge |
| THIRD-PARTY-NOTICES drift (`tools/sbom/check-third-party-notices.py`: entry versions vs SBOM packages, both directions) | same | every PR + main push | blocks merge |
| VEX well-formed (OpenVEX `@context`, required fields, valid `status`) | same | every PR + main push | blocks merge |
| License headers (defense-in-depth) | the release lane's `security-gates` job | every release | blocks `package` job |
| SBOM regeneration matches committed copy | same | every release | blocks `package` job |
| SPDX 2.3 structural sanity (`tools/sbom/check-spdx-structure.py`: valid + unique SPDXIDs, resolvable relationships) | same | every release | blocks `package` job |
| `grype` CVE scan on SPDX SBOM with VEX suppressions | same | every release | blocks `package` job (default threshold: High) |
| SBOM + grype SARIF uploaded as workflow artifact | same | every release (always) | n/a (artifact for legal/audit) |
| Windows native binary Authenticode signing (`factory_worker.exe` + `pagespeed.dll` via Microsoft Trusted Signing, Public Trust, client-secret) | `release.yml` job `windows-release-build` step `Authenticode-sign Windows native binaries` | every release | blocks Windows binary upload to GCS |
| Push `.nupkg` (Linux/macOS/Windows native asset packages + managed packages) to `nuget.org` (NuGet Trusted Publishing, OIDC, `--skip-duplicate`) | `release.yml` job `publish-nuget` | every release | blocks `promote-release` |
| Promote draft → published GitHub Release | `release.yml` job `promote-release` | every release, opt-in via `workflow_dispatch` input `auto_promote=true` | default `false` leaves release as draft for human review |

### Operator action required (one-time, before first release)

These gates are NOT in CI because they need secrets/setup only the operator can mint.
File secrets with `gh secret set …` on this repository.

**Docker / source-tarball signing (still `[manual]` until wired):**

- **`COSIGN_KEY`** + **`COSIGN_PASSWORD`** — for `cosign sign` of `ghcr.io/we-amp/pagespeed-worker:$VERSION` and `ghcr.io/we-amp/pagespeed-nginx:$VERSION`. Alternative: switch to keyless OIDC and grant `id-token: write` on the publishing job.
- **`GPG_PRIVATE_KEY`** + **`GPG_PASSPHRASE`** — for `gpg --detach-sign` of the source tarball.

Once those land, the Docker-sign and tarball-sign steps can move into `release.yml` as additional release-gates. They are documented as `[manual]` above until then.

**Outer `.nupkg` signing — deferred to post-v1.0.0 (separate Azure Key Vault cert needed):**

`Microsoft Trusted Signing rotates the certificate every 3 days; nuget.org Account Settings requires a single, long-lived code-signing certificate registered up front. The two models are presently incompatible (NuGet/NuGetGallery#10027 — open as of 2026-05). v1.0.0 ships outer `.nupkg` UNSIGNED to nuget.org (accepted on first-time submission; nuget.org also accepts unsigned packages indefinitely if no cert is ever registered against the account). For durable post-GA outer-package signing the path is: procure a standard CA-issued code-signing cert from DigiCert / Sectigo / Certum, provision in Azure Key Vault, sign via `dotnet/sign code azure-key-vault` (the dominant pattern across Polly, CoreWCF, Humanizer, NuGetPackageExplorer). Track NuGet/NuGetGallery#10027 in case Trusted Signing becomes nuget.org-compatible.

**Microsoft Trusted Signing — Azure-side setup (one-time, for `windows-release-build`'s Authenticode signing step):**

The Azure-side resources are **shared with the 1.1 line**'s IIS DLL + MSI signing — the Trusted Signing account, Public-Trust certificate profile, and App Registration / Service Principal are already provisioned. **No new Azure work needed**; just file the same six secrets on this repository. (Secrets can't be copied across repos via API — re-add them via `gh secret set` or the GitHub UI.)

The 1.1 workflow uses **client-secret authentication** via the `DefaultAzureCredential` env vars; we mirror that pattern here for consistency and to reuse the existing Service Principal without adding a federated-identity credential.

Required GitHub repo secrets (same names + same values as 1.1):

- `AZURE_CLIENT_ID` — App Registration client ID
- `AZURE_TENANT_ID` — Azure AD tenant ID
- `AZURE_CLIENT_SECRET` — App Registration client secret
- `AZURE_TRUSTED_SIGNING_ENDPOINT` — e.g. `https://eus.codesigning.azure.net/`
- `AZURE_TRUSTED_SIGNING_ACCOUNT` — Trusted Signing account name
- `AZURE_TRUSTED_SIGNING_PROFILE` — Public-Trust certificate profile name

**NuGet Trusted Publishing — nuget.org-side setup (one-time, for `publish-nuget`):**

1. Sign in to `nuget.org` under the account that **owns the reserved `WeAmp.*` prefix** (already reserved per Otto's Phase D2 decision — no new application work needed; the publish must happen under this account).
2. Profile → API Keys → **Manage Trusted Publishing** → Add new.
3. Repository: `We-Amp/pagespeed-optimizer`, Workflow: `release.yml`, **Environment: `2.2 CI release`** — the workflow's `publish-nuget` job declares `environment: "2.2 CI release"`, and nuget.org Trusted Publishing matches on environment. The corresponding GitHub Actions environment is provisioned in repo Settings → Environments (currently empty / no protection rules — add required-reviewers there if you want approval-gated releases).
4. No long-lived `NUGET_API_KEY` secret needed — `NuGet/login-action@v1` exchanges the workflow's OIDC token for a short-lived API key per run.

**GitHub repo configuration:**

- `permissions: id-token: write` is declared on `publish-nuget` (for `NuGet/login-action`). `windows-release-build`'s Authenticode signing step uses client-secret auth and does NOT need `id-token: write`.
- `contents: write` (already at workflow level) covers `gh release create` (in the `package` job) and `gh release edit --draft=false` (in `promote-release`).
- The `2.2 CI release` Actions environment must exist on the repo (Settings → Environments → New). It can be empty — no protection rules required for v1.0.0 — but the environment name must match between the workflow `environment:` field and the nuget.org Trusted Publisher entry.

## Source Tarball

```bash
# Generate source tarball (includes vendored deps, excludes reference/mod_pagespeed/)
./tools/create-source-tarball.sh

# Verify: tarball builds hermetically (no internet)
tar xf mod_pagespeed-2.1-$VERSION-src.tar.gz
cd mod_pagespeed-2.1-$VERSION-src && bazel build //...
```

Release artifact naming (2.1 line): `mod_pagespeed-2.1-<version>-<os>-<arch>`
for binary archives and the directories they unpack to, and
`mod_pagespeed-2.1-<version>-src` for the source tarball (which carries no
os/arch). The `mod_pagespeed-2.1` prefix is the same identity the NOTICE
product line and the SBOM root carry. The frozen 2.0 line keeps its
historical `pagespeed-2.0-*` names; a 2.0 security reship is cut from the
`release/2.0` branch and never adopts the new slug.

The tarball includes:
- All source code with license headers
- LICENSE, NOTICE, THIRD-PARTY-NOTICES
- SBOM file (sbom/mod_pagespeed-2.1.spdx.json)
- Vendored dependencies (from `bazel vendor`)

Excludes:
- `reference/mod_pagespeed/` (large, not needed to build)
- `.git/`, `node_modules/`, `.venv/`, `bazel-*` output directories

## Docker Images

> **Public images:** the customer-facing images are published from CI,
> not from these manual steps. The product `release.yml` archives the
> Docker-flavored binaries to the release-artifact store, and
> the `publish-images.yml` workflow in `We-Amp/modpagespeed-images` assembles,
> smoke-tests, **keyless-cosign-signs** (no `COSIGN_KEY`), SBOM-attests and
> pushes `ghcr.io/we-amp/pagespeed-{worker,nginx,combined}`. The publish job is
> manually gated. The commands below are the legacy local build/sign flow.

```bash
# Build release images
./docker/build-release.sh $VERSION

# Verify license files in images
docker run --rm ghcr.io/we-amp/pagespeed-worker:$VERSION ls /usr/share/doc/pagespeed/
# Expected: LICENSE  NOTICE  THIRD-PARTY-NOTICES  sbom.spdx.json  sbom.cdx.json

# Generate per-image SBOMs
syft ghcr.io/we-amp/pagespeed-worker:$VERSION -o spdx-json > sbom/docker-worker.spdx.json
syft ghcr.io/we-amp/pagespeed-nginx:$VERSION -o spdx-json > sbom/docker-nginx.spdx.json

# Sign images
cosign sign ghcr.io/we-amp/pagespeed-worker:$VERSION
cosign sign ghcr.io/we-amp/pagespeed-nginx:$VERSION

# Verify signatures
cosign verify ghcr.io/we-amp/pagespeed-worker:$VERSION
```

## Helm Chart

The chart is published as a classic Helm repository under the public website,
served at `https://modpagespeed.com/charts`. On a release, bump the pinned image
tags and chart version, re-package, and redeploy the website (which serves the
chart assets).

```bash
# 1. appVersion is already bumped by tools/cut-release.sh. Since #729 the worker
#    AND nginx images BOTH track appVersion via the `pagespeed.imageTag` helper —
#    there are no separate worker.image.tag / nginx.image.tag keys to bump (a
#    per-image tag override is `fail`-rejected by the chart).
# 2. Bump the chart's own `version:` in deploy/helm/pagespeed/Chart.yaml only if
#    the chart templates/values changed this release (cut-release.sh leaves it).

# 3. Re-package the chart + regenerate the repo index (committed assets)
deploy/helm/package-chart.sh
#    -> writes website/public/charts/pagespeed-<version>.tgz + index.yaml

# 4. Commit the regenerated website/public/charts/ assets, then deploy the
#    website (rebuilds + serves /charts/).

# 5. Verify the live repo resolves the new version
helm repo add weamp https://modpagespeed.com/charts && helm repo update
helm search repo weamp/pagespeed   # CHART VERSION + APP VERSION should match
```

`package-chart.sh` keeps every packaged version in `website/public/charts/` and
rebuilds `index.yaml` over all of them, so older chart versions stay installable.

## Binary Release

```bash
./tools/create-binary-release.sh $VERSION
```

Produces:
```
mod_pagespeed-2.1-$VERSION-linux-amd64/
  bin/pagespeed-worker
  modules/ngx_pagespeed_module.so
  console/          (built SPA)
  LICENSE
  NOTICE
  THIRD-PARTY-NOTICES
  sbom.spdx.json
  sbom.cdx.json
```

## Vulnerability Scanning

```bash
# Scan source dependencies
grype sbom:sbom/mod_pagespeed-2.1.spdx.json --vex sbom/mod_pagespeed-2.1.vex.json

# Scan Docker images
grype sbom:sbom/docker-worker.spdx.json
grype sbom:sbom/docker-nginx.spdx.json
```

### Vulnerability SLA

| Severity | Timeline |
|----------|----------|
| Critical/High | Patch or mitigate within 30 days |
| Medium | Within 90 days |
| Low | Next scheduled release |

## Signing

### Docker Images

Signed with [cosign](https://github.com/sigstore/cosign) (Sigstore). Signatures stored in the OCI registry alongside images.

### Source Tarball

GPG-signed with We-Amp's release signing key. Verify with:
```bash
gpg --verify mod_pagespeed-2.1-$VERSION-src.tar.gz.asc mod_pagespeed-2.1-$VERSION-src.tar.gz
```

## SBOM Maintenance

When adding or updating dependencies:
1. Update `MODULE.bazel` (C++), `Cargo.toml` (Rust), or `package.json` (JS)
2. Update `THIRD-PARTY-NOTICES` with new dependency info: name, built version, license identifier, copyright holder(s) as they appear in the component's license file, and a link to that file at the pinned tag or commit
3. Run `tools/generate-sbom.py` to regenerate SBOMs (the Rust and JavaScript rows are curated tables in the script; keep them at the lockfile versions)
4. Verify: `tools/generate-sbom.py --verify`
5. Verify: `tools/sbom/check-third-party-notices.py --links`

## Annual Review

- Update copyright year range in license headers
- Review for new compliance requirements
- Audit dependency licenses for any changes
