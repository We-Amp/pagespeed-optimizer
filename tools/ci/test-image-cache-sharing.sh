#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Image-level gate for the shared cache-volume contract (issues #1429 and
# #1456).
#
# The package world asserts this claim with the daemon-install rig's
# `peer-access` / `peer-cache-open` legs.  The container world had nothing: the
# `chmod 777 /data` + `chmod 666 cache.vol` sharing the entrypoints used to
# arrange was asserted nowhere, which is how the pair shipped a release whose
# nginx peer could not open the volume at all.  This is that missing leg.
#
# Usage:
#   tools/ci/test-image-cache-sharing.sh [WORKER_IMAGE] [NGINX_IMAGE] [COMBINED_IMAGE]
#
# Defaults to modpagespeed/worker:latest + modpagespeed/nginx:latest, i.e. what
# docker/build-release.sh just built.  Needs docker and about a minute.

set -uo pipefail

WORKER_IMAGE="${1:-modpagespeed/worker:latest}"
NGINX_IMAGE="${2:-modpagespeed/nginx:latest}"
# Optional third argument: the combined image, when one has been built.
COMBINED_IMAGE="${3:-}"

PS_UID=918
PS_GID=918
RUN_ID="pscs-$$-$RANDOM"
FAILURES=0
CHECKS=0

pass() { CHECKS=$((CHECKS + 1)); echo "  ok    $*"; }
fail() { CHECKS=$((CHECKS + 1)); FAILURES=$((FAILURES + 1)); echo "  FAIL  $*"; }
info() { echo "  ..    $*"; }
head_() { echo; echo "== $* =="; }

