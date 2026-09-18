# Releasing mod_pagespeed 2.1

Releases of the pagespeed-optimizer host packages are cut with the `Release`
workflow (`release.yml`). The lane builds the optimization daemon and the
`libpagespeed.so` client library natively on amd64 and arm64, packages them
as deb and rpm, validates the result fail-closed, and publishes a GitHub
release. It runs on GitHub-hosted runners only.

## Every user-facing change carries its note (enforced)

Release notes are not written at release time. They accumulate, one pull
request at a time, and CI enforces that.

`tools/ci/check_release_note.sh` runs on every pull request. If the diff
touches `src/` or `lib/` — excluding tests and build files — it requires
that the same pull request also touches `CHANGELOG.md`. Add the entry under
the unreleased heading at the top, written for someone deciding whether to
upgrade: what they observed before, what they observe now.

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

A release is a tag push. The tag names the version and the lane builds from
exactly that commit:

```
optimizer-v<X.Y.Z>          e.g. optimizer-v1.16.0
optimizer-v<X.Y.Z>-<pre>    e.g. optimizer-v1.16.0-rc.1 (a prerelease)
```

The lane never creates the tag. Pushing `optimizer-v<version>` to the
repository is the releaser's act, and a real run refuses to start unless the
tag already exists on the remote; if the tag moved between resolution and
checkout, the run refuses rather than package the wrong tree under the tag's
name.

Two ways to start a run:

- **Push an `optimizer-v*` tag** — a real run: build, validate, publish.
- **Dispatch the workflow by hand** with inputs `tag` and `dry_run`:
  - `dry_run: true` (the default) builds all four packages, validates the
    asset set and SHA256SUMS fail-closed, and uploads the result as a
    workflow artifact. It creates NO release and NO tag, and it may name a
    tag that does not exist yet (it then builds the dispatched ref). This is
    the rehearsal — run it before every real release.
  - `dry_run: false` is a real run and requires the tag on the remote.

Runs are serialized per tag and never cancelled in progress: two real
releases for the same tag cannot race, and a release build always runs out.

### The five-asset contract

A real run publishes a GitHub release with exactly five assets — no more,
no fewer:

| Asset | Contents |
|---|---|
| `pagespeed-optimizer-<version>-1.x86_64.rpm` | daemon + client library, rpm hosts |
| `pagespeed-optimizer-<version>-1.aarch64.rpm` | the same, arm64 |
| `pagespeed-optimizer_<version>_amd64.deb` | the same, deb hosts |
| `pagespeed-optimizer_<version>_arm64.deb` | the same, arm64 |
| `SHA256SUMS` | checksums for the four packages |

The serving-module packages download these assets by name and verify them
against SHA256SUMS, so the set and the names are a contract. Asset names
carry the upstream SemVer spelling (`1.16.0-rc.1`); the package-internal
version keeps the dpkg/rpm tilde pre-release form (`1.16.0~rc.1`). Both are
derived from the tag, never hand-typed. A `-<pre>` tag produces a release
marked as a prerelease.

Validation is fail-closed at two points. The manifest job checks that the
set is exactly the five assets, all present and non-empty, that
`sha256sum -c` passes, and that the package-internal versions are the
derived tilde form. The release job re-verifies SHA256SUMS across the job
boundary before publishing anything.

### The glibc floor gate

The daemon binary is built inside the jammy-based development image
(`docker/Dockerfile`), never on the runner host. Jammy's static libc++
archives keep the binary at a GLIBC_2.34 floor — the RHEL 9 / Debian 12
install contract. A build on a newer base advertises a higher floor
(GLIBC_2.38) and the resulting packages refuse to install on those hosts,
so the manifest job asserts the floor mechanically: every deb must declare
exactly `libc6 (>= 2.34)`, and every rpm must require `glibc >= 2.34` with
nothing above it.

Both legs are native builds (amd64 and arm64 each on their own runner arch)
with clang-20 + libc++-20; the release build adds optimization, LTO and
binary hardening, and `tools/assert-binary-hardening.sh` re-checks the
hardening properties on both artifacts before they are packaged.

## The website tree

`website/` (the modpagespeed.com site) is imported wholesale from the site's
source-of-truth repository by the maintainer export flow; it is not edited
here, because a direct edit would be overwritten by the next import. The
import applies a token-only scrub for internal references, asserts none
remain, and runs this repository's public-tree hygiene gate
(`tools/ci/check-public-hygiene.sh`) over the result before anything is
committed.

What enforces the quality of an import is the `Website` workflow
(`.github/workflows/website.yml`): on every push or pull request touching
`website/**` it runs `npm ci`, the vitest unit suite, and a production build
(with `PRICING_ALLOW_STALE=1` — a hosted build holds no pricing API
credentials, so it builds on the committed pricing file). A broken import
cannot merge.

## Vulnerability Scanning

```bash
# Scan source dependencies (VEX suppressions applied)
grype sbom:sbom/mod_pagespeed-2.1.spdx.json --vex sbom/mod_pagespeed-2.1.vex.json
```

### Vulnerability SLA

| Severity | Timeline |
|----------|----------|
| Critical/High | Patch or mitigate within 30 days |
| Medium | Within 90 days |
| Low | Next scheduled release |

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
