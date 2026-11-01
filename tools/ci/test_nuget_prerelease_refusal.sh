#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# A prerelease tag (v<X>-pre.<N>) may be dry-run but never released. This
# test asserts, on pipeline-release-nuget.yml and release.yml:
#   1. the ref-guard step that decides real or dry run refuses a prerelease
#      tag for a tag push and for a dispatch with dry_run false, and accepts a
#      dispatch dry run (the step's own script is extracted and executed);
#   2. every job but the first two depends, directly or through others, on
#      ref-guard, so a refusal there stops sign-windows, package,
#      sign-checksums, publish-nuget and promote-release, the jobs that run in
#      the `release` environment;
#   3. no workflow but release.yml calls the lane, and release.yml's v* push
#      trigger reaches it;
#   4. the VERSION.txt guard holds a prerelease dry run to its base version.
# Then it proves the assertions bite by running them on broken copies.
#
# Run: tools/ci/test_nuget_prerelease_refusal.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W="$(mktemp -d "${TMPDIR:-/tmp}/prerel.XXXXXX")"
trap 'rm -rf "$W"' EXIT
LANE=.github/workflows/pipeline-release-nuget.yml
CALLER=.github/workflows/release.yml

# step_script <file> <step name>: the run: block of a step, dedented.
step_script() {
  python3 -I - "$1" "$2" <<'PY'
import re, sys
lines = open(sys.argv[1]).read().split("\n")
name = sys.argv[2]
for i, l in enumerate(lines):
    if re.match(r"^\s+- (name|id): " + re.escape(name) + r"\s*$", l) or (l.strip() == "- name: " + name):
        break
else:
    sys.exit("step not found: " + name)
indent = len(lines[i]) - len(lines[i].lstrip())
j = i + 1
while j < len(lines) and not re.match(r"^\s*run: \|\s*$", lines[j]):
    if len(lines[j]) - len(lines[j].lstrip()) <= indent and lines[j].strip().startswith("- "): sys.exit("no run block")
    j += 1
body, k = [], j + 1
base = None
while k < len(lines):
    t = lines[k]
    if t.strip() == "":
        body.append(""); k += 1; continue
    ind = len(t) - len(t.lstrip())
    if base is None: base = ind
    if ind < base: break
    body.append(t[base:]); k += 1
print("\n".join(body))
PY
}

# structure <root>: prints violations of 2 and 3.
structure() {
  python3 -I - "$1/$LANE" "$1/$CALLER" "$1/.github/workflows" <<'PY'
import glob, os, re, sys
lane, caller, wfdir = sys.argv[1:4]
errs = []
text = open(lane).read().split("\n")
jobs, cur, injobs = {}, None, False
for l in text:
    if l == "jobs:": injobs = True; continue
    if not injobs: continue
    m = re.match(r"^  ([a-z0-9][a-z0-9-]*):\s*$", l)
    if m: cur = m.group(1); jobs[cur] = {"needs": [], "env": False}; continue
    if cur is None: continue
    m = re.match(r"^    needs:\s*(.*)$", l)
    if m:
        v = m.group(1).strip()
        if not v: errs.append("%s: block-form needs is not supported by this test" % cur)
        jobs[cur]["needs"] = [x.strip() for x in v.strip("[]").split(",") if x.strip()]
    if re.match(r"^    environment:", l): jobs[cur]["env"] = True
def closure(j, seen=None):
    seen = seen if seen is not None else set()
    for n in jobs.get(j, {"needs": []})["needs"]:
        if n not in seen: seen.add(n); closure(n, seen)
    return seen
for need in ("event-guard", "ref-guard", "sign-windows", "package", "sign-checksums", "publish-nuget", "promote-release"):
    if need not in jobs: errs.append("job %s not found" % need)
if "event-guard" not in jobs.get("ref-guard", {"needs": []})["needs"]: errs.append("ref-guard does not need event-guard")
envjobs = sorted(j for j in jobs if jobs[j]["env"])
for want in ("sign-windows", "package", "sign-checksums", "publish-nuget", "promote-release"):
    if want not in envjobs: errs.append("%s no longer names an environment (test needs updating)" % want)
for j in jobs:
    if j in ("event-guard", "ref-guard"): continue
    if "ref-guard" not in closure(j): errs.append("%s does not depend on ref-guard" % j)
c = open(caller).read()
if not re.search(r"tags:\s*\[[^\]]*'v\*'", c): errs.append("release.yml no longer triggers on v* tags (test needs updating)")
if "pipeline-release-nuget.yml@main" not in c: errs.append("release.yml does not call the lane")
if re.search(r"^\s*environment:", c, re.M): errs.append("release.yml names an environment itself")
for f in glob.glob(wfdir + "/*.yml"):
    if os.path.basename(f) in ("release.yml", "pipeline-release-nuget.yml"): continue
    if re.search(r"pipeline-release-nuget", open(f).read()): errs.append("%s calls the release lane" % os.path.basename(f))
print("\n".join(errs))
if not errs: print("jobs in the release environment: " + ", ".join(envjobs), file=sys.stderr)
sys.exit(1 if errs else 0)
PY
}

