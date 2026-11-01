#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for tools/ci/sidecar_fetch.sh. Builds a throwaway signed module
# release (own key, own fingerprint through the signature script's self-test
# override) and checks that verify and stage accept it and refuse each way it
# can be wrong. Offline; never touches the real release key.
#
# SIDECAR_LIVE_TAG=module-vX.Y.Z additionally runs fetch (and with it verify,
# stage and the post-publication checks) against that published release with
# the repository's own release public key.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FETCH="$HERE/sidecar_fetch.sh"

command -v gpg >/dev/null || { echo "gpg not found"; exit 1; }
W="$(mktemp -d /tmp/scf.XXXXXX)"
trap 'GNUPGHOME="$W/k1" gpgconf --kill gpg-agent >/dev/null 2>&1; GNUPGHOME="$W/k2" gpgconf --kill gpg-agent >/dev/null 2>&1; rm -rf "$W"' EXIT
export RUNNER_TEMP="$W"

fail=0
expect() { # expect ok|no <desc> <cmd...>
  local want="$1" desc="$2"; shift 2
  if "$@" >"$W/out" 2>&1; then got=ok; else got=no; fi
  if [ "$got" = "$want" ]; then echo "ok   $desc :: $(tail -n 1 "$W/out" | cut -c1-110)"; else echo "FAIL $desc (wanted $want, got $got)"; sed 's/^/     /' "$W/out"; fail=1; fi
}

mkkey() { # mkkey <dir> -> prints the fingerprint; writes <dir>.pub
  mkdir -m 700 "$1"
  GNUPGHOME="$1" gpg --batch --quiet --pinentry-mode loopback --passphrase pw \
    --quick-gen-key "Self-test <self-test@example.invalid>" ed25519 sign 1d 2>/dev/null
  local f
  f="$(GNUPGHOME="$1" gpg --batch --with-colons --list-secret-keys 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
  GNUPGHOME="$1" gpg --batch --armor --export "$f" 2>/dev/null > "$1.pub"
  echo "$f"
}
F1="$(mkkey "$W/k1")"
mkkey "$W/k2" >/dev/null
export RELEASE_SIG_SELF_TEST=1 RELEASE_KEY_FPR="$F1"

