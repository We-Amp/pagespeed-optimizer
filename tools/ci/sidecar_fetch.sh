#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Fetches and verifies the sidecar inputs of a mod_pagespeed module release
# under a committed pin, stages them for packing WeAmp.PageSpeed.Sidecar and
# WeAmp.PageSpeed.Sidecar.NativeAssets.Linux, and checks the packed packages
# (see docs/ci/release.md, "The sidecar pin"). Never "latest": the release is
# named by the pinned tag and its bytes by the pinned digests, and every
# managed and source input comes from that release's signed source tarball,
# never from a branch checkout.
#
# Usage:
#   tools/ci/sidecar_fetch.sh fetch  <pin.json> <out-dir> <public key file>
#   tools/ci/sidecar_fetch.sh verify <pin.json> <dir>     <public key file>
#   tools/ci/sidecar_fetch.sh stage  <pin.json> <dir>     <public key file> <work-dir>
#   tools/ci/sidecar_fetch.sh check-packages <dir> <nupkg-dir> <version>
#
# fetch:  downloads SHA256SUMS, SHA256SUMS.asc, sidecar-pair.json, the two
#         pair tarballs and the source tarball mod_pagespeed-<version>.tar.gz
#         of the pinned tag from the public module release
#         (We-Amp/mod_pagespeed), into an empty <out-dir>, then runs verify. GH_READ_TOKEN, when set, is sent to the
#         API endpoint only, against the anonymous rate limit.
# verify: refuses unless all of the following hold:
#   - the pin names a module-v<version> tag, two sha256 digests and the
#     release key fingerprint, which must equal the fingerprint pinned in
#     tools/ci/release_checksums_sig.sh;
#   - sha256(SHA256SUMS) is the pinned digest;
#   - SHA256SUMS.asc is one good signature over SHA256SUMS by the release key,
#     checked with release_checksums_sig.sh verify against <public key file>;
#   - sha256(sidecar-pair.json) is the pinned digest, and SHA256SUMS lists it
#     with that digest;
#   - sidecar-pair.json names the pinned tag and version and exactly the RIDs
#     linux-x64 and linux-arm64, and each tarball has the name, size and
#     sha256 it gives, which SHA256SUMS lists too;
#   - each pair tarball holds exactly its top directory and the files the
#     manifest names, regular files only, each with the manifest's sha256, and
#     the two executables are executable;
#   - the source tarball has the sha256 SHA256SUMS lists for it;
#   - <dir> holds exactly those six files.
# stage:  runs verify, then writes <work-dir>/aspnetcore: the aspnetcore/ tree
#         of the source tarball (regular files and directories only), with the
#         pair of each RID (nginx, ngx_pagespeed_module.so,
#         pagespeed-nginx-launch, build-info.json) placed in
#         WeAmp.PageSpeed.Sidecar.NativeAssets.Linux/runtimes/<rid>/native.
# check-packages: refuses unless <nupkg-dir> holds exactly the two sidecar
#         packages of <version>: the native package with exactly the four files
#         per RID at the digests of sidecar-pair.json in <dir>, its
#         buildTransitive targets and the license files; the managed package
#         with lib/net8.0 and lib/net10.0, a dependency on the native package
#         at the same version, and the readme and license files.

set -euo pipefail