cleanup() {
  docker rm -f "${RUN_ID}-worker" "${RUN_ID}-nginx" "${RUN_ID}-racer" >/dev/null 2>&1 || true
  docker rmi -f "${RUN_ID}-harness" >/dev/null 2>&1 || true
  for v in fresh migrate sandbox chrome chromeneg hostile peervol peerjson peersib peergen emptygen genmode modes asuser harness comb; do
    docker volume rm "${RUN_ID}-${v}" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

# Wait until the worker has published its volume + socket on the volume, or the
# container dies.  Returns 1 on timeout/death so the caller can dump the log.
wait_for_worker() {
  local name="$1"
  for _ in $(seq 1 60); do
    if ! docker inspect -f '{{.State.Running}}' "$name" 2>/dev/null | grep -q true; then
      return 1
    fi
    if docker exec "$name" sh -c 'test -S /data/pagespeed.sock' 2>/dev/null; then
      return 0
    fi
    sleep 1
  done
  return 1
}

# Wait until `docker logs` shows a fixed-string line, or a bounded timeout.
# wait_for_worker's signal (the socket exists) and the entrypoint/daemon's log
# flush are NOT ordered (#1466: a release gate failed 48/50 on a single read,
# then passed 50/50 on an immediate rerun against the same image), so an
# assertion that reads the log ONCE can beat the flush and fail a healthy run.
# Poll instead: the line is produced on every healthy run, so waiting for it
# is strictly more patient, never weaker.  Returns 1 early if the container
# died (further polling cannot produce the line).
wait_for_log() {
  local name="$1" pattern="$2"
  for _ in $(seq 1 30); do
    if docker logs "$name" 2>&1 | grep -qF -- "$pattern"; then
      return 0
    fi
    docker inspect -f '{{.State.Running}}' "$name" 2>/dev/null | grep -q true || return 1
    sleep 1
  done
  return 1
}

echo "Cache-sharing image gate"
echo "  worker: $WORKER_IMAGE"
echo "  nginx:  $NGINX_IMAGE"

# --------------------------------------------------------------------------
head_ "1. Image identity contract (the fixed GID both images must agree on)"

w_id="$(docker run --rm --entrypoint sh "$WORKER_IMAGE" -c 'id -u pagespeed; id -g pagespeed' 2>/dev/null | tr '\n' ':')"
if [ "$w_id" = "${PS_UID}:${PS_GID}:" ]; then
  pass "worker image: pagespeed is ${PS_UID}:${PS_GID}"
else
  fail "worker image: pagespeed is '${w_id%:}', expected ${PS_UID}:${PS_GID}"
fi

n_gid="$(docker run --rm --entrypoint sh "$NGINX_IMAGE" -c 'getent group pagespeed | cut -d: -f3' 2>/dev/null)"
if [ "$n_gid" = "$PS_GID" ]; then
  pass "nginx image: group pagespeed is $PS_GID"
else
  fail "nginx image: group pagespeed is '$n_gid', expected $PS_GID"
fi

n_groups="$(docker run --rm --entrypoint sh "$NGINX_IMAGE" -c 'id -G nginx' 2>/dev/null)"
if echo " $n_groups " | grep -q " $PS_GID "; then
  pass "nginx image: user nginx is a member of group $PS_GID (groups: $n_groups)"
else
  fail "nginx image: user nginx is NOT in group $PS_GID (groups: $n_groups)"
fi

w_data="$(docker run --rm --entrypoint sh "$WORKER_IMAGE" -c 'stat -c "%a %u:%g" /data' 2>/dev/null)"
if [ "$w_data" = "2750 ${PS_UID}:${PS_GID}" ]; then
  pass "worker image: /data is 2750 ${PS_UID}:${PS_GID} (a fresh named volume inherits this)"
else
  fail "worker image: /data is '$w_data', expected 2750 ${PS_UID}:${PS_GID}"
fi

# --------------------------------------------------------------------------
head_ "2. Fresh volume: the daemon starts unprivileged and shares by group"

docker volume create "${RUN_ID}-fresh" >/dev/null
docker run -d --name "${RUN_ID}-worker" --shm-size=256m \
  -v "${RUN_ID}-fresh:/data" \
  -e CACHE_SIZE=67108864 \
  -e PAGESPEED_API_PORT=9880 \
  -e PAGESPEED_API_NO_AUTH=true \
  -e PAGESPEED_ENABLE_BROWSER_ANALYSIS=true \
  -e ACCEPT_EULA=Y \
  "$WORKER_IMAGE" >/dev/null

if wait_for_worker "${RUN_ID}-worker"; then
  pass "worker started on a fresh volume and published its socket"
else
  fail "worker did not come up on a fresh volume"
  docker logs "${RUN_ID}-worker" 2>&1 | tail -30
fi

euid="$(docker exec "${RUN_ID}-worker" sh -c 'ps -o user=,uid=,comm= -C factory_worker | head -1' 2>/dev/null | awk '{print $2}')"
if [ "$euid" = "$PS_UID" ]; then
  pass "factory_worker runs as uid $PS_UID (not root)"
else
  fail "factory_worker runs as uid '$euid', expected $PS_UID"
fi

# The drop must be one-way: an empty capability bounding set so nothing can be
# regained, and NoNewPrivs so the base image's setuid binaries stop being
# escalation targets from inside the daemon's process tree.
st="$(docker exec "${RUN_ID}-worker" sh -c \
  'p=$(pgrep -n factory_worker); grep -E "^(NoNewPrivs|CapBnd|CapEff):" /proc/$p/status' 2>/dev/null)"
info "daemon privilege state:"; echo "$st" | sed 's/^/          /'
if echo "$st" | grep -qE '^NoNewPrivs:[[:space:]]*1'; then
  pass "the daemon runs with NoNewPrivs=1 (the drop cannot be walked back)"
else
  fail "NoNewPrivs is not set on the daemon"
fi
if echo "$st" | grep -qE '^CapBnd:[[:space:]]*0+$'; then
  pass "the daemon's capability bounding set is empty"
else
  fail "the daemon's capability bounding set is not empty"
fi

ww="$(docker exec "${RUN_ID}-worker" sh -c 'find /data -perm -o+w -print' 2>/dev/null)"
if [ -z "$ww" ]; then
  pass "nothing under /data is world-writable"
else
  fail "world-writable entries under /data:"
  echo "$ww" | sed 's/^/          /'
fi

dmode="$(docker exec "${RUN_ID}-worker" sh -c 'stat -c "%a %u:%g" /data' 2>/dev/null)"
if [ "$dmode" = "2750 ${PS_UID}:${PS_GID}" ]; then
  pass "/data is 2750 ${PS_UID}:${PS_GID} — the peer can read it but not create in it"
else
  fail "/data is '$dmode', expected 2750 ${PS_UID}:${PS_GID}"
fi

info "/data after a fresh start:"
docker exec "${RUN_ID}-worker" sh -c 'ls -lan /data' 2>/dev/null | sed 's/^/          /'

vol_modes="$(docker exec "${RUN_ID}-worker" sh -c \
  'for f in /data/cache-*.vol /data/pagespeed.sock; do stat -c "%n %a %u:%g" "$f"; done' 2>/dev/null)"
bad="$(echo "$vol_modes" | awk -v u="$PS_UID" -v g="$PS_GID" '$2!="660" || $3!=u":"g')"
if [ -n "$vol_modes" ] && [ -z "$bad" ]; then
  pass "volume + notify socket are 0660 ${PS_UID}:${PS_GID}"
else
  fail "volume/socket modes wrong:"
  echo "$vol_modes" | sed 's/^/          /'
fi

# --------------------------------------------------------------------------
head_ "3. Peer access: a group member opens the volume, a non-member cannot"

# The real pair's nginx master runs as root and calls initgroups() when it drops
# its workers, so the supplementary group comes from /etc/group.  Reproduce both
# halves: the identity nginx actually runs its workers under (member) and an
# identity outside the group (non-member).
probe='set -e; echo "id: $(id)";
  for f in /data/cache-*.vol; do
    if [ -r "$f" ] && [ -w "$f" ]; then echo "VOLUME: RW-OK"; else echo "VOLUME: NO-ACCESS"; fi
  done
  if head -c 1 /data/pagespeed-shared.conf >/dev/null 2>&1; then echo "SHAREDCONF: R-OK"; else echo "SHAREDCONF: NO-ACCESS"; fi
  if [ -w /data/pagespeed.sock ]; then echo "SOCKET: W-OK"; else echo "SOCKET: NO-ACCESS"; fi'

member="$(docker run --rm --user nginx -v "${RUN_ID}-fresh:/data" --entrypoint sh "$NGINX_IMAGE" -c "$probe" 2>&1)"
info "member peer (user nginx):"
echo "$member" | sed 's/^/          /'
if echo "$member" | grep -q "VOLUME: RW-OK" \
   && echo "$member" | grep -q "SOCKET: W-OK" \
   && echo "$member" | grep -q "SHAREDCONF: R-OK"; then
  pass "group-member peer reaches the volume, the socket and the shared config"
else
  fail "group-member peer was denied something it must reach"
fi

nonmember="$(docker run --rm --user 65534:65534 -v "${RUN_ID}-fresh:/data" --entrypoint sh "$NGINX_IMAGE" -c "$probe" 2>&1)"
info "non-member peer (uid 65534):"
echo "$nonmember" | sed 's/^/          /'
if echo "$nonmember" | grep -q "VOLUME: RW-OK" \
   || echo "$nonmember" | grep -q "SOCKET: W-OK"; then
  fail "a non-member reached the volume or socket — the boundary is not a boundary"
else
  pass "non-member peer is denied the volume and the socket"
fi

# nginx's own workers must land in the group; that is the whole deployment.
docker run -d --name "${RUN_ID}-nginx" -v "${RUN_ID}-fresh:/data" \
  -e BACKEND_HOST=127.0.0.1 -e BACKEND_PORT=1 "$NGINX_IMAGE" >/dev/null 2>&1
sleep 4
ngroups="$(docker exec "${RUN_ID}-nginx" sh -c \
  'for p in /proc/[0-9]*; do
     grep -q "^Uid:.[^0]" "$p/status" 2>/dev/null && grep "^Groups:" "$p/status" && break
   done' 2>/dev/null)"
if echo "$ngroups" | grep -qw "$PS_GID"; then
  pass "nginx worker processes carry supplementary group $PS_GID ($ngroups)"
else
  fail "nginx worker processes do not carry group $PS_GID ($ngroups)"
  docker logs "${RUN_ID}-nginx" 2>&1 | tail -10
fi
docker rm -f "${RUN_ID}-nginx" >/dev/null 2>&1 || true

# --------------------------------------------------------------------------
head_ "4. Browser sandbox (H4): the root blocker is gone"

# Chrome declines to run its own sandbox as uid 0, so as long as the image ran
# the daemon as root `--browser-sandbox=require` could never be satisfied and
# the images had to ship PAGESPEED_BROWSER_SANDBOX=off.  Dropping to uid 918
# removes that blocker.  ONE blocker remains and it is not this issue's: the
# container runtime's DEFAULT seccomp profile refuses clone/unshare with
# CLONE_NEWUSER for an unprivileged process, so the sandbox is still reported
# `unavailable` until the deployment supplies the Chrome-compatible profile
# the design calls for.  Assert both halves, so a regression in either
# is visible: the named cause under the default profile, and a working sandbox
# once the namespace syscalls are allowed.
health="$(docker exec "${RUN_ID}-worker" curl -sf http://127.0.0.1:9880/v1/health 2>/dev/null)"
sandbox="$(echo "$health" | tr ',' '\n' | grep browser_sandbox | head -1)"
if echo "$sandbox" | grep -qE '"(on|unavailable)"'; then
  pass "default seccomp profile: /v1/health reports $sandbox"
else
  fail "default seccomp profile: /v1/health reports $sandbox"
fi
if echo "$sandbox" | grep -q '"unavailable"'; then
  if wait_for_log "${RUN_ID}-worker" "CLONE_NEWUSER"; then
    pass "and the refusal names the cause (user namespaces denied), not the uid"
  else
    fail "sandbox unavailable but the log does not name user namespaces as the cause"
    docker logs "${RUN_ID}-worker" 2>&1 | grep -i sandbox | tail -3
  fi
fi

docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true

# The same image, with the namespace syscalls allowed: the sandbox must come up.
docker volume create "${RUN_ID}-sandbox" >/dev/null
docker run -d --name "${RUN_ID}-worker" --shm-size=256m \
  --security-opt seccomp=unconfined \
  -v "${RUN_ID}-sandbox:/data" \
  -e CACHE_SIZE=67108864 -e PAGESPEED_API_PORT=9880 -e PAGESPEED_API_NO_AUTH=true \
  -e PAGESPEED_ENABLE_BROWSER_ANALYSIS=true -e ACCEPT_EULA=Y \
  "$WORKER_IMAGE" >/dev/null
if wait_for_worker "${RUN_ID}-worker"; then
  health="$(docker exec "${RUN_ID}-worker" curl -sf http://127.0.0.1:9880/v1/health 2>/dev/null)"
  sandbox="$(echo "$health" | tr ',' '\n' | grep browser_sandbox | head -1)"
  if echo "$sandbox" | grep -q '"on"'; then
    pass "with user namespaces allowed: browser_sandbox is \"on\" as ${PS_UID} ($sandbox)"
  else
    fail "with user namespaces allowed: browser_sandbox is $sandbox (expected \"on\")"
    docker logs "${RUN_ID}-worker" 2>&1 | grep -i sandbox | tail -3
  fi
else
  fail "worker did not start for the sandbox leg"
fi

docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true
docker volume rm "${RUN_ID}-sandbox" >/dev/null 2>&1 || true

# --------------------------------------------------------------------------
head_ "4b. Headless Chrome actually starts under the unprivileged entrypoint (#1467)"

# Legs 2 and 4 prove the DAEMON comes up unprivileged; neither proves the
# browser it spawns does.  1.16.0-rc.9 shipped with every Chrome launch dying
# at once (setpriv kept the launcher's HOME=/root, which uid 918 cannot write,
# and Chrome aborts when it cannot create its crash-report directory there) and
# the daemon respawning it every few seconds -- invisible to every gate here.
# So: run with browser analysis on and the sandbox off (the default seccomp
# profile denies user namespaces, and `require` would then not spawn Chrome at
# all), give Chrome time to start, and assert it is running and has never
# exited.  Then the negative control: the same image with a Chrome wrapper
# that re-poisons HOME the way rc.9's entrypoint did must trip the assertion,
# so this leg is known to see the failure it guards against.
chrome_leg() {   # $1 = volume suffix, $2.. = extra docker run args
  local vol="$1"; shift
  docker volume create "${RUN_ID}-${vol}" >/dev/null
  docker run -d --name "${RUN_ID}-worker" --shm-size=512m \
    -v "${RUN_ID}-${vol}:/data" \
    -e CACHE_SIZE=67108864 -e PAGESPEED_API_PORT=9880 -e PAGESPEED_API_NO_AUTH=true \
    -e PAGESPEED_ENABLE_BROWSER_ANALYSIS=true -e PAGESPEED_BROWSER_SANDBOX=off \
    -e ACCEPT_EULA=Y "$@" \
    "$WORKER_IMAGE" >/dev/null
  if ! wait_for_worker "${RUN_ID}-worker"; then
    echo "worker-did-not-start"
    return
  fi
  sleep 20
  local exits health running
  exits="$(docker logs "${RUN_ID}-worker" 2>&1 | grep -c 'Chrome exited' || true)"
  health="$(docker exec "${RUN_ID}-worker" curl -sf http://127.0.0.1:9880/v1/health 2>/dev/null)"
  running="$(echo "$health" | tr ',' '\n' | grep '"chrome_running"' | head -1 | cut -d: -f2)"
  echo "exits=${exits} chrome_running=${running:-absent}"
}

res="$(chrome_leg chrome)"
info "sandbox off, default entrypoint: $res"
if [ "$res" = "exits=0 chrome_running=true" ]; then
  pass "headless Chrome is running under the unprivileged entrypoint and never exited"
else
  fail "headless Chrome did not stay up under the unprivileged entrypoint ($res)"
  docker logs "${RUN_ID}-worker" 2>&1 | grep -iE 'chrome|browser' | tail -8 | sed 's/^/          /'
fi
docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true

# Negative control.  The wrapper is bind-mounted over the browser launcher the
# daemon execs; it re-imposes an unwritable HOME so Chrome fails exactly as it
# did in rc.9 (crash handler cannot start, browser traps before the DevTools
# pipe opens).  The daemon's own HOME pinning is what the wrapper overrides,
# so this is the rc.9 failure mode on today's image, not a synthetic exit.
# Kernel-dependent: the trap only happens on kernels where crashpad's failed
# handshake escalates to SIGTRAP (native Linux, WSL2 6.x). On Docker Desktop's
# linuxkit VM Chrome logs the crashpad error and carries on, so on such a host
# this control reads "did not trip" on a healthy image. CI pins the job to
# trapping kernels via the `real-linux` runner label and asserts the Yama proxy
# up front; other callers (docker/build-release.sh on a Mac) hit it as a false
# red -- see the container-cache-sharing job in CI.
NEG_WRAPPER="$(mktemp)"
cat >"$NEG_WRAPPER" <<'EOS'
#!/bin/sh
export HOME=/nonexistent
unset XDG_CONFIG_HOME
exec /usr/lib/chromium/chromium "$@"
EOS
chmod 755 "$NEG_WRAPPER"
res="$(chrome_leg chromeneg -v "$NEG_WRAPPER:/usr/bin/chrome-headless-shell:ro")"
rm -f "$NEG_WRAPPER"
info "negative control (HOME re-poisoned in a Chrome wrapper): $res"
case "$res" in
  exits=0*|worker-did-not-start)
    fail "negative control did not trip: the leg would not have caught rc.9 ($res)" ;;
  *)
    pass "negative control trips the assertion (Chrome exits are counted, chrome_running is false)" ;;
