#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for tools/ci/release_checksums_sig.sh. Generates two throwaway
# keys; never touches the real release key or the caller's keyring.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SIG="$HERE/release_checksums_sig.sh"
REAL_FPR=411DEFDB571351D2B25AEFCC9C02B5BDCCE75313

# A short base directory: gpg-agent socket paths have a length limit.
W="$(mktemp -d /tmp/rcs.XXXXXX)"
trap 'for h in "$W"/k1 "$W"/k2; do GNUPGHOME="$h" gpgconf --kill gpg-agent >/dev/null 2>&1; done; rm -rf "$W"' EXIT
export RUNNER_TEMP="$W"

fail=0
expect() { # expect ok|no <desc> <cmd...>
  local want="$1" desc="$2"; shift 2
  if "$@" >"$W/out" 2>&1; then got=ok; else got=no; fi
  if [ "$got" = "$want" ]; then echo "ok   $desc"; else echo "FAIL $desc (wanted $want, got $got)"; sed 's/^/     /' "$W/out"; fail=1; fi
}

mkkey() { # mkkey <dir> -> prints fingerprint; writes <dir>.pub, <dir>.b64
  local h="$1"
  mkdir -m 700 "$h"
  GNUPGHOME="$h" gpg --batch --quiet --pinentry-mode loopback --passphrase pw \
    --quick-gen-key "Self-test <self-test@example.invalid>" ed25519 sign 1d 2>/dev/null
  local f
  f="$(GNUPGHOME="$h" gpg --batch --with-colons --list-secret-keys 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
  GNUPGHOME="$h" gpg --batch --armor --export "$f" 2>/dev/null > "$h.pub"
  GNUPGHOME="$h" gpg --batch --quiet --pinentry-mode loopback --passphrase pw --armor --export-secret-keys "$f" | base64 | tr -d '\n' > "$h.b64"
  echo "$f"
}

command -v gpg >/dev/null || { echo "gpg not found"; exit 1; }
F1="$(mkkey "$W/k1")"
F2="$(mkkey "$W/k2")"
printf 'abc  file\n' > "$W/SHA256SUMS"

# shellcheck disable=SC2329 # invoked through expect
sign() { RELEASE_GPG_KEY="$1" RELEASE_GPG_PASSPHRASE="$2" RELEASE_KEY_FPR="$3" bash "$SIG" sign "$W/SHA256SUMS" "$4"; }
# shellcheck disable=SC2329 # invoked through expect
verify() { RELEASE_KEY_FPR="$1" bash "$SIG" verify "$2" "$3" "$4"; }

expect no "sign: no secret set"             sign "" "" "$F1" "$W/none.asc"
expect no "sign: passphrase missing"        sign "$(cat "$W/k1.b64")" "" "$F1" "$W/none.asc"
expect no "sign: key is not the pinned one" sign "$(cat "$W/k2.b64")" pw "$F1" "$W/wrong.asc"
if [ ! -e "$W/wrong.asc" ]; then echo "ok   sign: refused key wrote no signature"; else echo "FAIL sign: refused key wrote a signature"; fail=1; fi
expect no "sign: real pin, test key"        sign "$(cat "$W/k1.b64")" pw "$REAL_FPR" "$W/none.asc"
expect no "sign: two keys in the secret"    sign "$( { printf '%s' "$(cat "$W/k1.b64")" | base64 -d; printf '%s' "$(cat "$W/k2.b64")" | base64 -d; } | base64 | tr -d '\n')" pw "$F1" "$W/none.asc"
expect no "sign: wrong passphrase"          sign "$(cat "$W/k1.b64")" nope "$F1" "$W/none.asc"
expect ok "sign: pinned key"                sign "$(cat "$W/k1.b64")" pw "$F1" "$W/ok.asc"
sign "$(cat "$W/k2.b64")" pw "$F2" "$W/other.asc" >/dev/null 2>&1

cat "$W/k1.pub" "$W/k2.pub" > "$W/both.pub"
expect ok "verify: pinned key and its signature"  verify "$F1" "$W/SHA256SUMS" "$W/ok.asc" "$W/k1.pub"
expect no "verify: signature by another key"      verify "$F1" "$W/SHA256SUMS" "$W/other.asc" "$W/k1.pub"
expect no "verify: key file is another key"       verify "$F1" "$W/SHA256SUMS" "$W/other.asc" "$W/k2.pub"
expect no "verify: key file holds two keys"       verify "$F1" "$W/SHA256SUMS" "$W/ok.asc" "$W/both.pub"
expect no "verify: key file missing"              verify "$F1" "$W/SHA256SUMS" "$W/ok.asc" "$W/missing.pub"
expect no "verify: signature missing"             verify "$F1" "$W/SHA256SUMS" "$W/missing.asc" "$W/k1.pub"
cp "$W/SHA256SUMS" "$W/tampered"; echo x >> "$W/tampered"
expect no "verify: tampered SHA256SUMS"           verify "$F1" "$W/tampered" "$W/ok.asc" "$W/k1.pub"
expect no "verify: default pin, test key"         env -u RELEASE_KEY_FPR bash "$SIG" verify "$W/SHA256SUMS" "$W/ok.asc" "$W/k1.pub"

# The committed public key is the pinned release key.
if [ -f "$ROOT/RELEASE-SIGNING-KEY.asc" ]; then
  got="$(GNUPGHOME="$W/k1" gpg --batch --with-colons --show-keys "$ROOT/RELEASE-SIGNING-KEY.asc" 2>/dev/null | awk -F: '$1=="pub"{p=1} $1=="sub"{p=0} $1=="fpr"&&p{print $10; p=0}')"
  if [ "$got" = "$REAL_FPR" ]; then echo "ok   RELEASE-SIGNING-KEY.asc is $REAL_FPR"; else echo "FAIL RELEASE-SIGNING-KEY.asc holds [$got], expected $REAL_FPR"; fail=1; fi
  if grep -q "^PINNED_FPR=$REAL_FPR\$" "$SIG"; then echo "ok   script pins $REAL_FPR"; else echo "FAIL script does not pin $REAL_FPR"; fail=1; fi
else
  echo "FAIL RELEASE-SIGNING-KEY.asc missing at the repository root"; fail=1
fi

[ "$fail" = 0 ] && echo "All release checksum signature cases passed" || echo "release checksum signature self-test FAILED"
exit "$fail"