# modes <root>: step 1.
modes() {
  local root="$1" f="$W/mode.sh" bad=0
  step_script "$root/$LANE" "Real run or dry run" > "$f" 2>"$W/err" || { cat "$W/err"; return 1; }
  [ -s "$f" ] || { echo "mode step not found"; return 1; }
  try() { # try <want ok|no> <event> <dry_run input> <ref> <desc>
    local want="$1" ev="$2" dry="$3" ref="$4" desc="$5" got out
    : > "$W/go"
    if out="$(EVENT_NAME="$ev" DRY_RUN_INPUT="$dry" REF="$ref" SHA=0123456789abcdef0123456789abcdef01234567 GITHUB_OUTPUT="$W/go" bash "$f" 2>&1)"; then got=ok; else got=no; fi
    if [ "$got" != "$want" ]; then echo "$desc: wanted $want, got $got"; bad=1; return; fi
    [ "$want" = no ] || [ -z "${6:-}" ] || grep -qx "real=$6" "$W/go" || { echo "$desc: expected real=$6, got $(grep real= "$W/go")"; bad=1; }
  }
  try no push ''    refs/tags/v2.8.0-pre.1   'push of a prerelease tag' 
  try no push ''    refs/tags/v2.8.0-pre.1+b 'push of a prerelease tag with build metadata'
  try no workflow_dispatch false refs/tags/v2.8.0-pre.1 'dispatch dry_run false of a prerelease tag'
  try ok workflow_dispatch true  refs/tags/v2.8.0-pre.1 'dispatch dry run of a prerelease tag' false
  try ok push ''    refs/tags/v2.8.0         'push of a release tag' true
  try ok workflow_dispatch false refs/tags/v2.8.0 'dispatch dry_run false of a release tag' true
  try ok workflow_dispatch true  refs/tags/v2.8.0 'dispatch dry run of a release tag' false
  [ "$bad" = 0 ]
}

# versions <root>: step 4.
versions() {
  local root="$1" f="$W/ver.sh" bad=0
  step_script "$root/$LANE" "Verify VERSION.txt matches the release tag" > "$f" 2>/dev/null
  [ -s "$f" ] || { echo "version step not found"; return 1; }
  tryv() { # tryv <want> <tag> <real> <VERSION.txt> <desc>
    local want="$1" tag="$2" real="$3" ver="$4" desc="$5" got
    mkdir -p "$W/vt"; printf '%s\n' "$ver" > "$W/vt/VERSION.txt"
    if (cd "$W/vt" && RELEASE_TAG="$tag" REAL_RUN="$real" bash "$f" >/dev/null 2>&1); then got=ok; else got=no; fi
    [ "$got" = "$want" ] || { echo "$desc: wanted $want, got $got"; bad=1; }
  }
  tryv ok v2.8.0-pre.1 false 2.8.0 'dry run of -pre.1, VERSION.txt is the base'
  tryv no v2.8.0-pre.1 false 2.7.0 'dry run of -pre.1, VERSION.txt is older'
  tryv no v2.8.0-pre.1 true  2.8.0 'real run of -pre.1'
  tryv no v2.8.0-rc.1  false 2.8.0 'dry run of -rc.1'
  tryv ok v2.8.0       false 2.8.0 'dry run of a release tag'
  tryv no v2.8.0       true  2.7.0 'real run, VERSION.txt differs'
  [ "$bad" = 0 ]
}

