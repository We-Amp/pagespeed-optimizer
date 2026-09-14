#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Cut a ModPageSpeed 2.0 release: bump every version-carrying file in lockstep,
# regenerate the SBOM, commit, and tag. Run from anywhere inside the repo, on a
# clean tree (normally main, after the CHANGELOG "## Unreleased" prose is written).
#
# Usage:
#   tools/cut-release.sh X.Y.Z [--push] [--date YYYY-MM-DD]
#
# Why this exists: release.yml's first job hard-rejects a tag whose VERSION.txt
# does not match (and VersionInvariantTests + the SBOM --verify gate enforce that
# VERSION.txt, Directory.Build.props <Version>, Chart appVersion and the SBOM
# versionInfo all agree). Tagging origin/main directly therefore FAILS. This
# script does the whole bump deterministically so that can't happen.
#
# Bumps, in lockstep:
#   1. VERSION.txt                                  -> X.Y.Z
#   2. CHANGELOG.md                                 renames "## Unreleased" -> "## X.Y.Z — DATE"
#   3. deploy/helm/pagespeed/Chart.yaml             appVersion -> "X.Y.Z"
#   4. samples/aspnetcore/Directory.Build.props     <Version> -> X.Y.Z, prepends a
#                                                   PackageReleaseNotes line from the CHANGELOG
#   5. sbom/mod_pagespeed-2.1.spdx.json             regenerated via tools/generate-sbom.py
# then commits "release: bump to X.Y.Z" and annotated-tags vX.Y.Z.
#
# It stops after committing+tagging so you can review. `main` is protected by a
# `pull_request` ruleset, so the bump commit must land via a squash-merged PR
# (a direct `git push origin HEAD:main` — and therefore `--push` onto main — is
# REJECTED). Land it, then push the tag to trigger release.yml:
#   git push origin HEAD:release/vX.Y.Z
#   gh pr create --base main --head release/vX.Y.Z --title "release: bump to X.Y.Z" --fill
#   gh pr merge --squash --delete-branch
#   git push origin vX.Y.Z
# (release.yml + deploy-all.sh build from the TAG, so the off-main tag commit is
# only a cosmetic provenance wart.) See RELEASING.md "Cutting a release".
#
# NOTE: the Helm chart `version:` (the chart's own version, not appVersion) is
# left untouched — bump it by hand in Chart.yaml only if the chart itself changed.
set -euo pipefail

die() { echo "cut-release: error: $*" >&2; exit 1; }

VERSION=""; PUSH=false; DATE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --push)    PUSH=true; shift;;
    --date)    DATE="${2:-}"; [ -n "$DATE" ] || die "--date needs YYYY-MM-DD"; shift 2;;
    -h|--help) sed -n '2,34p' "$0"; exit 0;;
    -*)        die "unknown flag: $1";;
    *)         [ -z "$VERSION" ] || die "version already given ('$VERSION')"; VERSION="$1"; shift;;
  esac
done

[ -n "$VERSION" ] || die "usage: tools/cut-release.sh X.Y.Z [--push] [--date YYYY-MM-DD]"
VERSION="${VERSION#v}"                       # tolerate a leading v
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "version must be X.Y.Z (got '$VERSION')"
TAG="v$VERSION"
[ -n "$DATE" ] || DATE="$(date +%F)"

ROOT="$(git rev-parse --show-toplevel)" || die "not in a git repo"
cd "$ROOT"
[ -f VERSION.txt ] || die "no VERSION.txt at repo root — wrong repo?"
[ -z "$(git status --porcelain)" ] || die "working tree not clean — commit or stash first"
git rev-parse "$TAG" >/dev/null 2>&1 && die "tag $TAG already exists"
[ "$(grep -c '^## Unreleased$' CHANGELOG.md)" = "1" ] || die "CHANGELOG.md must have exactly one '## Unreleased' heading — write this release's notes there first"

# From here we mutate tracked files. On any STANDALONE command failure (e.g. a
# Python assertion or generate-sbom.py error), restore the clean tree the entry
# guard required so a partial bump never leaves the repo dirty. Disabled once the
# commit+tag land (a later push failure must not roll back the commit).
trap 'echo "cut-release: aborted — restoring working tree" >&2; git checkout -- . >/dev/null 2>&1 || true' ERR

echo "cut-release: bumping to $VERSION (date $DATE)"

# 1) VERSION.txt
printf '%s\n' "$VERSION" > VERSION.txt

# 2-5) text bumps (robust, assertion-guarded)
VERSION="$VERSION" DATE="$DATE" python3 - <<'PY'
import os, re, sys
V, D = os.environ["VERSION"], os.environ["DATE"]