esac
# And the daemon must have said why, once, instead of only respawning.
if wait_for_log "${RUN_ID}-worker" 'Browser analysis is UNAVAILABLE'; then
  pass "the daemon names the refusal once the launch keeps failing"
else
  fail "no single refusal line after repeated Chrome launch failures"
  docker logs "${RUN_ID}-worker" 2>&1 | grep -iE 'chrome' | tail -5 | sed 's/^/          /'
fi
docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true
docker volume rm "${RUN_ID}-chrome" "${RUN_ID}-chromeneg" >/dev/null 2>&1 || true

# --------------------------------------------------------------------------
head_ "5. Migration: the #1456 volume (peer-owned stem + root-owned volume)"

# Exactly the state reported in #1456: a 0-byte stem owned by the nginx peer's
# uid, a real cache volume owned by root at 0666, a peer-owned generation file,
# and /data left world-writable by the old entrypoint.
docker volume create "${RUN_ID}-migrate" >/dev/null
docker run --rm -v "${RUN_ID}-migrate:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data
  touch /data/cache.vol && chown 999:999 /data/cache.vol && chmod 666 /data/cache.vol
  dd if=/dev/zero of=/data/cache-6-deadbeefdeadbeef.vol bs=1M count=8 status=none
  chown 0:0 /data/cache-6-deadbeefdeadbeef.vol && chmod 666 /data/cache-6-deadbeefdeadbeef.vol
  printf 6 > /data/cache.vol.gen && chown 999:999 /data/cache.vol.gen
  for f in pagespeed-shared.conf pagespeed.json pagespeed-hosts.conf \
           .pagespeed-serve-stats; do
    printf x > "/data/$f"; chown 0:0 "/data/$f"; chmod 666 "/data/$f"
  done' >/dev/null 2>&1