all() { local root="$1" n=0 o
  o="$(modes "$root")" || { echo "$o"; n=1; }
  o="$(structure "$root" 2>/dev/null)" || { echo "$o"; n=1; }
  o="$(versions "$root")" || { echo "$o"; n=1; }
  return $n; }

fail=0
if out="$(all "$ROOT")"; then echo "ok   prerelease refusal, job dependencies and version guard hold"; structure "$ROOT" 2>&1 >/dev/null | sed 's/^/     /'; else echo "FAIL"; echo "$out" | sed 's/^/     /'; fail=1; fi

mutate() { # mutate <desc> <file> <python-expr transforming s>
  local d="$W/m"; rm -rf "$d"; mkdir -p "$d/.github/workflows"
  cp "$ROOT"/.github/workflows/*.yml "$d/.github/workflows/"
  python3 -I - "$d/$2" "$3" <<'PY'
import sys
p, expr = sys.argv[1], sys.argv[2]
s = open(p).read()
s2 = eval(expr, {"s": s})
open(p, "w").write(s2)
sys.exit(0 if s2 != s else 3)
PY
  [ $? = 0 ] || { echo "FAIL mutation did not change anything: $1"; fail=1; return; }
  if all "$d" >/dev/null 2>&1; then echo "FAIL not caught: $1"; fail=1; else echo "ok   caught: $1"; fi
}
mutate 'the refusal removed' $LANE "s.replace('*-*) if [ \"\$REAL\" = true ]; then echo', '*-*) if false; then echo')"
mutate 'the refusal limited to dispatch' $LANE "s.replace('*-*) if [ \"\$REAL\" = true ]; then echo', '*-*) if [ \"\$EVENT_NAME\" = workflow_dispatch ] \&\& [ \"\$REAL\" = true ]; then echo')"
mutate 'the refusal matches a narrower pattern' $LANE "s.replace('*-*) if [ \"\$REAL\"', '*-rc.*) if [ \"\$REAL\"')"
mutate 'security-gates no longer needs ref-guard' $LANE "s.replace('  security-gates:\n    name: Security Gates (SBOM + Grype + VEX)\n    needs: ref-guard', '  security-gates:\n    name: Security Gates (SBOM + Grype + VEX)\n    needs: event-guard')"
mutate 'publish-nuget needs nothing' $LANE "s.replace('    needs: [ref-guard, package, attach-signature]', '    needs: []')"
mutate 'an environment job without ref-guard' $LANE "s + '\n  extra:\n    runs-on: ubuntu-latest\n    environment: release\n    steps: []\n'"
mutate 'the dry-run base rule removed' $LANE "s.replace('[0-9]*.[0-9]*.[0-9]*-pre.[0-9]*) EXPECTED', 'NEVER) EXPECTED')"
mutate 'the base rule applied to real runs too' $LANE "s.replace('if [ \"\$REAL_RUN\" != true ]; then\n            case', 'if true; then\n            case')"
mutate 'another workflow calls the lane' .github/workflows/website.yml "s + '\n  x:\n    uses: We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-nuget.yml@main\n'"
mutate 'the v* trigger removed' $CALLER "s.replace(\"'v*'\", \"'w*'\")"
[ "$fail" = 0 ] && echo PASS || { echo FAILED; exit 1; }