die() { echo "::error::$*" >&2; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIG="$HERE/release_checksums_sig.sh"
REPO=We-Amp/mod_pagespeed
FETCH_API=""

# pin_field <pin> <key>
pin_field() {
  python3 -I -c 'import json,sys; v=json.load(open(sys.argv[1])).get(sys.argv[2], ""); print(v if isinstance(v, str) else "")' "$1" "$2"
}

# Reads and checks the pin; sets PIN_TAG, PIN_VERSION, PIN_SUMS, PIN_PAIR, PIN_FPR.
read_pin() {
  local pin="$1" want_fpr
  [ -s "$pin" ] || die "the sidecar pin $pin is missing or empty"
  PIN_TAG="$(pin_field "$pin" tag)"
  PIN_SUMS="$(pin_field "$pin" sha256sums_sha256)"
  PIN_PAIR="$(pin_field "$pin" sidecar_pair_sha256)"
  PIN_FPR="$(pin_field "$pin" release_key_fingerprint)"
  case "$PIN_TAG" in
    PLACEHOLDER*|"") die "$pin has no module release tag: publish the module release first and record the pin" ;;
  esac
  [[ "$PIN_TAG" =~ ^module-v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] \
    || die "$pin names tag '$PIN_TAG', not module-v<version>"
  PIN_VERSION="${PIN_TAG#module-v}"
  [[ "$PIN_SUMS" =~ ^[0-9a-f]{64}$ ]] || die "$pin: sha256sums_sha256 is not a sha256 digest"
  [[ "$PIN_PAIR" =~ ^[0-9a-f]{64}$ ]] || die "$pin: sidecar_pair_sha256 is not a sha256 digest"
  want_fpr="$(sed -n 's/^PINNED_FPR=\([0-9A-F]\{40\}\)$/\1/p' "$SIG")"
  [ -n "$want_fpr" ] || die "no PINNED_FPR in $SIG"
  [ "$PIN_FPR" = "$want_fpr" ] \
    || die "$pin pins release key '$PIN_FPR', but $SIG pins $want_fpr"
}

cmd_verify() {
  [ $# -eq 3 ] || die "usage: $0 verify <pin.json> <dir> <public key file>"
  local pin="$1" dir="$2" key="$3" got
  read_pin "$pin"
  [ -d "$dir" ] || die "$dir is not a directory"
  for f in SHA256SUMS SHA256SUMS.asc sidecar-pair.json; do
    [ -s "$dir/$f" ] || die "$dir/$f is missing or empty"
  done
  got="$(python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$dir/SHA256SUMS")"
  [ "$got" = "$PIN_SUMS" ] \
    || die "SHA256SUMS of $PIN_TAG has sha256 $got, the pin says $PIN_SUMS"
  bash "$SIG" verify "$dir/SHA256SUMS" "$dir/SHA256SUMS.asc" "$key"
  python3 -I - "$dir" "$PIN_TAG" "$PIN_VERSION" "$PIN_PAIR" <<'PY'
import hashlib, json, os, re, sys, tarfile

directory, tag, version, pin_pair = sys.argv[1:5]
RIDS = ("linux-x64", "linux-arm64")
EXECUTABLES = ("nginx", "pagespeed-nginx-launch")
SOURCE = "mod_pagespeed-%s.tar.gz" % version


def refuse(msg):
    print("::error::sidecar_fetch: %s" % msg, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# sha256sum output: "<digest>  <name>" (text mode) or "<digest> *<name>"
# (binary mode); both are accepted.
SUMS_LINE = re.compile(r"^([0-9a-f]{64}) [ *](\S.*)$")
sums = {}
with open(os.path.join(directory, "SHA256SUMS")) as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        m = SUMS_LINE.match(line)
        if not m or m.group(2) in sums:
            refuse("SHA256SUMS has a malformed or repeated line: %r" % line)
        sums[m.group(2)] = m.group(1)

pair_path = os.path.join(directory, "sidecar-pair.json")
pair_sha = sha256_file(pair_path)
if pair_sha != pin_pair:
    refuse("sidecar-pair.json has sha256 %s, the pin says %s" % (pair_sha, pin_pair))
if sums.get("sidecar-pair.json") != pair_sha:
    refuse("SHA256SUMS does not list sidecar-pair.json with sha256 %s" % pair_sha)

with open(pair_path) as fh:
    manifest = json.load(fh)
if manifest.get("schema") != 1:
    refuse("sidecar-pair.json has schema %r, expected 1" % manifest.get("schema"))
if manifest.get("tag") != tag or manifest.get("version") != version:
    refuse("sidecar-pair.json names %r / %r, the pin %s / %s"
           % (manifest.get("tag"), manifest.get("version"), tag, version))
rids = manifest.get("rids") or {}
if set(rids) != set(RIDS):
    refuse("sidecar-pair.json names RIDs %s, expected exactly %s" % (sorted(rids), ", ".join(RIDS)))

expected = {"SHA256SUMS", "SHA256SUMS.asc", "sidecar-pair.json", SOURCE}
for rid in RIDS:
    entry = rids[rid]
    name = "pagespeed-sidecar-native-%s-%s.tar.gz" % (version, rid)
    if entry.get("asset") != name:
        refuse("%s: the manifest names asset %r, expected %s" % (rid, entry.get("asset"), name))
    path = os.path.join(directory, name)
    if not os.path.isfile(path):
        refuse("missing %s" % name)
    digest = sha256_file(path)
    if digest != entry.get("sha256") or sums.get(name) != digest:
        refuse("%s: sha256 %s, manifest %s, SHA256SUMS %s"
               % (name, digest, entry.get("sha256"), sums.get(name)))
    if os.path.getsize(path) != entry.get("size"):
        refuse("%s: size %d, the manifest says %r" % (name, os.path.getsize(path), entry.get("size")))
    want_files = entry.get("files") or {}
    if not want_files:
        refuse("%s: the manifest lists no files" % rid)
    top = name[: -len(".tar.gz")]
    seen = {}
    with tarfile.open(path, "r:gz") as tar:
        for m in tar.getmembers():
            mname = m.name.rstrip("/")
            if mname == top and m.isdir():
                continue
            base = mname[len(top) + 1:] if mname.startswith(top + "/") else ""
            if not base or "/" in base or base not in want_files or base in seen:
                refuse("%s: unexpected member %s" % (name, m.name))
            if not m.isfile():
                refuse("%s: %s is not a regular file" % (name, base))
            if base in EXECUTABLES and not (m.mode & 0o111):
                refuse("%s: %s is not executable" % (name, base))
            seen[base] = hashlib.sha256(tar.extractfile(m).read()).hexdigest()
    if seen != want_files:
        bad = sorted(k for k in set(seen) | set(want_files) if seen.get(k) != want_files.get(k))
        refuse("%s: files differ from the manifest: %s" % (name, ", ".join(bad)))
    expected.add(name)
    print("ok  %s  %s (%d files)" % (digest, name, len(seen)))

source_path = os.path.join(directory, SOURCE)
if not os.path.isfile(source_path):
    refuse("missing %s" % SOURCE)
source_sha = sha256_file(source_path)
if sums.get(SOURCE) != source_sha:
    refuse("%s: sha256 %s, SHA256SUMS %s" % (SOURCE, source_sha, sums.get(SOURCE)))
print("ok  %s  %s" % (source_sha, SOURCE))

present = set(os.listdir(directory))
if present != expected:
    refuse("%s holds %s; expected exactly %s"
           % (directory, sorted(present), sorted(expected)))
print("sidecar_fetch: %s verified (nginx %s, glibc floor %s, source %s)"
      % (tag, manifest.get("nginx_version"), manifest.get("glibc_floor"), manifest.get("source_sha")))
PY
}

cmd_fetch() {
  [ $# -eq 3 ] || die "usage: $0 fetch <pin.json> <out-dir> <public key file>"
  local pin="$1" out="$2" key="$3" auth=() name url
  read_pin "$pin"
  mkdir -p "$out"
  [ -z "$(ls -A "$out")" ] || die "$out is not empty"
  FETCH_API="$(mktemp)"
  trap 'rm -f "$FETCH_API"' EXIT
  if [ -n "${GH_READ_TOKEN:-}" ]; then
    auth=(-H "Authorization: Bearer ${GH_READ_TOKEN}")
  fi
  curl -fsSL --retry 5 --retry-all-errors \
    -H "Accept: application/vnd.github+json" ${auth[@]+"${auth[@]}"} \
    "https://api.github.com/repos/${REPO}/releases/tags/${PIN_TAG}" -o "$FETCH_API" \
    || die "no release $PIN_TAG in $REPO"
  for name in SHA256SUMS SHA256SUMS.asc sidecar-pair.json \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-x64.tar.gz" \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-arm64.tar.gz" \
      "mod_pagespeed-${PIN_VERSION}.tar.gz"; do
    url="$(python3 -I - "$FETCH_API" "$name" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
if doc.get("draft") or doc.get("prerelease"):
    sys.exit("the pinned module release is a draft or a pre-release")
urls = [a["browser_download_url"] for a in doc.get("assets", []) if a.get("name") == sys.argv[2]]
if len(urls) != 1:
    sys.exit("the pinned module release has %d assets named %s" % (len(urls), sys.argv[2]))
print(urls[0])
PY
)" || die "cannot resolve $name in $REPO $PIN_TAG"
    case "$url" in
      "https://github.com/${REPO}/releases/download/${PIN_TAG}/"*) ;;
      *) die "unexpected download URL for $name: $url" ;;
    esac
    # Anonymous: the release is public, and the token goes to the API only.
    curl -fsSL --retry 5 --retry-all-errors -o "$out/$name" "$url"
  done
  rm -f "$FETCH_API"
  cmd_verify "$pin" "$out" "$key"
}