docker run -d --name "${RUN_ID}-worker" --shm-size=256m \
  -v "${RUN_ID}-migrate:/data" -e CACHE_SIZE=67108864 -e ACCEPT_EULA=Y \
  "$WORKER_IMAGE" >/dev/null

if wait_for_worker "${RUN_ID}-worker"; then
  pass "worker starts on the pre-existing pre-2.1 volume (no refusal, no restart loop)"
else
  fail "worker refused the pre-existing volume — the #1456 outage is not fixed"
  docker logs "${RUN_ID}-worker" 2>&1 | tail -30
fi

if wait_for_log "${RUN_ID}-worker" "adopted 0-byte cache stem"; then
  pass "the peer-owned 0-byte stem was adopted, and said so in the log"
else
  fail "no operator-visible log line for the adopted stem"
fi
if wait_for_log "${RUN_ID}-worker" "adopted existing cache volume"; then
  pass "the pre-existing cache volume was adopted rather than abandoned"
else
  fail "no operator-visible log line for the adopted volume"
fi

info "/data after migration:"
docker exec "${RUN_ID}-worker" sh -c 'ls -lan /data' 2>/dev/null | sed 's/^/          /'

owner="$(docker exec "${RUN_ID}-worker" sh -c 'stat -c "%a %u:%g" /data/cache-6-deadbeefdeadbeef.vol' 2>/dev/null)"
if [ "$owner" = "660 ${PS_UID}:${PS_GID}" ]; then
  pass "the migrated volume is 0660 ${PS_UID}:${PS_GID} (group-shared, not abandoned)"
else
  fail "the migrated volume is '$owner', expected 660 ${PS_UID}:${PS_GID}"
fi

ww="$(docker exec "${RUN_ID}-worker" sh -c 'find /data -perm -o+w -print' 2>/dev/null)"
if [ -z "$ww" ]; then
  pass "the migrated volume has nothing world-writable left on it"
else
  fail "world-writable entries survived the migration:"
  echo "$ww" | sed 's/^/          /'
fi

member="$(docker run --rm --user nginx -v "${RUN_ID}-migrate:/data" --entrypoint sh "$NGINX_IMAGE" -c "$probe" 2>&1)"
if echo "$member" | grep -q "VOLUME: RW-OK" && echo "$member" | grep -q "SOCKET: W-OK"; then
  pass "the nginx peer reaches the migrated volume and socket"
else
  fail "the nginx peer cannot reach the migrated volume:"
  echo "$member" | sed 's/^/          /'
fi

# Adoption must not leave a file more permissive than the daemon's own writer.
#
# Asserted on a prepared-but-not-started volume, because this is a claim about
# what ADOPTION does: once the daemon is running it rewrites its own sidecars at
# its own modes (pagespeed-hosts.conf goes back to 0644, which is the daemon's
# decision and not this entrypoint's). What the migration controls is the state
# the daemon inherits, and that is what is checked here.
docker volume create "${RUN_ID}-modes" >/dev/null
docker run --rm -v "${RUN_ID}-modes:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data
  for f in pagespeed-shared.conf pagespeed.json pagespeed-hosts.conf \
           .pagespeed-serve-stats \
           pagespeed-webbotauth-keys.conf pagespeed-rslcap-keys.conf; do
    printf x > "/data/$f"; chown 0:0 "/data/$f"; chmod 666 "/data/$f"
  done
  printf 6 > /data/cache.vol.gen; chown 0:0 /data/cache.vol.gen; chmod 666 /data/cache.vol.gen' >/dev/null 2>&1
docker run --rm -v "${RUN_ID}-modes:/data" -e ACCEPT_EULA=Y --entrypoint bash "$WORKER_IMAGE" \
  -c '. /docker/lib-cache-perms.sh; ps_prepare_data_dir /data /data/cache.vol' >/dev/null 2>&1
modes="$(docker run --rm -v "${RUN_ID}-modes:/data" --entrypoint sh "$WORKER_IMAGE" -c 'ls -lan /data' 2>/dev/null)"
info "modes the daemon would inherit from the migration:"
echo "$modes" | sed 's/^/          /'
bad=""
for spec in "pagespeed-shared.conf 640" "pagespeed.json 640" \
            "pagespeed-hosts.conf 640" "pagespeed-webbotauth-keys.conf 640" \
            "pagespeed-rslcap-keys.conf 640" ".pagespeed-serve-stats 660" \
            "cache.vol.gen 640"; do
  f="${spec% *}"; want="${spec#* }"
  got="$(docker run --rm -v "${RUN_ID}-modes:/data" --entrypoint sh "$WORKER_IMAGE" -c "stat -c %a /data/$f" 2>/dev/null)"
  [ "$got" = "$want" ] || bad="$bad $f(want $want got ${got:-missing})"
done
if [ -z "$bad" ]; then
  pass "every adopted sidecar carries its own mode, never a blanket 0660"
else
  fail "adopted sidecar modes are wrong:$bad"
fi