# 2) CHANGELOG: rename the Unreleased heading (full-line anchored, so a heading
#    like "## Unreleased (beta)" is NOT a partial match)
p = "CHANGELOG.md"; s = open(p, encoding="utf-8").read()
assert len(re.findall(r"(?m)^## Unreleased$", s)) == 1, "expected exactly one '## Unreleased' line"
s = re.sub(r"(?m)^## Unreleased$", f"## {V} — {D}", s, count=1)
open(p, "w", encoding="utf-8").write(s)
m = re.search(rf"## {re.escape(V)} — {re.escape(D)}\n(.*?)(?:\n## |\Z)", s, re.S)
summary = " ".join(m.group(1).split()) if m and m.group(1).strip() else f"See CHANGELOG.md for {V}."

# 3) Chart.yaml appVersion
p = "deploy/helm/pagespeed/Chart.yaml"; s = open(p, encoding="utf-8").read()
s2 = re.sub(r'(?m)^appVersion:.*$', f'appVersion: "{V}"', s, count=1)   # tolerate any current quoting
assert s2 != s and f'appVersion: "{V}"' in s2, "Chart.yaml appVersion bump failed"
open(p, "w", encoding="utf-8").write(s2)

# 4) Directory.Build.props: <Version> + prepend a PackageReleaseNotes line
p = "samples/aspnetcore/Directory.Build.props"; s = open(p, encoding="utf-8").read()
s2 = re.sub(r"<Version>[0-9]+\.[0-9]+\.[0-9]+</Version>", f"<Version>{V}</Version>", s, count=1)
assert s2 != s, "Directory.Build.props <Version> bump failed"
assert s2.count("<PackageReleaseNotes>") == 1, "expected one <PackageReleaseNotes>"
xs = summary.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")   # summary is free-form CHANGELOG prose
# Replace, do not prepend: nuget.org rejects a ReleaseNotes property over
# 35000 chars, and the accumulated per-release history crossed that at 2.0.39.
# The CHANGELOG is the canonical history; the package carries only the current
# release's notes.
notes = f"{V}: {xs}\nSee CHANGELOG.md in the repository for earlier releases."
assert len(notes) <= 35000, f"PackageReleaseNotes exceeds nuget.org limit: {len(notes)} > 35000"
s2 = re.sub(r"<PackageReleaseNotes>.*?</PackageReleaseNotes>",
            lambda _m: f"<PackageReleaseNotes>{notes}</PackageReleaseNotes>",
            s2, count=1, flags=re.S)
open(p, "w", encoding="utf-8").write(s2)

print(f"  bumped VERSION.txt + CHANGELOG.md + Chart.yaml + Directory.Build.props")
PY

# 5) regenerate + verify the SBOM
python3 tools/generate-sbom.py >/dev/null
python3 tools/generate-sbom.py --verify >/dev/null || die "SBOM --verify failed after regen"
echo "  regenerated sbom/mod_pagespeed-2.1.spdx.json"

# guard: every version source must agree (mirrors the CI gates)
vt="$(tr -d '[:space:]' < VERSION.txt)"
db="$(grep -oE '<Version>[0-9]+\.[0-9]+\.[0-9]+</Version>' samples/aspnetcore/Directory.Build.props | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"
cv="$(grep -E '^appVersion:' deploy/helm/pagespeed/Chart.yaml | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"
sb="$(python3 -c 'import json; d=json.load(open("sbom/mod_pagespeed-2.1.spdx.json")); print(next(p["versionInfo"] for p in d["packages"] if p.get("name")=="mod_pagespeed-2.1"))')"
[ "$vt" = "$VERSION" ] && [ "$db" = "$VERSION" ] && [ "$cv" = "$VERSION" ] && [ "$sb" = "$VERSION" ] \
  || die "version disagreement: VERSION.txt=$vt props=$db chart=$cv sbom=$sb (want $VERSION)"
echo "  agree: VERSION.txt / Directory.Build.props / Chart appVersion / SBOM = $VERSION"

# commit + annotated tag
git add VERSION.txt CHANGELOG.md LICENSE deploy/helm/pagespeed/Chart.yaml \
        samples/aspnetcore/Directory.Build.props sbom/mod_pagespeed-2.1.spdx.json
git commit -q -m "release: bump to $VERSION"
git tag -a "$TAG" -m "ModPageSpeed $VERSION"
trap - ERR   # commit + tag have landed; a push failure below must not roll them back
echo "cut-release: committed + tagged $TAG (at $(git rev-parse --short HEAD))"
echo "NOTE: Chart 'version:' left unchanged — bump it by hand only if the chart changed."

if $PUSH; then
  git push origin HEAD:main
  git push origin "$TAG"
  echo "cut-release: pushed bump to main + $TAG → release.yml will run"
else
  echo "cut-release: review with 'git show', then:"
  echo "    git push origin HEAD:main && git push origin $TAG"
fi
