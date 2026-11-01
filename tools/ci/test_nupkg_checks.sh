#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for tools/ci/nupkg_checks.sh. Builds small synthetic packages (no
# network, no dotnet) and checks that the offline checks accept a good set
# and reject each defect they claim to catch.
#
# Run: tools/ci/test_nupkg_checks.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHK="$HERE/nupkg_checks.sh"
W="$(mktemp -d "${TMPDIR:-/tmp}/nupkgchk.XXXXXX")"
trap 'rm -rf "$W"' EXIT
V=9.9.9

# mkset <dir> [mutation]: seven synthetic packages.
mkset() {
  python3 -I - "$1" "${2:-}" "$V" <<'PY'
import os, struct, sys, zipfile
d, mut, v = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(d, exist_ok=True)
ids = ["WeAmp.PageSpeed", "WeAmp.PageSpeed.AspNetCore", "WeAmp.PageSpeed.NativeAssets.Linux",
       "WeAmp.PageSpeed.NativeAssets.Windows", "WeAmp.PageSpeed.NativeAssets.macOS",
       "WeAmp.PageSpeed.Sidecar", "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux"]
REPO = "https://github.com/We-Amp/pagespeed-optimizer"
def pe(signed):
    b = bytearray(512); b[0:2] = b"MZ"; struct.pack_into("<I", b, 0x3c, 0x80)
    b[0x80:0x84] = b"PE\0\0"; struct.pack_into("<H", b, 0x80 + 24, 0x20b)
    if signed:
        struct.pack_into("<II", b, 0x80 + 24 + 112 + 4 * 8, 400, 100)
    return bytes(b)
for i in ids:
    ver = v
    name = "%s.%s.nupkg" % (i, v)
    deps = ""
    if i.endswith("AspNetCore"):
        deps = '<dependencies><group targetFramework="net10.0"><dependency id="WeAmp.PageSpeed" version="%s" /></group></dependencies>' % (("1.0.0" if mut == "unpinned" else v))
    if i == "WeAmp.PageSpeed.Sidecar":
        deps = '<dependencies><group targetFramework="net10.0"><dependency id="WeAmp.PageSpeed.Sidecar.NativeAssets.Linux" version="%s" /></group></dependencies>' % v
    repo = "" if i.startswith("WeAmp.PageSpeed.Sidecar") else '<repository type="git" url="%s" commit="abc" />' % (REPO if not (mut == "badrepo" and i == "WeAmp.PageSpeed") else "https://example.invalid/x")
    if mut == "sourcelink" and i == "WeAmp.PageSpeed": repo += "<sourceLink />"
    if mut == "nuspecver" and i == "WeAmp.PageSpeed": ver = "1.2.3"
    if mut == "norepo" and i == "WeAmp.PageSpeed": repo = ""
    nuspec = ('<?xml version="1.0"?><package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd"><metadata>'
              '<id>%s</id><version>%s</version><authors>We-Amp</authors><description>d</description>'
              '<license type="expression">Apache-2.0</license><copyright>c</copyright><icon>icon.png</icon>%s%s</metadata></package>') % (i, ver, repo, deps)
    if mut == "nolicense" and i == "WeAmp.PageSpeed": nuspec = nuspec.replace('<license type="expression">Apache-2.0</license>', "")
    if mut == "noicon" and i == "WeAmp.PageSpeed": nuspec = nuspec  # icon declared, not shipped (below)
    if mut == "missing" and i == "WeAmp.PageSpeed.Sidecar": continue
    if mut == "missingnative" and i == "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux": continue
    z = zipfile.ZipFile(os.path.join(d, name), "w")
    z.writestr("[Content_Types].xml", "<Types/>"); z.writestr("_rels/.rels", "<Relationships/>")
    z.writestr(i + ".nuspec", nuspec)
    if not (mut == "noicon" and i == "WeAmp.PageSpeed"): z.writestr("icon.png", b"png")
    if i.endswith("AspNetCore"):
        z.writestr("lib/net10.0/WeAmp.PageSpeed.AspNetCore.dll", b"MZ\0\0" + (b"" if mut == "nospa" else b"\0WeAmp.PageSpeed.AspNetCore.Console/index.html\0 xx"))
    if i.endswith("NativeAssets.Windows"):
        z.writestr("runtimes/win-x64/native/pagespeed.dll", pe(mut != "unsigned-win"))
    z.close()
if mut == "extra": open(os.path.join(d, "WeAmp.Other.9.9.9.nupkg"), "wb").write(b"x")
if mut == "pinnote": open(os.path.join(d, "README-SIDECAR-PIN.txt"), "w").write("note")
if mut == "corrupt":
    p = os.path.join(d, "WeAmp.PageSpeed.%s.nupkg" % v); open(p, "wb").write(b"not a zip")
PY
}

fail=0
t() { # t ok|no <desc> <mutation> <check...>
  local want="$1" desc="$2" mut="$3"; shift 3
  rm -rf "$W/set"; mkset "$W/set" "$mut"
  if "$CHK" "$@" >"$W/out" 2>&1; then got=ok; else got=no; fi
  if [ "$got" = "$want" ]; then echo "ok   $desc"; else echo "FAIL $desc (wanted $want, got $got)"; sed 's/^/     /' "$W/out"; fail=1; fi
}
S="$W/set"
t ok 'a good set passes every offline check'           ''          offline "$S" $V signed
t ok 'a README-SIDECAR-PIN.txt note is tolerated'      pinnote     offline "$S" $V signed
t no 'six packages'                                    missing     expect-seven "$S" $V
t no 'sidecar native package missing'                  missingnative expect-seven "$S" $V
t no 'an eighth package'                               extra       expect-seven "$S" $V
t no 'an unexpected package id (identity)'             extra       identity "$S" $V
t no 'a wrong version on the command line'             ''          identity "$S" 9.9.8
t no 'a nuspec version that differs from the file'     nuspecver   identity "$S" $V
t no 'an internal dependency not pinned to the version' unpinned   identity "$S" $V
t no 'no license expression'                           nolicense   identity "$S" $V
t no 'a declared icon that is not in the package'      noicon      identity "$S" $V
t no 'a package that is not a zip'                     corrupt     identity "$S" $V
t no 'a SourceLink element'                            sourcelink  metadata "$S"
t no 'a repository url that is not the public repo'    badrepo     metadata "$S"
t no 'no repository url on a non-sidecar package'      norepo      metadata "$S"
t no 'the console SPA missing from the dll'            nospa       console-spa "$S"
t ok 'signed Windows binaries when signed is expected' ''          signing "$S" signed
t no 'unsigned Windows binaries when signed is expected' unsigned-win signing "$S" signed
t ok 'unsigned Windows binaries when unsigned is expected' unsigned-win signing "$S" unsigned
t no 'signed Windows binaries when unsigned is expected' ''        signing "$S" unsigned
t no 'a bad signing mode'                              ''          signing "$S" maybe
[ "$fail" = 0 ] && echo PASS || { echo FAILED; exit 1; }
