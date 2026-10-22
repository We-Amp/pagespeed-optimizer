#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Fetches and verifies the sidecar files of a mod_pagespeed module release
# (the native pair of WeAmp.PageSpeed.Sidecar) under a committed pin (see
# docs/ci/release.md, "The sidecar pin"). Never "latest": the release is named
# by the pinned tag and its bytes by the pinned digests.
#
# Usage:
#   tools/ci/sidecar_fetch.sh fetch  <pin.json> <out-dir> <public key file>
#   tools/ci/sidecar_fetch.sh verify <pin.json> <dir>     <public key file>
#
# fetch:  downloads SHA256SUMS, SHA256SUMS.asc, sidecar-pair.json and the two
#         pair tarballs of the pinned tag from the public module release
#         (We-Amp/mod_pagespeed; SIDECAR_REPO overrides it), into an empty
#         <out-dir>, then runs verify. GH_READ_TOKEN, when set, is sent to the
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
#   - each tarball holds exactly its top directory and the files the manifest
#     names, regular files only, each with the manifest's sha256, and the two
#     executables are executable;
#   - <dir> holds exactly those five files.

set -euo pipefail

die() { echo "::error::$*" >&2; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIG="$HERE/release_checksums_sig.sh"
REPO="${SIDECAR_REPO:-We-Amp/mod_pagespeed}"

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
import hashlib, json, os, sys, tarfile

directory, tag, version, pin_pair = sys.argv[1:5]
RIDS = ("linux-x64", "linux-arm64")
EXECUTABLES = ("nginx", "pagespeed-nginx-launch")


def refuse(msg):
    print("::error::sidecar_fetch: %s" % msg, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


sums = {}
with open(os.path.join(directory, "SHA256SUMS")) as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        digest, _, name = line.partition("  ")
        name = name.lstrip("*")
        if len(digest) != 64 or not name or name in sums:
            refuse("SHA256SUMS has a malformed or repeated line: %r" % line)
        sums[name] = digest

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

expected = {"SHA256SUMS", "SHA256SUMS.asc", "sidecar-pair.json"}
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
  local pin="$1" out="$2" key="$3" api="$2/.release.json" auth=() name url
  read_pin "$pin"
  mkdir -p "$out"
  [ -z "$(ls -A "$out")" ] || die "$out is not empty"
  if [ -n "${GH_READ_TOKEN:-}" ]; then
    auth=(-H "Authorization: Bearer ${GH_READ_TOKEN}")
  fi
  curl -fsSL --retry 5 --retry-all-errors \
    -H "Accept: application/vnd.github+json" ${auth[@]+"${auth[@]}"} \
    "https://api.github.com/repos/${REPO}/releases/tags/${PIN_TAG}" -o "$api" \
    || die "no release $PIN_TAG in $REPO"
  for name in SHA256SUMS SHA256SUMS.asc sidecar-pair.json \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-x64.tar.gz" \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-arm64.tar.gz"; do
    url="$(python3 -I - "$api" "$name" <<'PY'
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
  rm -f "$api"
  cmd_verify "$pin" "$out" "$key"
}

case "${1:-}" in
  fetch) shift; cmd_fetch "$@" ;;
  verify) shift; cmd_verify "$@" ;;
  *) die "usage: $0 fetch|verify <pin.json> <dir> <public key file>" ;;
esac
