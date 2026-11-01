#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Asserts the NuGet publish rehearsal is structurally without a NuGet key:
# its two workflow files name no environment, read no secret, ask for no
# OIDC token and no write permission, never log in to nuget.org and never
# push anywhere but a local folder feed. Then proves the assertions bite by
# running them against deliberately broken copies.
#
# Run: tools/ci/test_nuget_rehearsal_keyless.sh   (no network, no dotnet)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W="$(mktemp -d "${TMPDIR:-/tmp}/keyless.XXXXXX")"
trap 'rm -rf "$W"' EXIT

FILES=".github/workflows/nuget-rehearsal.yml .github/workflows/pipeline-nuget-rehearsal.yml"
SCRIPT="tools/ci/nupkg_checks.sh"
MOCK="tools/ci/nuget_mock_feed.py"

# check <root>: prints one line per violation, returns non-zero if any.
check() {
  local root="$1" f n=0 body
  for f in $FILES $SCRIPT $MOCK; do
    [ -s "$root/$f" ] || { echo "missing $f"; n=$((n+1)); }
  done
  [ "$n" = 0 ] || return 1
  for f in $FILES; do
    body="$(sed 's/^[[:space:]]*#.*$//; s/[[:space:]]#[[:space:]].*$//' "$root/$f")"  # drop comments
    bad() { printf '%s\n' "$body" | grep -nE "$1" >/dev/null && { echo "$f: $2"; n=$((n+1)); }; return 0; }
    bad '^[[:space:]]*environment[[:space:]]*:'                 'names an environment'
    bad 'secrets[.[]'                                           'reads a secret'
    bad '^[[:space:]]*secrets[[:space:]]*:'                     'passes secrets'
    bad 'inherit'                                               'inherits secrets'
    bad 'id-token'                                              'asks for an OIDC token'
    bad ':[[:space:]]*write'                                    'asks for write permission'
    bad 'write-all'                                             'asks for write-all'
    bad 'NuGet/login'                                           'logs in to nuget.org'
    bad 'NUGET_API_KEY|--api-key|NUGET_[A-Z_]*KEY'              'handles a NuGet key'
    bad 'nuget[[:space:]]+push|nuget[[:space:]]+delete'         'pushes or deletes from the workflow'
    bad 'AZURE_|RELEASE_GPG|CORP_DISPATCH_TOKEN'                'names a release secret'
    bad 'pipeline-release-nuget'                                'calls the real release lane'
  done
  # The only push in the script goes to the throwaway folder feed.
  if grep -nE 'nuget (push|delete)' "$root/$SCRIPT" | grep -vE -- '--source "\$feed_url"' >/dev/null; then
    echo "$SCRIPT: a nuget push/delete that does not target the loopback feed"; n=$((n+1))
  fi
  grep -qE '^[[:space:]]*feed_url="http://127\.0\.0\.1:' "$root/$SCRIPT" \
    || { echo "$SCRIPT: feed_url is not a loopback address"; n=$((n+1)); }
  grep -qE '(bind|ThreadingHTTPServer)\(?\("127\.0\.0\.1"' "$root/tools/ci/nuget_mock_feed.py" 2>/dev/null \
    || { echo "nuget_mock_feed.py does not bind loopback only"; n=$((n+1)); }
  if grep -nE -- '--api-key|NUGET_API_KEY|-ApiKey' "$root/$SCRIPT" >/dev/null; then
    echo "$SCRIPT: handles a NuGet key"; n=$((n+1))
  fi
  # The caller passes nothing but inputs, and calls only the rehearsal.
  grep -q 'pipeline-nuget-rehearsal.yml@main' "$root/.github/workflows/nuget-rehearsal.yml" \
    || { echo "caller does not call pipeline-nuget-rehearsal.yml@main"; n=$((n+1)); }
  grep -qE '^[[:space:]]*(push|pull_request|schedule|workflow_run|release):' "$root/.github/workflows/nuget-rehearsal.yml" \
    && { echo "caller has a trigger other than workflow_dispatch"; n=$((n+1)); }
  [ "$n" = 0 ]
}

fail=0
if out="$(check "$ROOT")"; then echo "ok   the rehearsal is keyless (no environment, no secret, no OIDC, no push)"; else echo "FAIL the rehearsal is not keyless"; echo "$out" | sed 's/^/     /'; fail=1; fi

# Mutations: each must be caught.
mutate() { # mutate <desc> <file> <sed-expr>
  local d="$W/m"; rm -rf "$d"; mkdir -p "$d/.github/workflows" "$d/tools/ci"
  for f in $FILES $SCRIPT $MOCK; do cp "$ROOT/$f" "$d/$f"; done
  sed -i.bak "$3" "$d/$2" && rm -f "$d/$2.bak"
  if cmp -s "$ROOT/$2" "$d/$2"; then echo "FAIL mutation did not change anything: $1"; fail=1; return; fi
  if check "$d" >/dev/null; then echo "FAIL not caught: $1"; fail=1; else echo "ok   caught: $1"; fi
}
P=.github/workflows/pipeline-nuget-rehearsal.yml
C=.github/workflows/nuget-rehearsal.yml
mutate 'environment: release'          $P 's/^    runs-on: ubuntu-latest/    environment: release\n    runs-on: ubuntu-latest/'
mutate 'a secrets reference'           $P 's/GH_TOKEN: \${{ github.token }}/GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}/'
mutate 'secrets: inherit in the caller' $C 's/^    uses: /    secrets: inherit\n    uses: /'
mutate 'id-token: write'               $P 's/^  actions: read/  actions: read\n  id-token: write/'
mutate 'contents: write'               $P 's/^  contents: read/  contents: write/'
mutate 'NuGet/login step'              $P 's/- name: Ensure .NET 10 SDK/- uses: NuGet\/login@v1\n      - name: Ensure .NET 10 SDK/'
mutate 'a nuget push in the workflow'  $P 's/run: tools\/ci\/nupkg_checks.sh nuget-org nupkg/run: dotnet nuget push nupkg\/*.nupkg/'
mutate 'a push to nuget.org in the script' $SCRIPT 's/--source "\$feed_url" --configfile/--source https:\/\/api.nuget.org\/v3\/index.json --configfile/'
mutate 'a mock bound beyond loopback' $MOCK 's/("127.0.0.1", 0)/("0.0.0.0", 0)/'
mutate 'an api key in the script'      $SCRIPT 's/--skip-duplicate 2>&1/--skip-duplicate --api-key x 2>\&1/'
mutate 'a call to the real lane'       $C 's#pipeline-nuget-rehearsal.yml@main#pipeline-release-nuget.yml@main#'
mutate 'a tag-push trigger'            $C 's/^  workflow_dispatch:/  push:\n  workflow_dispatch:/'

[ "$fail" = 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