# The ones that matter as capabilities rather than as numbers: pagespeed.json is
# the daemon-written sidecar the peer must be able to read but never rewrite
# (a peer-writable copy would let it steer daemon startup — see the peer-owned
# pagespeed.json refusal below), and pagespeed-shared.conf is what the peer
# needs to read to find the worker. Each probe runs in its own subshell: a
# redirection failure is fatal to a non-interactive shell, which would otherwise
# abort the probe at the first denial and report nothing.
sec="$(docker run --rm --user nginx -v "${RUN_ID}-modes:/data" --entrypoint sh "$NGINX_IMAGE" -c '
  if (: > /data/pagespeed.json) >/dev/null 2>&1; then echo "JSON: WRITABLE"; else echo "JSON: denied"; fi
  if (head -c1 /data/pagespeed.json) >/dev/null 2>&1; then echo "JSON: readable"; else echo "JSON: NOT-READABLE"; fi
  if (head -c1 /data/pagespeed-shared.conf) >/dev/null 2>&1; then echo "SHAREDCONF: readable"; else echo "SHAREDCONF: NOT-READABLE"; fi' 2>&1)"
info "peer capability probe on the migrated sidecars:"; echo "$sec" | sed 's/^/          /'
if echo "$sec" | grep -q "JSON: denied" \
   && echo "$sec" | grep -q "JSON: readable" && echo "$sec" | grep -q "SHAREDCONF: readable"; then
  pass "the peer cannot rewrite pagespeed.json, but still reads what it needs"
else
  fail "the peer has the wrong capabilities on the migrated sidecars"
fi
docker volume rm "${RUN_ID}-modes" >/dev/null 2>&1 || true

docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true

# --------------------------------------------------------------------------
head_ "6. Fail-fast: adoption disabled refuses BEFORE the health dependency"

out="$(docker run --rm -v "${RUN_ID}-migrate:/data" \
  -e PAGESPEED_ADOPT_VOLUME=off -e ACCEPT_EULA=Y \
  --entrypoint sh "$WORKER_IMAGE" -c '
    chown 0:0 /data/cache-*.vol
    exec /entrypoint-worker.sh' 2>&1)"
rc=$?
if [ "$rc" = "78" ]; then
  pass "refusal exits 78 (EX_CONFIG), distinct from the daemon'\''s own exit 1"
else
  fail "refusal exited $rc, expected 78"
fi
if echo "$out" | grep -q "Remedy" && echo "$out" | grep -q "chown -R ${PS_UID}:${PS_GID}"; then
  pass "the refusal names the offending path and a one-line remedy"
else
  fail "the refusal is not actionable:"
  echo "$out" | tail -20 | sed 's/^/          /'
fi
if echo "$out" | grep -q "before the daemon starts"; then
  pass "the refusal happens before the daemon starts (no restart-loop-only symptom)"
else
  fail "the refusal did not report that it preceded daemon startup"
fi

# --------------------------------------------------------------------------
head_ "7. Hostile peer: a symlink swap during migration must not escape /data"

# The migration runs as root and decides per entry: check the entry, then chown
# it.  On a pre-2.1 volume /data is 0777, so the peer -- a uid that handles
# untrusted requests -- can rename an entry away and drop a symlink in its place
# between the check and the chown, and a plain chown FOLLOWS it.  Two things
# stop that: `chown -h` cannot dereference, and the directory is held at 0700
# for the whole scan so no other uid can stage anything at all.
#
# A NEGATIVE CONTROL runs first: the same race against a deliberately vulnerable
# shape (path chown, no directory lock), which must succeed in chowning the
# out-of-tree file.  Without it a green result could just mean the race never
# landed.
SECRET_DIR="$(mktemp -d)"
chmod 755 "$SECRET_DIR"

# The sentinel is recreated (not chowned back) between races: once a losing race
# has chowned it to uid 918, this unprivileged shell can no longer chown it back,
# and a stale owner would make the next race look like an instant loss. Deleting
# and recreating only needs write on the directory, which we own.
SENTINEL_UID=""
reset_sentinel() {
  rm -f "$SECRET_DIR/SENTINEL"
  : > "$SECRET_DIR/SENTINEL"
  chmod 600 "$SECRET_DIR/SENTINEL"
  SENTINEL_UID="$(stat -c '%u' "$SECRET_DIR/SENTINEL")"
}