cmd_stage() {
  [ $# -eq 4 ] || die "usage: $0 stage <pin.json> <dir> <public key file> <work-dir>"
  local pin="$1" dir="$2" key="$3" work="$4"
  cmd_verify "$pin" "$dir" "$key"
  mkdir -p "$work"
  [ ! -e "$work/aspnetcore" ] || die "$work/aspnetcore already exists"
  python3 -I - "$dir" "$PIN_VERSION" "$work" <<'PY'
import os, shutil, sys, tarfile

directory, version, work = sys.argv[1:4]
RIDS = ("linux-x64", "linux-arm64")
NATIVE = {"nginx": 0o755, "ngx_pagespeed_module.so": 0o644,
          "pagespeed-nginx-launch": 0o755, "build-info.json": 0o644}
NATIVE_PROJECT = "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux"


def refuse(msg):
    print("::error::sidecar_fetch stage: %s" % msg, file=sys.stderr)
    sys.exit(1)


def copy_member(tar, m, dest, mode):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with tar.extractfile(m) as src, open(dest, "wb") as out:
        shutil.copyfileobj(src, out)
    os.chmod(dest, mode)


# The aspnetcore/ tree of the verified source tarball: regular files and
# directories only, no absolute path, no "..", nothing outside aspnetcore/.
prefix = "mod_pagespeed-%s/aspnetcore/" % version
root = os.path.join(work, "aspnetcore")
count = 0
with tarfile.open(os.path.join(directory, "mod_pagespeed-%s.tar.gz" % version), "r:gz") as tar:
    for m in tar:
        name = m.name
        if not name.startswith(prefix):
            continue
        rel = name[len(prefix):].rstrip("/")
        if not rel:
            continue
        parts = rel.split("/")
        if name.startswith("/") or any(p in ("", ".", "..") for p in parts):
            refuse("unsafe path %s" % name)
        dest = os.path.join(root, *parts)
        if m.isdir():
            os.makedirs(dest, exist_ok=True)
        elif m.isfile():
            copy_member(tar, m, dest, 0o755 if m.mode & 0o111 else 0o644)
            count += 1
        else:
            refuse("%s is not a regular file or directory" % name)
for p in ("Directory.Build.props", NATIVE_PROJECT + "/" + NATIVE_PROJECT + ".csproj",
          "WeAmp.PageSpeed.AspNetCore/WeAmp.PageSpeed.AspNetCore.csproj",
          "WeAmp.PageSpeed.AspNetCore.Tests/WeAmp.PageSpeed.AspNetCore.Tests.csproj"):
    if not os.path.isfile(os.path.join(root, p)):
        refuse("the source tarball has no aspnetcore/%s" % p)
print("staged %d files of aspnetcore/ from the source tarball" % count)

# The verified pair of each RID, where the native project packs it from.
for rid in RIDS:
    name = "pagespeed-sidecar-native-%s-%s" % (version, rid)
    native = os.path.join(root, NATIVE_PROJECT, "runtimes", rid, "native")
    os.makedirs(native, exist_ok=True)
    for f in os.listdir(native):
        if f != ".gitkeep":
            refuse("%s already holds %s" % (native, f))
    placed = set()
    with tarfile.open(os.path.join(directory, name + ".tar.gz"), "r:gz") as tar:
        for m in tar.getmembers():
            base = m.name[len(name) + 1:] if m.name.startswith(name + "/") else ""
            if base in NATIVE and m.isfile():
                copy_member(tar, m, os.path.join(native, base), NATIVE[base])
                placed.add(base)
    if placed != set(NATIVE):
        refuse("%s: missing %s" % (name, ", ".join(sorted(set(NATIVE) - placed))))
    print("placed %s in %s" % (", ".join(sorted(placed)), os.path.relpath(native, work)))
PY
}

cmd_check_packages() {
  [ $# -eq 3 ] || die "usage: $0 check-packages <dir> <nupkg-dir> <version>"
  python3 -I - "$@" <<'PY'
import hashlib, json, os, re, sys, zipfile
import xml.etree.ElementTree as ET

directory, nupkgs, version = sys.argv[1:4]
RIDS = ("linux-x64", "linux-arm64")
NATIVE_FILES = ("nginx", "ngx_pagespeed_module.so", "pagespeed-nginx-launch", "build-info.json")
MANAGED = "WeAmp.PageSpeed.Sidecar"
NATIVE = "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux"
LICENSES = ("LICENSE", "NOTICE", "THIRD-PARTY-NOTICES")


def refuse(msg):
    print("::error::sidecar_fetch check-packages: %s" % msg, file=sys.stderr)
    sys.exit(1)


def nuspec(z, pkg_id):
    names = [n for n in z.namelist() if n.endswith(".nuspec") and "/" not in n]
    if names != [pkg_id + ".nuspec"]:
        refuse("%s: nuspec entries %s" % (pkg_id, names))
    root = ET.fromstring(z.read(names[0]))
    ns = {"n": root.tag[1:].split("}")[0]} if root.tag.startswith("{") else {"n": ""}
    meta = root.find("n:metadata", ns)
    get = lambda k: (meta.findtext("n:" + k, default="", namespaces=ns) or "").strip()
    if get("id") != pkg_id or get("version") != version:
        refuse("%s: nuspec says %s %s, expected %s %s" % (pkg_id, get("id"), get("version"), pkg_id, version))
    repo = meta.find("n:repository", ns)
    if repo is not None and any(repo.get(a) for a in ("url", "commit", "branch")):
        refuse("%s: source-control metadata in the nuspec" % pkg_id)
    return meta, ns


with open(os.path.join(directory, "sidecar-pair.json")) as fh:
    manifest = json.load(fh)

want = {"%s.%s.nupkg" % (MANAGED, version), "%s.%s.nupkg" % (NATIVE, version)}
present = {f for f in os.listdir(nupkgs) if f.endswith(".nupkg")}
if present != want:
    refuse("%s holds %s, expected exactly %s" % (nupkgs, sorted(present), sorted(want)))

with zipfile.ZipFile(os.path.join(nupkgs, "%s.%s.nupkg" % (NATIVE, version))) as z:
    nuspec(z, NATIVE)
    names = set(z.namelist())
    for f in LICENSES + ("buildTransitive/%s.targets" % NATIVE,):
        if f not in names:
            refuse("%s: no %s" % (NATIVE, f))
    if any(n.startswith("lib/") for n in names):
        refuse("%s: carries lib/ (a content-only package)" % NATIVE)
    natives = sorted(n for n in names if n.startswith("runtimes/") and not n.endswith("/"))
    expect = sorted("runtimes/%s/native/%s" % (r, f) for r in RIDS for f in NATIVE_FILES)
    if natives != expect:
        refuse("%s: runtimes entries %s, expected %s" % (NATIVE, natives, expect))
    for rid in RIDS:
        files = manifest["rids"][rid]["files"]
        for f in NATIVE_FILES:
            got = hashlib.sha256(z.read("runtimes/%s/native/%s" % (rid, f))).hexdigest()
            if got != files[f]:
                refuse("%s: runtimes/%s/native/%s has sha256 %s, sidecar-pair.json %s"
                       % (NATIVE, rid, f, got, files[f]))
    print("ok  %s %s: %d native files at the digests of sidecar-pair.json" % (NATIVE, version, len(expect)))

with zipfile.ZipFile(os.path.join(nupkgs, "%s.%s.nupkg" % (MANAGED, version))) as z:
    meta, ns = nuspec(z, MANAGED)
    names = set(z.namelist())
    for f in LICENSES + ("README.md",) + tuple("lib/%s/%s.dll" % (t, MANAGED) for t in ("net8.0", "net10.0")):
        if f not in names:
            refuse("%s: no %s" % (MANAGED, f))
    if any(n.startswith("runtimes/") for n in names):
        refuse("%s: carries runtimes/ (the pair belongs in %s)" % (MANAGED, NATIVE))
    deps = meta.findall(".//n:dependency", ns)
    native_deps = [d for d in deps if d.get("id") == NATIVE]
    groups = meta.findall(".//n:dependencies/n:group", ns)
    if len(groups) < 2 or len(native_deps) != len(groups):
        refuse("%s: every framework group must depend on %s (groups %d, deps %d)"
               % (MANAGED, NATIVE, len(groups), len(native_deps)))
    for d in native_deps:
        if re.sub(r"^\[|[\],].*$", "", d.get("version", "")) != version:
            refuse("%s: depends on %s %s, expected %s" % (MANAGED, NATIVE, d.get("version"), version))
    print("ok  %s %s: net8.0 + net10.0, depends on %s %s" % (MANAGED, version, NATIVE, version))
PY
}

case "${1:-}" in
  fetch) shift; cmd_fetch "$@" ;;
  verify) shift; cmd_verify "$@" ;;
  stage) shift; cmd_stage "$@" ;;
  check-packages) shift; cmd_check_packages "$@" ;;
  *) die "usage: $0 fetch|verify|stage|check-packages ..." ;;
esac