sha() { python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
sign() { GNUPGHOME="$1" gpg --batch --quiet --pinentry-mode loopback --passphrase pw --armor --detach-sign --output "$3" "$2" 2>/dev/null; }

# build <dir> <version>: a well-formed release directory signed by k1.
build() {
  local d="$1" v="$2" tag="module-v$2"
  mkdir -p "$d" "$W/src/mod_pagespeed-$v/aspnetcore/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux" \
    "$W/src/mod_pagespeed-$v/aspnetcore/WeAmp.PageSpeed.AspNetCore" \
    "$W/src/mod_pagespeed-$v/aspnetcore/WeAmp.PageSpeed.AspNetCore.Tests"
  local a="$W/src/mod_pagespeed-$v/aspnetcore"
  echo '<Project/>' > "$a/Directory.Build.props"
  echo '<Project/>' > "$a/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux.csproj"
  echo '<Project/>' > "$a/WeAmp.PageSpeed.AspNetCore/WeAmp.PageSpeed.AspNetCore.csproj"
  echo '<Project/>' > "$a/WeAmp.PageSpeed.AspNetCore.Tests/WeAmp.PageSpeed.AspNetCore.Tests.csproj"
  tar -C "$W/src" -czf "$d/mod_pagespeed-$v.tar.gz" "mod_pagespeed-$v"
  python3 -I - "$d" "$v" "$tag" <<'PY'
import hashlib, json, os, sys, tarfile, io
d, v, tag = sys.argv[1:4]
rids = {}
for rid in ("linux-x64", "linux-arm64"):
    top = "pagespeed-sidecar-native-%s-%s" % (v, rid)
    files = {"nginx": b"#!nginx " + rid.encode(), "ngx_pagespeed_module.so": b"so " + rid.encode(),
             "pagespeed-nginx-launch": b"#!launch " + rid.encode(), "build-info.json": b"{}"}
    path = os.path.join(d, top + ".tar.gz")
    with tarfile.open(path, "w:gz") as t:
        di = tarfile.TarInfo(top); di.type = tarfile.DIRTYPE; di.mode = 0o755; t.addfile(di)
        for n, b in files.items():
            ti = tarfile.TarInfo(top + "/" + n); ti.size = len(b)
            ti.mode = 0o755 if n in ("nginx", "pagespeed-nginx-launch") else 0o644
            t.addfile(ti, io.BytesIO(b))
    raw = open(path, "rb").read()
    rids[rid] = {"asset": top + ".tar.gz", "sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw),
                 "files": {n: hashlib.sha256(b).hexdigest() for n, b in files.items()}}
json.dump({"schema": 1, "tag": tag, "version": v, "nginx_version": "0", "glibc_floor": "0",
           "source_sha": "0" * 40, "rids": rids}, open(os.path.join(d, "sidecar-pair.json"), "w"))
PY
  sums "$d"
}
# sums <dir> [signer]: (re)writes SHA256SUMS over every asset but SHA256SUMS(.asc) and signs it.
sums() {
  local d="$1" f
  rm -f "$d/SHA256SUMS" "$d/SHA256SUMS.asc"
  ( cd "$d" && for f in *; do printf '%s  %s\n' "$(sha "$f")" "$f"; done > "$W/sums.tmp" )
  mv "$W/sums.tmp" "$d/SHA256SUMS"
  sign "${2:-$W/k1}" "$d/SHA256SUMS" "$d/SHA256SUMS.asc"
}
copy() { rm -rf "$2"; cp -R "$1" "$2"; }

build "$W/good" 9.9.9
PUB="$W/k1.pub"
expect ok "verify: a well-formed signed release"           bash "$FETCH" verify module-v9.9.9 "$W/good" "$PUB"
expect ok "verify: prints the SHA256SUMS digest"           bash -c "bash '$FETCH' verify module-v9.9.9 '$W/good' '$PUB' | grep -q \"SHA256SUMS sha256 $(sha "$W/good/SHA256SUMS")\""
expect ok "stage: writes the tree and both pairs"          bash "$FETCH" stage module-v9.9.9 "$W/good" "$PUB" "$W/work"
for rid in linux-x64 linux-arm64; do
  if [ -x "$W/work/aspnetcore/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux/runtimes/$rid/native/nginx" ]; then
    echo "ok   stage: $rid nginx is executable"
  else
    echo "FAIL stage: $rid nginx missing or not executable"; fail=1
  fi
done
expect no "stage: refuses an existing work tree"           bash "$FETCH" stage module-v9.9.9 "$W/good" "$PUB" "$W/work"

expect no "verify: tag is not module-v<version>"           bash "$FETCH" verify v9.9.9 "$W/good" "$PUB"
expect no "verify: tag names another version"              bash "$FETCH" verify module-v9.9.8 "$W/good" "$PUB"
expect no "verify: signed by another key"                  bash -c "cd '$W' && cp -R good other && cd other && rm SHA256SUMS.asc && GNUPGHOME='$W/k2' gpg --batch --quiet --pinentry-mode loopback --passphrase pw --armor --detach-sign --output SHA256SUMS.asc SHA256SUMS && bash '$FETCH' verify module-v9.9.9 '$W/other' '$PUB'"

copy "$W/good" "$W/t1"; echo x >> "$W/t1/SHA256SUMS"
expect no "verify: SHA256SUMS changed after signing"       bash "$FETCH" verify module-v9.9.9 "$W/t1" "$PUB"
copy "$W/good" "$W/t2"; rm "$W/t2/SHA256SUMS.asc"
expect no "verify: signature missing"                      bash "$FETCH" verify module-v9.9.9 "$W/t2" "$PUB"
copy "$W/good" "$W/t3"; echo x >> "$W/t3/pagespeed-sidecar-native-9.9.9-linux-x64.tar.gz"
expect no "verify: pair tarball changed, still signed over the old bytes" bash "$FETCH" verify module-v9.9.9 "$W/t3" "$PUB"
copy "$W/good" "$W/t4"; echo x >> "$W/t4/pagespeed-sidecar-native-9.9.9-linux-x64.tar.gz"; sums "$W/t4"
expect no "verify: pair tarball changed and re-signed (manifest digest)" bash "$FETCH" verify module-v9.9.9 "$W/t4" "$PUB"
copy "$W/good" "$W/t5"; echo x >> "$W/t5/mod_pagespeed-9.9.9.tar.gz"
expect no "verify: source tarball changed after signing"   bash "$FETCH" verify module-v9.9.9 "$W/t5" "$PUB"
copy "$W/good" "$W/t6"; echo junk > "$W/t6/extra.txt"; sums "$W/t6"
expect no "verify: extra file in the directory"            bash "$FETCH" verify module-v9.9.9 "$W/t6" "$PUB"
copy "$W/good" "$W/t7"; python3 -I -c 'import json,sys; p=sys.argv[1]; m=json.load(open(p)); m["tag"]="module-v9.9.8"; json.dump(m,open(p,"w"))' "$W/t7/sidecar-pair.json"; sums "$W/t7"
expect no "verify: signed manifest names another tag"      bash "$FETCH" verify module-v9.9.9 "$W/t7" "$PUB"
copy "$W/good" "$W/t8"; grep -v ' sidecar-pair.json$' "$W/t8/SHA256SUMS" > "$W/t8.sums"; mv "$W/t8.sums" "$W/t8/SHA256SUMS"; sign "$W/k1" "$W/t8/SHA256SUMS" "$W/t8/SHA256SUMS.asc.new"; mv "$W/t8/SHA256SUMS.asc.new" "$W/t8/SHA256SUMS.asc"
expect no "verify: SHA256SUMS does not list the manifest"  bash "$FETCH" verify module-v9.9.9 "$W/t8" "$PUB"

# The tag is derived, never read from a file.
expect ok "resolve: real run, v<X> takes module-v<X>"      bash -c "[ \"\$(bash '$FETCH' resolve v2.8.0 real)\" = module-v2.8.0 ]"
expect no "resolve: refuses a pre-release tag"             bash "$FETCH" resolve v2.8.0-pre.1 real
expect no "resolve: refuses a module tag"                  bash "$FETCH" resolve module-v2.8.0 real
expect no "resolve: refuses an unknown mode"               bash "$FETCH" resolve v2.8.0 latest
if [ -e "$ROOT/tools/ci/sidecar-pin.json" ]; then echo "FAIL tools/ci/sidecar-pin.json still exists"; fail=1; else echo "ok   no pin file"; fi

if [ -n "${SIDECAR_LIVE_TAG:-}" ]; then
  # The repository's own release key and fingerprint, not the self-test key.
  expect ok "live: fetch, verify and post-publication checks of $SIDECAR_LIVE_TAG" \
    env -u RELEASE_SIG_SELF_TEST -u RELEASE_KEY_FPR bash "$FETCH" fetch "$SIDECAR_LIVE_TAG" "$W/live" "$ROOT/RELEASE-SIGNING-KEY.asc"
  expect ok "live: stage the fetched release" \
    env -u RELEASE_SIG_SELF_TEST -u RELEASE_KEY_FPR bash "$FETCH" stage "$SIDECAR_LIVE_TAG" "$W/live" "$ROOT/RELEASE-SIGNING-KEY.asc" "$W/live-work"
fi

[ "$fail" = 0 ] && echo "All sidecar fetch cases passed" || echo "sidecar fetch self-test FAILED"
exit "$fail"