seed_open_volume() {
  docker run --rm -v "${RUN_ID}-hostile:/data" --entrypoint sh "$WORKER_IMAGE" -c '
    rm -rf /data/..?* /data/.[!.]* /data/* 2>/dev/null || true
    chmod 777 /data
    dd if=/dev/zero of=/data/cache-6-deadbeefdeadbeef.vol bs=1M count=1 status=none
    chown 0:0 /data/cache-6-deadbeefdeadbeef.vol; chmod 666 /data/cache-6-deadbeefdeadbeef.vol' >/dev/null 2>&1
}

# The racer: the peer identity (uid 999, member of 918) hammering the stem name
# between a real empty file and a symlink pointing outside the cache.
racer='while :; do
  ln -sf /secret/SENTINEL /data/cache.vol 2>/dev/null
  rm -f /data/cache.vol 2>/dev/null
  : > /data/cache.vol 2>/dev/null
  rm -f /data/cache.vol 2>/dev/null
done'

run_race() {
  local body="$1" i
  for i in $(seq 1 25); do
    seed_open_volume
    docker run -d --name "${RUN_ID}-racer" --user 999:918 \
      -v "${RUN_ID}-hostile:/data" -v "$SECRET_DIR:/secret" \
      --entrypoint sh "$WORKER_IMAGE" -c "$racer" >/dev/null 2>&1
    docker run --rm -v "${RUN_ID}-hostile:/data" -v "$SECRET_DIR:/secret" \
      --entrypoint bash "$WORKER_IMAGE" -c "$body" >/dev/null 2>&1
    docker rm -f "${RUN_ID}-racer" >/dev/null 2>&1
    if [ "$(stat -c '%u' "$SECRET_DIR/SENTINEL")" != "$SENTINEL_UID" ]; then
      echo "$i"
      return 0
    fi
  done
  echo ""
  return 1
}

docker volume create "${RUN_ID}-hostile" >/dev/null

# Control: the vulnerable shape this PR replaces — stat-then-chown by path, no
# directory lock, chown without -h.
vuln='for p in /data/cache.vol; do
        [ -e "$p" ] || continue
        [ -L "$p" ] && continue
        chown 918:918 "$p" 2>/dev/null
      done'
reset_sentinel
hit="$(run_race "$vuln")"
if [ -n "$hit" ]; then
  pass "control: the vulnerable shape loses the race (SENTINEL chowned on iteration $hit) — the test can detect the bug"
else
  fail "control: the race never landed in 25 iterations; the hostile-peer legs below prove nothing"
fi

# The real thing: the shipped entrypoint library.
reset_sentinel
real='. /docker/lib-cache-perms.sh; ps_prepare_data_dir /data /data/cache.vol'
hit="$(run_race "$real")"
if [ -z "$hit" ]; then
  pass "the shipped migration never dereferences the peer's symlink (25 iterations, SENTINEL still root-owned)"
else
  fail "the shipped migration chowned an out-of-tree file on iteration $hit"
fi
sentinel_owner="$(stat -c '%u' "$SECRET_DIR/SENTINEL")"
if [ "$sentinel_owner" = "$SENTINEL_UID" ]; then
  pass "the out-of-tree sentinel still belongs to uid $SENTINEL_UID"
else
  fail "the out-of-tree sentinel is now owned by uid $sentinel_owner (was $SENTINEL_UID)"
fi
rm -rf "$SECRET_DIR"
docker volume rm "${RUN_ID}-hostile" >/dev/null 2>&1 || true

# --------------------------------------------------------------------------
head_ "8. Peer-authored cache content is never adopted"

# Root-owned content is the pre-2.1 daemon's and is migrated.  Content owned by
# the peer uid is of unknown authorship — the peer could create files in a 0777
# /data — and must be refused rather than laundered into the optimizer's mmap.
docker volume create "${RUN_ID}-peervol" >/dev/null
docker run --rm -v "${RUN_ID}-peervol:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data
  dd if=/dev/zero of=/data/cache-6-deadbeefdeadbeef.vol bs=1M count=1 status=none
  chown 999:999 /data/cache-6-deadbeefdeadbeef.vol; chmod 666 /data/cache-6-deadbeefdeadbeef.vol' >/dev/null 2>&1

out="$(docker run --rm -v "${RUN_ID}-peervol:/data" -e ACCEPT_EULA=Y "$WORKER_IMAGE" 2>&1)"
rc=$?
if [ "$rc" = "78" ] && echo "$out" | grep -q "owned by a non-root uid is never adopted"; then
  pass "a peer-owned cache volume is refused (exit 78), not adopted"
else
  fail "a peer-owned cache volume was not refused as expected (exit $rc)"
  echo "$out" | tail -12 | sed 's/^/          /'
fi
# 2700: chmod preserves a directory's setgid bit for short octal modes, so the
# lock mode is 2700 rather than 0700. What matters is that group and other have
# no permission bits at all, which is what the second test states directly.
dmode="$(docker run --rm -v "${RUN_ID}-peervol:/data" --entrypoint sh "$WORKER_IMAGE" -c 'stat -c %a /data' 2>/dev/null)"
if [ "$dmode" = "2700" ]; then
  pass "the refused volume is left inert at /data 2700, not half-migrated"
else
  fail "/data left at mode '$dmode' after a refusal, expected 2700"
fi
grp="$(docker run --rm -v "${RUN_ID}-peervol:/data" --user 999:918 --entrypoint sh "$WORKER_IMAGE" -c 'ls /data >/dev/null 2>&1 && echo REACHABLE || echo denied' 2>/dev/null)"
if [ "$grp" = "denied" ]; then
  pass "and a group-member peer cannot reach into the refused directory"
else
  fail "a group-member peer can still reach the refused directory ($grp)"
fi
docker volume rm "${RUN_ID}-peervol" >/dev/null 2>&1 || true

# pagespeed.json is applied at startup BEFORE the command line is parsed, so a
# peer-owned copy is a way to set the Web Bot Auth / RSL-CAP key directories
# the daemon fetches its verification keys from. Adopting one would be a
# signature-verification bypass, not a permissions nicety.
docker volume create "${RUN_ID}-peerjson" >/dev/null
docker run --rm -v "${RUN_ID}-peerjson:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data
  printf "{\"rsl_cap_key_directories\":[\"https://attacker.invalid/jwks\"]}" > /data/pagespeed.json
  chown 999:999 /data/pagespeed.json; chmod 666 /data/pagespeed.json' >/dev/null 2>&1
out="$(docker run --rm -v "${RUN_ID}-peerjson:/data" -e ACCEPT_EULA=Y "$WORKER_IMAGE" 2>&1)"
rc=$?
if [ "$rc" = "78" ] && echo "$out" | grep -q "daemon-read state owned by a non-root uid"; then
  pass "a peer-owned pagespeed.json is refused (exit 78), never adopted"
else
  fail "a peer-owned pagespeed.json was not refused (exit $rc)"
  echo "$out" | tail -12 | sed 's/^/          /'
fi
owner="$(docker run --rm -v "${RUN_ID}-peerjson:/data" --entrypoint sh "$WORKER_IMAGE" -c 'stat -c %u /data/pagespeed.json' 2>/dev/null)"
if [ "$owner" = "999" ]; then
  pass "and it still belongs to the peer — the entrypoint did not touch it"
else
  fail "the peer-owned pagespeed.json changed owner to '$owner'"
fi
docker volume rm "${RUN_ID}-peerjson" >/dev/null 2>&1 || true

# An arbitrary name that merely shares the stem prefix is cache-adjacent state
# by the daemon's own matching rule, so the same uid-0 requirement applies.
docker volume create "${RUN_ID}-peersib" >/dev/null
docker run --rm -v "${RUN_ID}-peersib:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data; printf hostile > /data/cachefoo
  chown 999:999 /data/cachefoo; chmod 666 /data/cachefoo' >/dev/null 2>&1
out="$(docker run --rm -v "${RUN_ID}-peersib:/data" -e ACCEPT_EULA=Y "$WORKER_IMAGE" 2>&1)"
rc=$?
if [ "$rc" = "78" ]; then
  pass "a peer-owned stray file sharing the cache stem prefix is refused, not adopted"
else
  fail "a peer-owned '/data/cachefoo' was not refused (exit $rc)"
  echo "$out" | tail -8 | sed 's/^/          /'
fi
docker volume rm "${RUN_ID}-peersib" >/dev/null 2>&1 || true

# The generation counter is the one non-root adoption that survives, because
# the volume in #1456 has a peer-owned one and its content is checked to be
# digits. A .gen that is NOT digits must still refuse.
docker volume create "${RUN_ID}-peergen" >/dev/null
docker run --rm -v "${RUN_ID}-peergen:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data; printf "../../etc/passwd" > /data/cache.vol.gen
  chown 999:999 /data/cache.vol.gen; chmod 666 /data/cache.vol.gen' >/dev/null 2>&1
out="$(docker run --rm -v "${RUN_ID}-peergen:/data" -e ACCEPT_EULA=Y "$WORKER_IMAGE" 2>&1)"
rc=$?
if [ "$rc" = "78" ] && echo "$out" | grep -q "not a plain generation counter"; then
  pass "a non-numeric generation counter is refused even though .gen adopts from any uid"
else
  fail "a non-numeric .gen was not refused (exit $rc)"
  echo "$out" | tail -8 | sed 's/^/          /'
fi

# The generation check has to look at the WHOLE file. A line-oriented digits
# test is satisfied by any file with a blank line in it, because the empty line
# matches -- so these payloads were adopted while the code claimed they could
# not be. Each is a peer-owned .gen and each must be refused.
for payload in '\n../../etc/passwd' 'https://attacker.invalid/jwks\n\n' 'HOSTILE\n6\n' '1\n2\n'; do
  docker run --rm -v "${RUN_ID}-peergen:/data" --entrypoint sh "$WORKER_IMAGE" -c "
    chmod 777 /data; rm -f /data/cache.vol.gen
    printf '%b' '$payload' > /data/cache.vol.gen
    chown 999:999 /data/cache.vol.gen; chmod 666 /data/cache.vol.gen" >/dev/null 2>&1
  out="$(docker run --rm -v "${RUN_ID}-peergen:/data" -e ACCEPT_EULA=Y "$WORKER_IMAGE" 2>&1)"
  rc=$?
  if [ "$rc" = "78" ] && echo "$out" | grep -q "not a plain generation counter"; then
    pass "a peer-owned .gen containing $(printf '%s' "$payload" | head -c 34) is refused"
  else
    fail "a peer-owned .gen containing '$payload' was NOT refused (exit $rc)"
  fi
done
docker volume rm "${RUN_ID}-peergen" >/dev/null 2>&1 || true

# ...and an EMPTY one must still be adopted. A truncated .gen on a pre-2.1
# volume is the same situation as the 0-byte stem: the daemon reads an
# unparsable counter as generation 0 and cold-starts, so refusing it would
# recreate the outage this path exists to prevent.
docker volume create "${RUN_ID}-emptygen" >/dev/null
docker run --rm -v "${RUN_ID}-emptygen:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data; : > /data/cache.vol.gen
  chown 999:999 /data/cache.vol.gen; chmod 666 /data/cache.vol.gen' >/dev/null 2>&1
docker run -d --name "${RUN_ID}-worker" --shm-size=256m -v "${RUN_ID}-emptygen:/data" \
  -e CACHE_SIZE=67108864 -e ACCEPT_EULA=Y "$WORKER_IMAGE" >/dev/null
if wait_for_worker "${RUN_ID}-worker"; then
  pass "an empty (truncated) generation counter is adopted, not refused"
else
  fail "an empty .gen was refused — a truncated counter would recreate the outage"
  docker logs "${RUN_ID}-worker" 2>&1 | tail -12 | sed 's/^/          /'
fi
gmode="$(docker exec "${RUN_ID}-worker" sh -c 'stat -c "%a %u:%g" /data/cache.vol.gen' 2>/dev/null)"
info "adopted empty .gen: $gmode"
docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true
docker volume rm "${RUN_ID}-emptygen" >/dev/null 2>&1 || true

# The peer must be able to READ the generation counter and not to rewrite it:
# the daemon writes it 0644 "nginx needs read access" and the peer never writes.
docker volume create "${RUN_ID}-genmode" >/dev/null
docker run --rm -v "${RUN_ID}-genmode:/data" --entrypoint sh "$WORKER_IMAGE" -c '
  chmod 777 /data; printf 6 > /data/cache.vol.gen
  chown 0:0 /data/cache.vol.gen; chmod 666 /data/cache.vol.gen' >/dev/null 2>&1
docker run --rm -v "${RUN_ID}-genmode:/data" -e ACCEPT_EULA=Y --entrypoint bash "$WORKER_IMAGE" \
  -c '. /docker/lib-cache-perms.sh; ps_prepare_data_dir /data /data/cache.vol' >/dev/null 2>&1
gen="$(docker run --rm --user nginx -v "${RUN_ID}-genmode:/data" --entrypoint sh "$NGINX_IMAGE" -c '
  if (head -c1 /data/cache.vol.gen) >/dev/null 2>&1; then echo "GEN: readable"; else echo "GEN: NOT-READABLE"; fi
  if (: > /data/cache.vol.gen) >/dev/null 2>&1; then echo "GEN: WRITABLE"; else echo "GEN: not-writable"; fi' 2>&1)"
info "peer probe on the generation counter:"; echo "$gen" | sed 's/^/          /'
if echo "$gen" | grep -q "GEN: readable" && echo "$gen" | grep -q "GEN: not-writable"; then
  pass "the peer can read the generation counter but not rewrite it"
else
  fail "wrong peer capabilities on the generation counter"
fi
docker volume rm "${RUN_ID}-genmode" >/dev/null 2>&1 || true

# --------------------------------------------------------------------------
head_ "9. Explicit non-root start (compose \`user:\`)"

docker volume create "${RUN_ID}-asuser" >/dev/null
docker run -d --name "${RUN_ID}-worker" --user "${PS_UID}:${PS_GID}" \
  -v "${RUN_ID}-asuser:/data" -e CACHE_SIZE=67108864 -e ACCEPT_EULA=Y \
  "$WORKER_IMAGE" >/dev/null
if wait_for_worker "${RUN_ID}-worker"; then
  pass "starts with --user ${PS_UID}:${PS_GID} on a fresh volume (image /data ownership is inherited)"
else
  fail "did not start with --user ${PS_UID}:${PS_GID}"
  docker logs "${RUN_ID}-worker" 2>&1 | tail -20
fi
if wait_for_log "${RUN_ID}-worker" "container started as uid ${PS_UID}"; then
  pass "and says it skipped the root-only preparation step"
else
  fail "no log line naming the non-root start"
fi
ww="$(docker exec "${RUN_ID}-worker" sh -c 'find /data -perm -o+w -print' 2>/dev/null)"
[ -z "$ww" ] && pass "nothing world-writable under --user" || fail "world-writable under --user: $ww"
docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true

# The world-writable mount warning (a Kubernetes emptyDir arrives 0777 and the
# entrypoint cannot fix it without root, so it has to be loud).
docker run --rm -v "${RUN_ID}-asuser:/data" --entrypoint sh "$WORKER_IMAGE" -c 'chmod 777 /data' >/dev/null 2>&1
out="$(docker run --rm --user "${PS_UID}:${PS_GID}" -v "${RUN_ID}-asuser:/data" \
  -e ACCEPT_EULA=Y --entrypoint bash "$WORKER_IMAGE" -c \
  '. /docker/lib-cache-perms.sh; ps_prepare_data_dir /data /data/cache.vol' 2>&1)"
if echo "$out" | grep -q "WORLD-WRITABLE" && echo "$out" | grep -q "fsGroup"; then
  pass "a world-writable mount under --user warns loudly and names fsGroup: ${PS_GID}"
else
  fail "no world-writable warning under --user:"
  echo "$out" | tail -8 | sed 's/^/          /'
fi

# A uid without the shared gid runs perfectly well and shares nothing — the
# silent no-sharing case the whole contract exists to make impossible.
out="$(docker run --rm --user "${PS_UID}:0" -v "${RUN_ID}-asuser:/data" \
  -e ACCEPT_EULA=Y --entrypoint bash "$WORKER_IMAGE" -c \
  '. /docker/lib-cache-perms.sh; ps_prepare_data_dir /data /data/cache.vol' 2>&1)"
if echo "$out" | grep -q "effective gid is 0, not ${PS_GID}" && echo "$out" | grep -q "share NOTHING"; then
  pass "a non-root start with the wrong gid warns that nothing will be shared"
else
  fail "no wrong-gid warning under --user:"
  echo "$out" | tail -8 | sed 's/^/          /'
fi
docker volume rm "${RUN_ID}-asuser" >/dev/null 2>&1 || true

# --------------------------------------------------------------------------
head_ "10. Pre-contract images keep the old behaviour (the tools/ harnesses)"

# The in-repo harnesses build their own worker images from the base runtime and
# exec this entrypoint without the identity or the library.  They must be
# untouched by all of the above, or this change silently reaches the periodic
# compose suites.
harness_ctx="$(mktemp -d)"
docker run --rm --entrypoint sh "$WORKER_IMAGE" -c 'cat /docker/entrypoint-worker.sh' \
  > "$harness_ctx/entrypoint-worker.sh" 2>/dev/null
cat > "$harness_ctx/Dockerfile" <<DOCKERFILE
FROM ${WORKER_IMAGE}
RUN userdel pagespeed && groupdel pagespeed 2>/dev/null || true
# g-s first: this control derives FROM the worker image, so /data arrives 2750.
# A harness image creates /data at runtime under umask 0000 and never has the
# setgid bit, and chmod preserves it for short octal modes -- so without this
# the control would report 2777 where the real thing reports 777.
RUN rm -f /docker/lib-cache-perms.sh && chmod g-s /data && chmod 777 /data
DOCKERFILE
if docker build -q -t "${RUN_ID}-harness" "$harness_ctx" >/dev/null 2>&1; then
  docker volume create "${RUN_ID}-harness" >/dev/null
  docker run -d --name "${RUN_ID}-worker" -v "${RUN_ID}-harness:/data" \
    -e CACHE_SIZE=67108864 -e ACCEPT_EULA=Y "${RUN_ID}-harness" >/dev/null
  if wait_for_worker "${RUN_ID}-worker"; then
    st="$(docker exec "${RUN_ID}-worker" sh -c 'stat -c %a /data; stat -c %a /data/cache.vol' 2>/dev/null | tr '\n' ' ')"
    uid="$(docker exec "${RUN_ID}-worker" sh -c 'ps -o uid= -C factory_worker | head -1' 2>/dev/null | tr -d ' ')"
    if [ "$st" = "777 666 " ] && [ "$uid" = "0" ]; then
      pass "a pre-contract image is bit-for-bit unchanged (/data 777, cache.vol 666, daemon uid 0)"
    else
      fail "pre-contract image behaviour drifted: modes '$st' uid '$uid'"
    fi
  else
    fail "a pre-contract image no longer starts"
    docker logs "${RUN_ID}-worker" 2>&1 | tail -15
  fi
  docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true
  docker volume rm "${RUN_ID}-harness" >/dev/null 2>&1 || true
  docker rmi -f "${RUN_ID}-harness" >/dev/null 2>&1 || true
else
  fail "could not build the pre-contract control image"
fi
rm -rf "$harness_ctx"

# --------------------------------------------------------------------------
if [ -n "$COMBINED_IMAGE" ]; then
  head_ "11. Combined image (one container, both processes)"
  docker volume create "${RUN_ID}-comb" >/dev/null
  docker run -d --name "${RUN_ID}-worker" --shm-size=256m -v "${RUN_ID}-comb:/data" \
    -e BACKEND_HOST=127.0.0.1 -e BACKEND_PORT=1 -e CACHE_SIZE=67108864 -e ACCEPT_EULA=Y \
    "$COMBINED_IMAGE" >/dev/null
  if wait_for_worker "${RUN_ID}-worker"; then
    pass "combined image starts and publishes its socket"
  else
    fail "combined image did not start"
    docker logs "${RUN_ID}-worker" 2>&1 | tail -20
  fi
  # The combined entrypoint starts nginx only AFTER the optimizer's socket
  # appears, so wait_for_worker returning is not the same as the container being
  # fully up. Wait for nginx itself before probing identities.
  for _ in $(seq 1 30); do
    docker exec "${RUN_ID}-worker" sh -c 'pgrep -x nginx >/dev/null' 2>/dev/null && break
    sleep 1
  done
  procs="$(docker exec "${RUN_ID}-worker" ps -eo uid,comm 2>/dev/null | grep -E 'factory_worker|nginx')"
  info "processes (deduplicated):"; echo "$procs" | awk '{print $1" "$2}' | uniq -c | sed 's/^/          /'
  if echo "$procs" | grep -qE "^ *${PS_UID} +factory_worker" && echo "$procs" | grep -qE "^ *0 +nginx"; then
    pass "the optimizer runs as ${PS_UID} while nginx keeps a root master for :80"
  else
    fail "combined image process identities are wrong"
  fi
  ww="$(docker exec "${RUN_ID}-worker" sh -c 'find /data -perm -o+w -print' 2>/dev/null)"
  [ -z "$ww" ] && pass "combined: nothing under /data is world-writable" \
    || fail "combined: world-writable entries: $ww"
  ngroups="$(docker exec "${RUN_ID}-worker" sh -c \
    'for p in /proc/[0-9]*; do
       grep -q "^Uid:.[^0]" "$p/status" 2>/dev/null && grep -q nginx "$p/comm" 2>/dev/null \
         && grep "^Groups:" "$p/status" && break
     done' 2>/dev/null)"
  if echo "$ngroups" | grep -qw "$PS_GID"; then
    pass "combined: nginx workers carry group $PS_GID ($ngroups)"
  else
    fail "combined: nginx workers lack group $PS_GID ($ngroups)"
  fi
  docker rm -f "${RUN_ID}-worker" >/dev/null 2>&1 || true
  docker volume rm "${RUN_ID}-comb" >/dev/null 2>&1 || true
else
  info "no combined image supplied (3rd argument); skipping its leg"
fi

# --------------------------------------------------------------------------
echo
echo "=============================================================="
echo "cache-sharing image gate: $((CHECKS - FAILURES))/$CHECKS checks passed"
echo "=============================================================="
[ "$FAILURES" -eq 0 ] || exit 1
