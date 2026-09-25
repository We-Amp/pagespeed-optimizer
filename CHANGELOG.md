# Changelog

All notable changes to mod_pagespeed 2.1 are documented in this file.

## Unreleased

Security: on Windows, a local privilege-escalation issue is fixed. A Windows
host is exposed when other local code (for example another IIS application
pool or a Windows service) runs next to an application that uses
ModPageSpeed. Affected: the `WeAmp.PageSpeed.AspNetCore` and
`WeAmp.PageSpeed.NativeAssets.Windows` NuGet packages (the latter carries
`pagespeed.dll`), versions 2.0.0 through 2.1.0, and the Windows optimizer
built from them. Update recommended. Linux and macOS are not affected.

Fixed: the Windows optimizer worker (`factory_worker.exe`) starts on a
Windows Server that has no Visual C++ runtime installed. It used to fail at
once with a missing-DLL error (exit code 0xC0000135) for any argument,
`--help` included, unless the Visual C++ 2015-2022 redistributable was
present. The worker now carries the runtime itself, as `pagespeed.dll`
already did, so nothing beyond the operating system is needed.

Added: the Windows optimizer worker runs as a Windows service. Register it
with `--service` on its command line and the Service Control Manager starts
it, sees it as running once it is ready, and stops it gracefully (the same
shutdown Ctrl+C gives) on a service stop or at system shutdown. A start the
worker refuses is reported with the worker's exit code. A service has no
console, so `--log-file PATH`, valid only with `--service`, appends the log
to a file that can be read while the service runs; a log file that cannot be
opened stops the service with service-specific exit code 90. Run from a
console, `--service` is refused with a message saying so.

## [2.1.0] - 2026-09-17

Changed (plan for this before you upgrade): **the disk cache starts empty.**
This release moves the optimizer to a new on-disk cache format. The new
binary opens a new cache file instead of converting the old one, so every
deployment begins with a cold cache that refills as traffic arrives. Nothing
is lost that cannot be rebuilt and no configuration change is needed, but
expect a temporary drop in cache hit rate and a matching rise in origin
fetches and optimization work until traffic has warmed the cache again --
so upgrade at a quiet hour if your origin is sensitive to that.

Check your free space before you start. During the upgrade both the old and
the new cache file exist, so the cache directory needs room for a second
volume: free space at least equal to your configured cache size, on top of
whatever the existing volume already occupies. If the directory has that
room, leave the old file where it is -- that is what makes a rollback warm --
and delete it once you are confident you will not roll back. **If it does
not, stop the optimizer, delete the old cache file, and only then start the
new one.** The new volume is extended to its full configured size while the
optimizer starts, ahead of any cleanup, so starting first on a directory
sized for a single volume can exhaust the disk. The file is allocated
sparsely, so that failure would not show up at the upgrade: it would show up
hours later, mid-traffic, as the cache warms into space that is not there.

If a web server on the same host carries the serving module, start the
optimizer before it: until the optimizer has created its new volume the module
refuses to start rather than degrading, so the web server does not come up.
At boot the packaged unit now handles the order for you -- see UPGRADING.md.

Downgrading is warm provided you still have the old file -- an earlier binary
reopens it untouched, and nothing in this release removes it: a full cache
purge (`POST /v1/cache/purge` with `"scope":"all"`) and the automatic recovery
the web-server module runs when it cannot open the cache both clear only the
volumes of the format they are running, so the rollback survives them. (A
volume still sitting at the un-fingerprinted legacy path, written by a build
that predates format-stamped filenames, carries no format to respect and is
still cleared.) The old
file is therefore yours to delete, whenever you are confident you will not
roll back to the build that wrote it.

Fixed: on a host that was momentarily busy when the optimizer started, the
browser-analysis sandbox probe could declare the sandbox unavailable for the
daemon's whole lifetime. The probe gives its test process a bounded wait, and
when that wait elapsed without an answer -- say because a loaded machine
simply never scheduled the probe inside it -- the refusal was reported with
the timeout's generic remedy instead of the real cause. A probe attempt that
times out, or whose process cannot be started under resource pressure, is now
retried before any outcome is reported, so a transient stall resolves to the
answer the next attempt reaches: when a syscall filter is responsible, the
refusal again names the SIGSYS and the remedy that actually fixes it.

Fixed: a URL whose optimized variants were re-recorded many times could
permanently stop accepting new ones. Every re-record used to leave the
superseded copy linked behind the new one, so the stored chain for that URL
grew on every refresh while the number of distinct variants stayed put,
until it reached its limit and every further write to that URL was refused.
The URL then froze: reads kept succeeding, so nothing looked wrong, but the
browser was served an increasingly stale version that was never replaced.
Superseded copies are now unlinked as the replacement is written, so the
chain no longer grows with refreshes. A chain that still reaches the limit
recovers by itself only in the narrow case where the URL holds that one
variant and nothing else; a URL that also holds its original -- the ordinary
case for an optimized page -- is not covered by that reset, and is recovered
instead by clearing the entry at the next revalidation.

Fixed: after content was re-optimized, replaced or purged, the in-memory
cache tier could keep serving the superseded copy until it happened to be
evicted. The stale in-memory entry is now dropped when the content behind it
is re-recorded or removed, so a refresh takes effect immediately instead of
after an unpredictable delay.

Changed: the cache enforces a 64 MB ceiling on a single stored object. The
limit was already documented but was never applied; an over-size write is now
rejected before anything is buffered, which also bounds how much of one
response the cache holds in memory at once. Almost nothing reaches it in
practice: cached originals are capped at 16 MiB well before this, so it
applies only to an optimized variant, and an optimized variant that large is
outside anything the optimizer produces.

Changed: the bundled Cyclone cache library is licensed under the Apache
License 2.0, matching the rest of the distribution. `NOTICE` and the SBOM
record it.

Changed: license — the source tree is now under the Apache License 2.0 (was
BUSL-1.1).

Changed: mod_pagespeed 2.1 is licensed under the Apache License 2.0. Every feature is
available to everyone. The console shows a single dismissible pointer to
support subscriptions. A 2.0 configuration starts unchanged (see
UPGRADING.md).

Changed: the NOTICE product line and the SBOM root package now use the
mod_pagespeed 2.1 identity; the SBOM file is `sbom/mod_pagespeed-2.1.spdx.json`.

Added (host packages): the deb and rpm packages ship `THIRD-PARTY-NOTICES`
next to `LICENSE` and `NOTICE` (`/usr/share/doc/pagespeed-optimizer/` on
Debian and Ubuntu, `/usr/share/licenses/pagespeed-optimizer/` on Enterprise
Linux), so the notices of the components statically linked into the daemon
travel with the binaries the way the Docker images and the NuGet packages
already carried them. `THIRD-PARTY-NOTICES` now also lists the Rust standard
library and the `flo_curves`, `color_quant`, `scoped_threadpool` and
`miniz_oxide` crates that the SVG vectorizer links.

Fixed: a cache purge, and the automatic recovery the web-server module runs
when it cannot open the cache, no longer delete a cache file left by a
previous release's on-disk format. Both cleared every cache file belonging to
this cache whatever format it was written in, so either one silently removed
the volume an operator was keeping in order to roll back. Files from another
format are now left alone -- they are not this build's data to remove -- which
means they stay on disk until you delete them yourself.

Fixed: a URL whose stored variants could no longer be replaced or removed --
the cache accepted the removal and kept serving the same stale entry -- was
served unoptimized while being re-optimized over and over, and never
recovered. The optimizer now clears such a URL's cache entry outright at the
next revalidation, so the URL is optimized again from the fresh origin
response instead of repeating the cycle.

Fixed: the optimizer's systemd unit now starts ahead of `apache2.service` /
`httpd.service` / `nginx.service` and counts as started only once its notify
socket exists
(bounded 30 s wait), so a web server that carries the module directives and
boots on the same host finds the daemon on its first attempt.

Removed: the `/v1/license/*` endpoints (404), the `license` object and
`checks.license` entry in `/v1/health`, and the `x-pagespeed-warn` response
header. Monitoring that keyed on `checks.license.pass` should key on `ready`
/ `status` instead.

Changed: `agent_optimize` (the same-URL markdown variant for AI agents), the
synthesized `/llms.txt` index and the browser-analysis suite are enabled by
their operator flags alone (`--agent-optimize`, `--agent-optimize-llms-txt`,
`--enable-browser-analysis`). The `agent_optimize_entitled` key in
`pagespeed-shared.conf` keeps its name for compatibility with 2.0-era readers
and mirrors the operator flag.

Removed: instance telemetry. The daemon no longer derives a machine id or
writes `pagespeed.instance-id`, and it makes no outbound request of its own
accord; the only egress is what the operator configures (origin fetches, the
Web Bot Auth and RSL-CAP key directories). A `pagespeed.instance-id` file a
2.0 install left behind is left in place and never read.

Changed (host packages): the optimizer daemon's systemd unit now **enforces**
its system-call allow-list by default. Earlier 2.1 release candidates shipped
the unit log-only, with enforcement an opt-in drop-in the operator installed
after a soak; the allow-list — systemd's `@system-service` group minus
`@privileged` and `@resources`, plus `RestrictNamespaces=yes` — now lives in
the unit itself, and the kernel terminates the daemon (`SIGSYS`, exit status
31) on the first call outside it. The list is in the unit rather than a
drop-in on purpose: no drop-in can be installed "alone" any more, which
retires the earlier warning that the browser-analysis file must never be
installed without its companion. The flip rests on the syscall census across
three distributions and the package's own verification, which on every build
runs the real daemon and a real headless Chrome under the enforcing profile
and asserts no kill; the unit keeps `SystemCallLog=~@system-service`, so calls
outside the group are still recorded. What changes on upgrade: the host
enforces from the service restart the package performs on install. Browser
analysis works under it with no extra step: the `20-browser-analysis.conf`
re-admission list for headless Chrome's own sandbox ships installed and
active as a vendor drop-in next to the unit (it only adds to the allow-list);
a host without browser analysis can mask it with an empty same-named file
under `/etc/systemd/system/pagespeed-optimizer.service.d/` to take the
narrower daemon-only profile. Opting out of enforcement: copy
`/usr/share/doc/pagespeed-optimizer/90-syscall-filter-off.conf.example` into
that `.service.d/` directory, `systemctl daemon-reload`, restart — it resets
both directives, and it survives `apt upgrade` and `dnf upgrade` because the
package never writes under `/etc/systemd`. Confirm
either posture with `systemctl show -p SystemCallFilter pagespeed-optimizer`
(a long allow-list when enforcing, `SystemCallFilter=~` when opted out); the
daemon's own `syscall_filter` health field says `"filtered"` in both. A kill
by the profile reads `code=dumped, status=31/SYS` in `systemctl status` (no
dump is written), or `Chrome exited (status=0, signal=31)` in the journal
when the browser was the victim; the kernel's audit record (type `1326`)
names the call by number — `ausearch -m SECCOMP -i` where `auditd` runs,
`journalctl -k | grep 'type=1326'` where it does not (a kill carries `sig=31`;
`sig=0` records are the log filter's entries), `ausyscall <N>` to name a bare number. Update recommended for host
installs.

Fixed: origin refresh no longer false-purges unchanged origins every
freshness cycle (#1503). Both front ends fire the origin-refreshed sentinel
unconditionally on an age-expired re-fetch, and the worker purged the URL's
whole variant set on arrival — and with the identity/original-format slot
holding the worker's own proactive variant (flagged `kFlagWorkerProcessed`),
the inline rebuild was refused, so every TTL expiry cost a purge plus a
deferred re-optimization of identical bytes (on a module-serves front end
that never serves stale, a recurring dead window with nothing optimized to
serve at all). The worker now decides changed-vs-unchanged first, hashing
the pristine origin reference — the durable-original sentinel (`0x0C`) read
by exact id, else a genuine flag-clear identity, never the worker-processed
slot — against the stored content-hash oracle. An unchanged origin with a
freshly re-recorded reference gets its variant set restamped in place from
the fresh origin metadata (no purge, no rebuild), where "fresh" is
discriminated relatively — the re-record postdates the variant set's own
stamps — so an Age-backdated re-record from a CDN-fronted origin is
recognised as the re-record rather than deferred forever; an unchanged
reference
whose re-record has not landed yet (the 1.16 module sends the sentinel ahead
of its re-record) defers the decision to the record+notify that follows,
where the image branch now runs its content-hash check over the pristine
reference as well and restamps on a match. A genuinely changed origin still
purges and rebuilds exactly as before. Text resources (CSS/JS, and HTML
without `agent_optimize`) carry no content-hash oracle and keep the
per-cycle purge — the false-purge fix covers URLs with the oracle (all
images, agent-markdown HTML). New counters: `origin_refresh.unchanged`
and `origin_refresh.deferred` in `/v1/stats`.

Added: `GET /v1/stats` (and the `/v1/ws/stats` stream, which shares the same
builder) now carries a `serve_classes` block whenever the serve-stats mmap is
available: the five-class serve partition (`optimized`, `original_cold`,
`original_pending`, `original_declined`, `original_skew`), the orthogonal
`notify_suppressed` counter, and the `class_unrecognized` /
`flags_unrecognized` drop counters that keep the partition honest. These
counters are written by the serving front end — live where the 1.16 module
(mod_pagespeed) fronts the cache, still all-zero under an nginx 2.0 or
ASP.NET front end, where all-zero means "not instrumented", not "nothing
happened". No struct or ABI change; the block is read from the existing v8
serve-stats layout.

Fixed: `systemctl reload pagespeed-optimizer` terminated the daemon instead
of reloading it. The unit declared no `ExecReload=`, so systemd fell back to
sending SIGHUP to the main process, and the daemon — which installed no
SIGHUP handler — died on it; with `Restart=on-failure` it came straight
back, so every "reload" was an unannounced restart that dropped in-flight
optimizations and reopened the cache volume. Reload has no honest semantics
for this daemon — its settings are fixed at startup, and the hot-reloadable
subset lives behind `PATCH /v1/config` — so the daemon now ignores SIGHUP
and the unit declares an `ExecReload=` that fails loudly and points at
`systemctl restart pagespeed-optimizer`. Update recommended for host
installs.

Fixed: a package upgrade left `/etc/default/pagespeed-optimizer` at 0644
root:root. The file is one of the two `EnvironmentFile`s the optimizer unit
reads, and the design gate for those files is 0640 root:pagespeed (the other,
`/etc/pagespeed-optimizer/daemon.env`, carries the generated API/PURGE tokens
and was already enforced). The gap was the upgrade path: dpkg and rpm replace
an operator-unedited `/etc/default` file with the payload's mode and
ownership, and nothing re-asserted the gate afterwards — fresh installs were
fine, upgrades were not. The packages now ship the file at 0640, the
maintainer scripts re-pin the mode on every install and upgrade (an edited
file keeps its contents; only the mode is re-pinned), and a tmpfiles.d `z`
line re-scopes the group to `pagespeed` on every install, upgrade and boot.
Update recommended for host installs.

Fixed: the companion `20-browser-analysis.conf` drop-in shipped as an
example for deployments that run browser analysis under the opt-in enforcing
syscall profile did not re-admit everything headless Chrome's sandbox needs:
with the 1.16.0-rc.10 copy installed, enforcement killed Chrome at launch and
browser analysis backed off to unavailable. The drop-in has now been measured
against a running Chromium under the packaged service unit and completed; the
enforcing profile itself and the default unit are unchanged. One name arrived
after that measurement: `mincore`, which Chromium's MemoryInfra thread calls
in a burst every few minutes — every shorter observation window
records zero calls, so the miss only surfaced when a burst happened to land
inside a CI render cycle and the enforcing profile killed the browser
process. The name is now re-admitted. Host installs:
the package now suggests a browser and the packaged `/etc/default` file and
the browser-analysis documentation say which package to install and how to
point the daemon at it. Update recommended if you enabled the enforcing
drop-in together with browser analysis.

Fixed: with browser analysis enabled and a persistent profile directory, the
headless browser could not start after the optimizer's container was
recreated (an image upgrade, a `docker compose up` that changed the service,
a host reboot) or after a host's name changed: the profile kept a lock from
the previous browser, every launch exited immediately with status 21, and
browser analysis stayed down until the lock was removed by hand. The
optimizer now removes a lock that belongs to another host or to a process
that is no longer running before each launch (and logs the previous owner;
a profile directory must not be shared between optimizer instances),
names a locked profile in the launch-failure log line instead of only the
exit status, and clears the failure count and retry delay reported under
`browser` in `/v1/health` once a launch has stayed up for a minute. Update
recommended if browser analysis is enabled.
Fixed: the management API serves the web console's static bundle — `GET` and
`HEAD` requests to `/console` and paths under it — without requiring the API
token, matching the documented behavior. The bundle carries no operational
data; every `/v1/*` data endpoint, and every non-GET/HEAD request, still
requires the token when one is configured.

Fixed: `GET /v1/health` and `/v1/stats` report the connection figures of the
management API listener itself in `connections` — the cap it actually enforces
(32 by default) and its live active count — rather than the notification
listener's separate `--max-connections` limit (128 by default).

Fixed: with browser analysis enabled, the 1.16.0-rc.9 container image could
not start the headless browser. Every launch failed immediately and the
optimizer retried it every few seconds, so browser-driven optimizations never
ran and the host accumulated a core dump per attempt. The optimizer now hands
the browser an environment that matches the unprivileged user it runs as, and
a browser that keeps failing at launch is retried with an increasing delay
(up to five minutes) with a single log line naming the condition; `/v1/health`
reports the failure count and the current retry delay under `browser`. A
latent fault in the browser-exit path that could stop the optimizer itself
after a browser exit is closed as well. Affects 1.16.0-rc.9 only. Update
recommended if browser analysis is enabled.

Security: the optimizer daemon's management API is no longer
open by omission, and headless Chrome no longer runs unsandboxed. Two
changes, both with an operator-visible default flip.

The management API now enforces one rule while parsing its configuration:
remote is never unauthenticated, and unauthenticated is never remote. Any
combination that breaks it is a refusal to start, with a message naming the
flag that would make the requested posture legal. Previously an unset token
made every endpoint answer without a credential -- including cache purge and
the cached-URL inventory -- and an unset token also silently opened all read
endpoints. Both are closed: an absent token now fails closed, and
`--api-read-open` is an explicit choice rather than a side effect of a
credential someone forgot. A new `--api-socket` serves the same HTTP API over
a group-scoped unix socket (mode 0660) as the recommended local transport: a
connection accepted there is an authenticated peer, because reaching the
socket already means being in the daemon's group, so no token is asked for on
reads or on cache purge and the daemon binds no TCP port at all. A TCP bind
stays available and is loopback-only unless `--api-allow-remote` and a token
are both set; the two transports are mutually exclusive, and `--api-bind`
takes an IPv4 literal (hostnames and IPv6 are refused by name at startup). Package installs now generate
`PAGESPEED_API_TOKEN` and `PAGESPEED_PURGE_TOKEN` into
`/etc/pagespeed-optimizer/daemon.env` (0640 `root:pagespeed`) once, at install
time, never overwriting an existing value -- so cache purge over the
management socket works on a stock install instead of answering "no purge
token configured", and enabling the API does not require inventing a
credential. Update recommended; review your configuration if you exposed the
management API.

Headless Chrome now runs with its own sandbox. `--no-sandbox` was previously
passed unconditionally, so every headless render of untrusted page content ran
without kernel-enforced isolation -- a review item open in this codebase since
2024. A new `--browser-sandbox` takes `require` (the default) or `off`. In
`require` mode the daemon probes at startup: if the sandbox is available Chrome
runs sandboxed, and if it is not, browser analysis refuses to initialize with
one message naming the cause and the remedy while the daemon keeps serving
without it. It never falls back to an unsandboxed browser, and there is
deliberately no mode that does. `off` is the documented opt-out and is loud:
a warning at startup and on every browser start. `GET /v1/health` and
`/v1/stats` report the resolved state as `browser_sandbox` (`"on"`,
`"unavailable"`, `"off"`, `"disabled"`), so a monitoring check can see an
unsandboxed browser without reading logs. Chrome's profile directory is also
now pinned by the daemon (0700, inside the service runtime directory) instead
of left to Chrome to choose.

Two follow-ups to that gate make its diagnostics honest. When the browser
sandbox probe is refused, the daemon now distinguishes a kernel syscall filter
denying it from a probe that simply did not answer in time -- previously both
reported the same "did not complete", which sent operators looking at the wrong
thing. The syscall-filter case names the kind of setting that produces it and
points at the browser-analysis documentation, so it is actionable rather than a
dead end. And `GET /v1/health` and `/v1/stats` now report `syscall_filter`
(`"filtered"`, `"none"`, `"unknown"`), read from the kernel's view of the
running process: a monitoring check can see whether a deployment is running
under a kernel syscall filter without reading a unit file on the host. It is
read-only reporting and not a setting -- the daemon does not install the filter
itself. Note that `"filtered"` means *a* filter is attached, not that any
particular profile is: a container started with your runtime's default seccomp
profile already reports `"filtered"`, and so does a stock package install on a
host, because the unit below attaches one.

The optimizer daemon's systemd unit now ships a hardened profile. The kernel
and host surfaces the daemon never needs are closed off -- kernel tunables,
modules and logs, control groups, the system clock, the hostname, other users'
processes, SUID/SGID bits, realtime scheduling and personality changes -- the
address families it may use are restricted to the four it actually uses, and
core dumps are disabled, because a dump of this process would contain cache
contents and credentials. Syscall filtering ships in **log-only** mode: the
unit records every system call outside systemd's normal system-service set and
lets it proceed, so an operator can read a full traffic cycle's worth of
evidence out of the audit log before turning on enforcement (where those
records land depends on whether an audit daemon is running -- the packaged
documentation gives both reads and the check that decides which applies). Enforcement is then a
drop-in the package installs as an example, plus a companion drop-in for
deployments that run browser analysis, meant to re-admit what headless
Chrome's own sandbox needs (the rc.10 copy was incomplete; see the entry
above). Install the first without the second and the daemon still
starts and still serves; browser analysis refuses to initialize and names the
restriction, rather than falling back to an unsandboxed browser.

To be precise about what changes on upgrade: **no system call is killed by
default** -- the enforcing profile is opt-in and arrives only with that
drop-in. (Superseded within this release: enforcement is now the default and
the opt-in drop-in is retired -- see the "Changed (host packages)" entry at
the top of this section.) The rest of the hardening above does ship enabled,
and is a behaviour change. The daemon can now open only unix, netlink and IPv4/IPv6
sockets; it can no longer write kernel tunables, load modules, read the
kernel log, change the system clock or hostname, acquire SUID/SGID bits or
realtime scheduling; it cannot see other users' processes in `/proc`; and it
writes no core dumps. If you depend on one of those -- a core dump for crash
triage is the realistic case -- restore it with a drop-in of your own.
Containers are covered from the other side, since a container has no systemd
and the unit profile does not apply there at all: the package now ships a
Chrome-compatible container seccomp profile. Update recommended.

Breaking for container users, 2.0 -> 2.1 (migration notes):

- **Helm pod seccomp profile.** Was: `podSecurityContext` was empty, so pods
  inherited whatever the cluster defaulted to -- on many clusters that is
  seccomp *unconfined*, which is weaker than what a plain `docker run` gives
  you. Now: the chart sets `seccompProfile.type: RuntimeDefault`. If a
  workload of yours needs a syscall the runtime's default profile blocks, opt
  out with `podSecurityContext: {seccompProfile: null}` in your values --
  Helm deep-merges values, so setting the key to `null` is what removes it;
  an empty `podSecurityContext: {}` will NOT override the chart default.

- **Management API.** Was: the container published the API on `0.0.0.0`, and
  the token was optional -- when unset, every endpoint including cache purge
  answered without a credential. Now: the image defaults to a loopback bind, a
  token is required, and a non-loopback bind refuses to start without both
  `PAGESPEED_API_ALLOW_REMOTE=true` and a token. If you deliberately published
  the API, set both and put it behind your reverse proxy's own authentication
  as well. When the API is enabled and you supply no token, the entrypoint now
  generates one per container start and prints it once to the container log —
  along with a generated PURGE token, so cache invalidation works out of the
  box. **Both are printed to stdout: if you ship container logs off-host, you
  are shipping those credentials with them.** Set `PAGESPEED_API_TOKEN` and
  `PAGESPEED_PURGE_TOKEN` yourself to keep them out of the log and stable
  across restarts.
- **Browser sandbox.** Chrome declines to sandbox itself as root. The images no
  longer run the optimizer as root (see below), so the `require` default is now
  satisfiable in a container -- but the container runtime's default seccomp
  profile also has to permit user namespaces. Where it does not, browser
  analysis refuses to start and `/v1/health` reports
  `browser_sandbox: "unavailable"` naming the cause; the daemon keeps serving.
  Run with a Chrome-compatible seccomp profile, or set
  `PAGESPEED_BROWSER_SANDBOX=off` to accept an unsandboxed browser deliberately.
- **Shared cache volume.** The worker and nginx images now share the cache
  through group `pagespeed` at a fixed **GID 918**, present in both, instead of
  making the volume world-writable. The optimizer in the worker and combined
  images runs as the unprivileged `pagespeed` user (UID 918); nginx's worker
  processes join the group. `/data` is 2750 -- readable and traversable by the
  group, writable by nobody but the optimizer, because the serving module only
  ever opens files the optimizer authored and never creates any. The cache
  volume and sockets are 0660, and nothing on the volume is world-readable or
  world-writable any more. The optimizer also starts with no capabilities and
  with new privileges disabled, so the drop cannot be walked back.

  Existing deployments do not need to change anything -- the entrypoint still
  starts as root, prepares the volume and drops privileges itself -- but three
  cases need attention. If you run the worker with an explicit `user:` /
  `--user`, use `918:918`, and make sure a bind-mounted data directory is owned
  by it. On Kubernetes set `securityContext.fsGroup: 918`. If you mount the data
  directory into a *third* container that reads the cache, add `918` to that
  container's groups.

  A volume created before this release is migrated in place on first start: the
  entrypoint takes ownership of the cache files the optimizer authors, logs each
  one, and keeps the existing cache rather than abandoning it and cold-starting.
  Only root-owned cache content is migrated -- that is the previous release's
  optimizer, the one identity whose authorship is not in question; content owned
  by any other user is refused rather than adopted. Where the entrypoint cannot
  take ownership safely it refuses **before** the optimizer starts, exits 78,
  names the path, and prints the one-line remedy -- rather than exiting into a
  restart loop whose only symptom is that the web server never starts -- and
  leaves the data directory unreadable, so nothing can reach the volume while it
  is in that state. Files already taken over before the refusal keep their new
  ownership; the printed remedy is safe to run over them. Exit 78 always means
  the configuration or the volume rather than a transient fault, which is worth
  distinguishing in restart alerting. Set
  `PAGESPEED_ADOPT_VOLUME=off` to opt out of the in-place migration and be told
  what to fix by hand instead.

Package installs are unaffected by both: the API ships disabled, and browser
analysis is off by default.

Security: the packaged optimizer daemon no longer runs
as root, and nothing it creates is world-accessible anymore. The
`pagespeed-optimizer` deb/rpm now creates a dedicated unprivileged
`pagespeed` system user (via sysusers.d), runs the daemon as
`User=pagespeed` with an empty capability set, and moves the cache into a
versioned directory `/var/cache/pagespeed-optimizer/v1` (3770 setgid+sticky,
`pagespeed:pagespeed`, tmpfiles.d). The cache volume, notify/health
sockets, and serve-stats mmap are now explicitly 0660 owner+group (were
world-writable 0666/0777 under the old `UMask=0000` unit), and
`pagespeed-shared.conf` is 0640 (was world-readable). The serving module's
web-server user reaches everything through membership in group `pagespeed`
(the module packages perform that join). Upgrades cold-start the daemon
cache by design: no installer chowns or migrates pre-existing root-owned
cache content, and the postinst prints a one-line notice when it detects
one. The daemon refuses to start — loudly, naming the cause and the
migration doc — when its cache directory is missing, unwritable, or holds
content owned by another uid. Also new: `--cache-dir` (default
`/var/cache/pagespeed-optimizer/v1`; `--cache-path` remains as the expert
override), a `cache_dir_generation` field published in the shared config
(C API 1.10: `ps_read_shared_config_generation`) so a
module/daemon generation skew fails loudly, and a secrets env file
`/etc/pagespeed-optimizer/daemon.env` (0640 `root:pagespeed`) so
`--api-token` no longer rides the process command line, where it was exposed
to other local accounts on the host. Update recommended.

Action required on upgrade: the daemon's default cache and socket paths move
with this change, but your web server keeps whatever path you configured.
After upgrading, point `pagespeed_cache_path` (nginx) or
`ModPagespeedDaemonVolumePath` / `ModPagespeedDaemonSocketPath` at
`/var/cache/pagespeed-optimizer/v1/cache` and
`/run/pagespeed-optimizer/notify.sock`, then restart the web server. Until you
do, in-place optimization stays off and the log reports the socket as absent
even though the daemon is running — the module is still looking at the old
location. The upgrade prints the same reminder.

Fixed: the JavaScript minifier now handles an object literal that spreads the
result of a call. In `{...f(), k: v}` -- a shape that bundler output and
analytics trackers produce routinely -- the spread argument was read as if it
were a property name, so the call's parenthesis was taken for a method
shorthand's parameter list and the comma that follows had nothing to close.
Minification of the file then failed and the script was served exactly as
received -- no optimized variant was written. Spread of a call is now
recognized as a value, so these files minify normally.
One deliberate change on invalid input: `{...a: 1}` -- a spread element cannot
take a property colon -- is now refused instead of tolerated, which leaves the
script byte-for-byte as received. Output for all other valid JavaScript is
unchanged. Affects all earlier releases. Update recommended if you serve
JavaScript through the optimizer daemon or the middleware.

Fixed: the JavaScript minifier no longer drops a required line break after a
`let` declaration whose binding is preceded by an HTML-style comment. `<!--`
opens a comment anywhere, and `-->` opens one at the start of a line; with
either between `let` and the name it binds, the declaration was misread as an
ordinary identifier and the line break that ends it was removed. Where that
line break was the only thing separating the declaration from the next
statement, the result no longer parsed -- and because a script is parsed as a
whole, the failure took the entire file down, not just the statement. Both
comment forms are now recognized, so the line break is preserved. (2.0.41
fixed the same misreading for `//` and `/* */` comments; the HTML comment forms
remained.) These forms are rare in modern JavaScript, and output for all other
valid JavaScript is unchanged. Affects all earlier releases. Update recommended
if you serve JavaScript through the optimizer daemon or the middleware.

Fixed: a serving module or middleware that opens the optimizer daemon's cache
volume as a member of the daemon's group — the default arrangement after the
privilege drop above — was refused with an I/O error and ran with in-place
optimization off for the life of every worker process, while still reporting
itself healthy, so a site could serve entirely unoptimized responses with no
failing check to show for it. Only the process that created the volume can set
its mode, so a peer was being refused for not changing a mode that was already
correct. The client now accepts a volume that already carries the expected
mode. It still refuses a volume whose mode is wrong, one reached through a
symlink, and — new — one that a second name is hard-linked to. Affects the
1.16.0-rc.7 package pair; update recommended.

Fixed: the ASP.NET Core host now provisions its own cache directory, so the
worker it starts comes up on a machine where the optimizer was never
installed from a package. The daemon validates its cache directory at
startup and refuses to start when the directory is absent — the right
behaviour for the packaged service, whose directory is created ahead of it,
but the NuGet package has no such install step. The host was relying on that
directory happening to be created first by the request-side cache, so
whichever of the two ran first decided whether the worker survived startup:
the worker came up on one run and exited on the next, and when it exited,
optimization stayed off and the worker's local API never answered. The
directory is now created before the worker is started, owner-only, at
whatever location `Cache.VolumePath` selects; a directory that already
exists is left exactly as it is. No configuration change is required.

Fixed: image optimization for a lossy WebP source on a desktop-class request
no longer decodes the source image twice when only the original-format
variant is served. The optimization pass now reuses the decode it already
performed, halving the decode work on that path; optimized output is
unchanged. (#1407)

Fixed: a URL whose optimized variants were evicted from the cache while the
optimizer still remembered processing them is no longer skipped forever.
Previously, every renotification for such a URL was dropped as "already
processed" without checking that anything servable remained, so the front
end kept falling through to the unoptimized original with no path back.
The optimizer now validates a dedup hit against the cache and, when
nothing servable remains for the URL, forgets the stale entry and
reoptimizes; these recoveries are visible in a new `dedup_healed`
counter on the stats endpoints. A family with nothing servable is not
always a lost one, though: several paths record a terminal decision
while writing nothing servable (a deterministic CSS or JS minify
failure, an integrity-pinned URL, an oversized image), and a decline
tombstone is written when ANY slot declines, so its presence alone does
not prove a family is empty by decision. The reoptimization is therefore
bounded to at most once per URL per ten seconds rather than forbidden
outright: a changed source is still picked up, while a terminal decision
cannot turn into a per-request re-optimization loop. Deferred heals are
visible in a new `dedup_heal_rate_limited` counter on the stats
endpoints.

Fixed: an origin-content refresh can no longer rebuild a URL's optimized
variants from superseded bytes. When the refreshed origin had not (yet)
been recorded at the default cache slot, the refresh handling could keep
the optimizer's own previous output there and reuse it as the source for
the rebuilt variants, republishing stale content as freshly optimized.
The refresh now keeps that slot only when it holds a genuine origin
recording, and otherwise defers the rebuild until the fresh content
arrives; these deferrals are visible in a new
`origin_refresh_rebuild_refused` counter on the stats endpoints.

Added: a Debian package for the standalone optimizer daemon
(pagespeed-optimizer). tools/packaging/build-optimizer-deb.sh wraps a built
worker binary into a versioned .deb carrying a hardened systemd unit, an
/etc/default/pagespeed-optimizer conffile for overrides, and maintainer
scripts that enable and start the service on install, stop it on removal,
and remove its state directory only on purge. The package also ships the
C API client library (libpagespeed.so) in the multiarch libdir with an
ldconfig refresh, so a serving module can bind the daemon by soname out of
the box, and the unit's cache path is an extensionless stem the serving
module can actually locate (the daemon derives the physical volume name by
inserting a generation/geometry suffix before any extension) — without
either, module integrations silently fell through to serving unoptimized.
The package depends only on glibc (floor derived as the max
across both shipped ELF objects); the C++ runtime is linked statically by
the build's release toolchain. An RPM sibling
(tools/packaging/build-optimizer-rpm.sh) packages the same payload for
EL hosts with the same derived floor, version tripwire, and self-test
discipline. (#1392, #1399)

Fixed: building the worker from source with a host GCC toolchain now works.
The switch-fallthrough annotation degraded to a no-op outside clang (GCC then
correctly refused the unannotated fallthrough); two locals crossing setjmp in
the PNG/JPEG conversion paths were not volatile, so their values after a
libpng/libjpeg error longjmp were formally indeterminate; and three
const-qualified scalar casts tripped -Werror=ignored-qualifiers. All are
fixed for both toolchains; release binaries were unaffected. (#1388)

Fixed: on resized viewports, the original-format image slot for a lossy
WebP source is now re-encoded at the viewport dimensions using the
request's own quality settings (Save-Data reductions included), instead
of a second full-size re-encode of the origin under the resized variant
id -- and that re-encode is bound to the same quality-verification
verdict as every other path: a candidate that fails verification is
declined and the original bytes are served. PNG keeps its full-size
stream optimization (re-encoding would un-palette palettized sources)
and GIF is carried verbatim; lossless WebP sources remain untouched.
(#1380)

Fixed: a failure inside the quality-verification metric itself is no longer
recorded as a catastrophic quality measurement. Metric errors are now reported
on their own channel, so an affected variant is declined as unmeasurable on
every format arm instead of shipping without a verdict, and a decline no
longer cites a score that was never produced. (#1382)

Fixed: the decoded-size limit for monochrome AVIF images now accounts for the
full-size color conversion performed during decoding, so an image sized just
under the limit no longer transiently allocates about three times that limit.
(#1382)

Fixed: an image variant refused by quality verification was also counted as
skipped-for-no-savings when its original-format slot stayed empty, overstating
both counters; the refusal is now counted only once. (#1382)

Added: variants refused by quality verification are now remembered per source
image, so repeated cache notifications for an image no longer re-run the full
optimization attempt ladder for a variant that was already refused and never
stored. The skip is visible in a new `ssimulacra2_decline_tombstone_hits`
counter, and a changed source image retries its variants immediately. (#1382)

Fixed: re-optimizing a lossy WebP image in its own format is now bound to
the same quality-verification verdict as the format conversions. This
path previously accepted its result on the size test alone, so a
re-encode that failed verification -- including catastrophically low
scores -- could replace the original; it is now declined and the original
bytes are served instead. Lossless WebP sources remain untouched, and
JPEG's accept-at-the-cap behavior is unchanged. Update recommended where
quality verification is enabled. (#1385)

Fixed: image decoding no longer terminates the process on input it
cannot decode. A truncated PNG — or any undecodable bytes offered to a
format's decoder — now fails cleanly and is rejected instead of crashing
the worker. Update recommended: any deployment that serves images from
untrusted sources was exposed to process termination on malformed input.
(#1371)

Fixed: a stored image variant is now bound to its quality-verification
verdict. When the optimizer's SSIMULACRA2 check measures a WebP or AVIF
candidate below the configured acceptance floor — including
catastrophically low scores, which are legitimate verdicts — the variant
is declined instead of stored, and serving falls back to the remaining
variants and the original. A candidate whose quality cannot be measured
at all (a failed decode-back, or a pixel layout not comparable with the
source) is declined on every format arm rather than shipped unverified —
previously this fired for every monochrome image. Among competing encode
attempts, the one closest to the acceptance band is the one stored, and
the shipped attempt's encoder setting is what gets recorded. Declines
are counted in a new `ssimulacra2_declines` counter — a decision, not an
error. (#1381)

Fixed: an image whose pixel decode fails — some GIFs, for example — is no
longer stored only under a format-specific variant it was never converted to.
Its bytes now go to the original-format variant, which every client can select,
so such an image is served from cache instead of falling through to the origin's
copy for every client that does not advertise that format.

A counter, `image_unconverted_fallthrough`, now records each such refusal —
"there was nothing to convert" was previously indistinguishable from "the
optimizer ran and could not win", which is why this went unnoticed. (#1374)

Fixed: lossy WebP images are optimized rather than passed through. A WebP
origin encoded above the quality this pipeline targets was previously served as
it arrived, on the assumption that a WebP is already optimal; it is not, and
such an image now re-encodes. The result is kept only when it is strictly
smaller than the origin — the same rule JPEG and PNG already get — so an image
the encoder cannot beat is still served untouched, and a *lossless* (VP8L)
origin keeps the old passthrough, because every WebP encoder here is lossy and
a size gate cannot see a fidelity change. Lossy images that merely carry an
alpha channel are optimized like any other.

Reachability, stated precisely: the optimized bytes are stored at the variant a
WebP-negotiating client selects, so that is who receives them. A client whose
`Accept` names AVIF negotiates to the AVIF variant, finds none for a WebP
origin, and is served the origin's own bytes rather than the smaller WebP
sibling it also accepts — the selection inversion tracked in #1376, which is
where the remaining gap lives. (#1375)

Fixed: a request that sends no `Accept` header is now classified as `*/*`,
per RFC 9110 §12.5.1. It was the only request shape that resolved differently
from the wildcard it stands for, and the effect was that such a client was
served the original of every resource whose optimized variants all sit at
converted formats, instead of an optimized one. (#1377)

Fixed: a stylesheet with an invalid value in a complete longhand family no
longer loses the whole family through minification. CSS drops one declaration
for an invalid longhand but every declaration the shorthand would have set for
an invalid shorthand, so collapsing `padding-top/right/bottom/left` around a
typo — `padding-right:red`, a mistyped unit, a malformed `calc()`, a colour
that is not one, an unknown `overflow` keyword — turned one ignored declaration
into the loss of all of them. The collapse now checks each member value against
what that longhand accepts and declines the collapse otherwise, so the typo
costs exactly what it costs a browser. The check is per longhand, so a negative
`padding` or `border-width` — invalid, and previously collapsed — is declined
too, while a negative `margin` still collapses.

This is not free on well-formed CSS: a family the check cannot positively
recognise is left as longhands, which costs a few bytes. The complete list of
value shapes that no longer collapse is: math and environment functions
(`calc()`, `min()`, `max()`, `clamp()`, `env()`); `url()` values; colour
functions other than `rgb()`/`rgba()`/`hsl()`/`hsla()`, such as `color-mix()`
and `oklch()`; the slash-alpha (`rgb(0 0 0 / 50%)`) and `<angle>`-hue
(`hsl(120deg …)`) colour forms; the modern space-separated colour form
(`rgb(255 0 0)`); and numbers in scientific notation. Plain
`rgb()`/`rgba()`/`hsl()`/`hsla()` colours — with a well-formed comma-separated
argument list — and container-query units (`cqw`, `cqi`, …) do collapse. (#1378)

Workbench: the operator surface now names and explains the three
notify-rejection counters added by PR #1314 — `rejected_version`
(version skew between components), `rejected_malformed` (unparseable
notifications), and `rejected_sentinel` (reserved sentinel name). The
metrics page shows human-readable descriptions for all six notification
counters.

Fixed: ASP.NET Core middleware read-back now honors the true origin-revalidation
signal via `ReadResult.OriginCcFlags` (PS_CC_ORIGIN_* bitfield). The middleware
write path persists origin Cache-Control state using `ps_parse_cache_control`, so
cache entries carry the correct revalidation requirement and aggressive-mode
`stale-if-error` is properly suppressed when the origin requires revalidation.
For a private cache (in-process middleware), only `no-cache` and `must-revalidate`
trigger revalidation; `proxy-revalidate` and `s-maxage` are ignored per RFC 9111
§5.2.2.9-10. (#772)

Fixed: `ps_cache_stats` now fills only the caller-declared struct prefix and
reports the bytes written in `out->struct_size`. An out struct whose
`struct_size` is unset or smaller than the size of the first field is rejected
with `PS_ERR_INVALID_ARG`. Previously the call cleared and filled the
library's own size of the struct regardless of the caller's — harmless while
caller and library share the same header, but an overrun for a caller built
against a smaller, older definition of the struct once the library's grows.
(#1304)

Changed: the nginx serve path now re-validates a cache HIT's selected image
variant against the request's negotiated capability mask immediately before
serving, as a second, independent layer behind variant selection. Selection
already refuses a format-incompatible variant (the WebP/AVIF fix above), so
this check changes nothing a correctly-functioning selector hands down: a
stored Original remains the universal fallback, stored SVG remains universal,
and an exact format match serves exactly as before. Should a selector
regression or a differently-scored peer ever hand down a WebP or AVIF variant
the request's mask does not name, the variant is now refused at the seam —
the request falls through to re-optimization exactly as on a cache miss, and
a WARN log names the URL with the stored and request masks. No counter was
added: the shared stats file's layout is version-pinned and its reserved
anomaly counters belong to a different writer, and this event should never
occur.

Added: the C API publishes `PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE`, a
stored-entry flag a front end sets while recording a response whose headers
include something no later serve can reproduce. Until now that knowledge was
lost at the moment it was learned: the question can only be answered with the
origin's complete header set in hand, and on a cache hit those headers are
gone. A serve path was therefore left with two options, and no way to tell
which one it was in — answer from the entry and drop headers the origin sent,
silently, or give up optimizing the whole class.

With the flag on the entry it has a third: serve optimized where the headers
are reproducible, which is the large majority of responses, and fall through to
the plain response where they are not. Falling through is visible and
measurable; dropping a header is neither. `ps_headers_sidecar_classify` already
answers the question a writer sets the flag from, so no new classification is
involved.

The flag is advisory. It records what the origin sent and gates nothing: a
marked response is stored, scored, optimized and expired exactly as an unmarked
one is, and a front end that does not set it behaves exactly as before. It sits
on the entry the writer stamped — the optimizer's own variants are written with
their own flags and do not carry it — so a serve path reads it back from that
entry by id.

One effect reaches the wire, for a front end that serves marked entries: the
weak `ETag` of a hit covers the flag, so a marked entry does not revalidate
against an unmarked one, which is correct because the two are not served the
same way. Nothing in the shipped engine serves marked entries today, so no
existing deployment sees it. API version 1.7 → 1.8; additions only, so existing
callers and binaries are unaffected.

Fixed: a request whose `Accept` header advertised no image format at all could
be answered with a WebP or AVIF variant it never said it could decode. Variant
selection hard-disqualified a transfer-encoding mismatch but treated an
image-format mismatch as merely a low score — and a family holding only WebP
and AVIF variants (exactly what a resource stored as a durable original
produces, since the stored original itself is never selectable) had no
format-compatible member to outscore them. Worse, the WebP and AVIF entries
tied at exactly the same score, so which undecodable format the client
received depended on cache listing order.

Format mismatches are now disqualified the same way encoding mismatches are: a
stored WebP or AVIF variant is served only to a request whose negotiated mask
names that format, with the original format remaining the universal fallback
and SVG — which the optimizer only ever stores for content it produced itself
— unchanged. When no variant in the family is compatible, selection returns
nothing and the request falls through to re-optimization, the same as a cache
miss. The disqualify is deliberately strict in one direction: a client that
sent `image/*` or `*/*` and could in fact decode a stored AVIF may now miss
the cache where it previously hit, because the negotiated mask cannot prove
that acceptance — a re-optimization is the safe cost, undecodable bytes were
not. Equal-scoring alternates also no longer resolve by listing order: ties
break toward the system's own format preference (AVIF over WebP over
Original, the order `Accept` negotiation itself uses), then toward the
numerically larger variant id, so the same family answers every listing order
the same way.

Fixed: a front end that stores a response as a durable original — the storage
class holding the bytes the origin sent — and then notifies the optimizer now
gets that resource optimized. Until now it got nothing, and got it silently: the
notification was accepted, the optimizer looked for something to work on, found
nothing it was allowed to use, and returned. Nothing errored and nothing was
logged that distinguished it from a resource with no cache entry at all, so what
an operator saw was a cache filling up and an optimizer that never produced
anything.

The cause was the interaction of two behaviours that are each correct. Stored
originals are never returned by ordinary cache selection — that is the guarantee
that keeps a stored copy of the origin's bytes from ever being served in place
of an optimized variant — and the optimizer was looking for its input through
that same selection. It now asks for the stored original deliberately, by name,
which is how the class was always meant to be read, and only after ordinary
selection has found nothing usable. Anything already being optimized is
optimized from the same input as before; nothing that was preferred as an input
is preferred any less.

The guarantee itself is unchanged and unchanged in scope: no request, of any
shape, is answered with a stored original by ordinary selection, before or after
the optimizer has used one as input. Nor does using one as input change how long
its bytes may be used: an optimized variant built from a stored original carries
that original's origin caching state verbatim, so it goes stale exactly when the
original would have. The existing refusal to optimize a resource whose origin
negotiates on `Accept` also still applies, and now reads that marker off a
stored original as well, so the refusal cannot be escaped by storing one.

What this covers is the resource the notification names, and the sub-resources
optimizing a page pulls in — a page's external stylesheets, `@import` targets,
and the images it measures — are NOT covered: those still resolve through
ordinary selection only. On a front end that stores nothing but durable
originals, a page's external stylesheet therefore reads as absent until that
stylesheet is itself recorded and notified, and until then the page is
re-optimized on its cooldown rather than settling. It does settle once each
sub-resource has been through the optimizer, because the optimizer's own output
is an ordinary cache entry. Widening the remaining reads is a separate change.

The statistics surface gains `source_reads_from_durable_original`, which counts
the inputs that came from the stored original rather than from ordinary
selection. It reads zero on a deployment whose front end stores ordinary cache
entries, and tracks the notification rate on one that stores durable originals —
which is the distinction the logs could not previously make.

Fixed: re-recording a sentinel entry — the content hash rewritten when a page's
origin bytes change, the `/llms.txt` pair refreshed on its TTL, the agent
markdown variant re-rendered on refresh — no longer leaves the superseded copy
linked under the same id. Every such refresh used to add one node to the URL's
alternate chain while reads kept finding the newest, so nothing looked wrong
until the chain reached the storage layer's traversal ceiling — at which point
the whole key stopped accepting alternate writes and the URL silently lost every
cache class, not just the one that grew. A re-record now unlinks the previous
entry before writing its replacement, so a single writer per URL keeps one node;
concurrent writers for the same URL and id can still accumulate, bounded by the
chain ceiling and made visible in the log, until a later storage version makes
replacement unconditional. One consequence of replacing rather than adding: the
previous entry is removed when the write begins and the new one exists when it
completes, so a write abandoned in between leaves the URL without that entry
until the next store — which reads as an ordinary miss, never as wrong bytes.

Added: the C API publishes `PS_FLAG_ORIGIN_VARIES_ACCEPT`, the stored-entry flag
that records "the origin negotiates on `Accept` itself", together with
`ps_vary_varies_accept` — the predicate that answers whether a response's `Vary`
calls for it. A writer storing a durable original MUST set the flag when that
predicate says so. The optimizer refuses to derive variants from an entry
carrying it, and refusing is the only correct outcome: the stored copy is
whichever representation the first requester's `Accept` elicited, so a variant
family derived from it would be served to clients that asked for something else.
The flag is the entire record of that decision, because on a cache hit the
origin's own `Vary` is gone.

This closes a gap rather than adding an option. The obligation existed and the
optimizer already honoured the flag, but there was no published constant to set
and no exported way to compute it, so a writer using only documented API could
not comply — and the omission is silent, which is why it is called out here
rather than left to the reference. `ps_vary_varies_accept` is the second half of
the same verdict `ps_vary_uncacheable` already answered; both classify one value
once, so they cannot disagree. API version 1.6 → 1.7; additions only, so
existing callers and binaries are unaffected.

Added: the optimizer now publishes the size it opened its cache volume with, so
that another process opening the same volume can inherit that number instead of
assuming one. The volume's on-disk filename is derived from its geometry, which
means a second process opening the same directory with a different size does not
collide with the optimizer and does not fail — it quietly creates and uses a
different file, shares nothing, and runs a permanently cold cache, with no error
on either side. The only way to be sure two processes are looking at the same
cache is for the second one to be told the number rather than guess it.

The size appears in the shared configuration file the optimizer already writes,
and the C API exposes it as `ps_read_shared_config_volume_size`. A caller that
gets 0 back is being told the size is not known — no configuration file, an
unreadable one, or one written by an older optimizer — and the documented
answer to 0 is to decline to open the volume, not to substitute a default,
because substituting a default is exactly what produces the second cold cache.
Nothing existing reads the new value: the bundled front end resolves the file a
different way and is unaffected.

The shared configuration's schema version deliberately did not move. Its version
gate is a reader-side "I cannot read this at all" — a reader seeing a higher
version discards the entire file and falls back to built-in defaults for every
setting in it — and unknown keys are ignored by design, so every already
installed reader keeps working unchanged. Raising the version to announce an
optional key would have degraded all of them in exchange for announcing
something none of them consult.

The C API also gains `ps_cache_config_init_sized`, which fills the cache
configuration structure while writing no byte past the size the caller states it
owns. The existing `ps_cache_config_init` is not size-aware — it writes the
structure as *this* library defines it, which overruns a caller compiled against
an older, shorter definition before that caller can check anything. Callers that
may meet a newer library should prefer the sized form; `ps_cache_config_init` is
unchanged and keeps working for callers whose definition matches.


Added: the notification protocol and the C API now carry the options a request
resolved to. A module in front of the optimizer resolves its own configuration
per request — globals, per-host, per-directory, per-request overrides — and
until now none of that reached here at all.

A notification may now carry that resolved configuration as a value, together
with a signature over it. The value is carried, never interpreted: nothing in
the optimizer merges configuration, inherits from it, or reads policy out of it.
The signature is checked byte for byte against the value it arrived with, on
every notification that carries one. That check is what it is for: a sender and
this optimizer that have drifted apart in how they render a configuration find
out on the first notification rather than never.

What the optimizer then does with an accepted context is worth stating plainly,
because it is less than the field's name suggests. The work is done and stored
under the default context, whatever context named it. That is not a shortcut. On
this surface optimized output is a function of the resource and of the request —
the client's capabilities, the content type, whether the request was an entitled
agent request — plus this optimizer's own configuration, which a notification
cannot reach. No value a module resolves per request reaches a rewriter here, so
two requests that resolved to two different configurations cannot produce two
different optimized artifacts, and one stored artifact is the correct answer for
both of them. Splitting the cache by a name that separates nothing would turn
one warm cache into several cold ones and buy nothing. Requests that supply no
options context, which is every deployment shipping today, are unaffected in
every respect and key byte for byte as they always have.

An options context that arrives from a format this release does not read, with a
malformed signature, or with a signature that does not match its own contents is
refused: the notification is dropped before anything is read or written, the
resource it named is served without optimization that time round, and the
refusal is counted in `rejected_option_context` and reported with a rate-limited
log line naming which of those it was. An oversized payload is refused a layer
earlier, as a frame this release cannot accept, so it is counted in
`rejected_malformed` rather than with the three above — and the sending side
refuses to put one on the wire in the first place.

The statistics and metrics endpoints carry the other half too:
`accepted_non_default_option_context` counts notifications accepted while naming
a configuration other than the default one, which is how a deployment sees that
its front end is resolving and sending contexts at all, and where that work is
landing.

The notification protocol version moved with the field. Version skew between a
module and this optimizer therefore behaves the way it always has: a
notification from a peer speaking another version is refused whole and counted,
the resource it named is served without optimization that time round, and
nothing is written. That is deliberately the outcome rather than reading the
part of the message that looks familiar, which would mean acting on fields whose
meaning is guesswork.

The C API gains `ps_notify_worker_ex`, which takes every parameter
`ps_notify_worker` takes plus the agent-request bit and the options context, and
`ps_option_context_signature` for computing a signature over a canonical
payload. `ps_notify_worker` is unchanged and keeps working. Exporting the
signature function is deliberate: every consumer that declares an options
context has to produce byte-identical signatures for byte-identical payloads
indefinitely, and a consumer that reimplements it and drifts by one byte has
every notification it sends refused.

Fixed: a resource whose origin negotiates on `Accept` is now cached and served
faithfully, and is excluded from optimization. An origin that answers a
request with `Vary: Accept` is telling every cache in front of it that IT
chooses the representation from what the client asked for. Until now that
declaration was accepted for storage and then ignored twice over.

It was ignored on the way in. The response was cached and also queued for
optimization, and the optimizer worked from whichever representation the
FIRST visitor's `Accept` happened to elicit from the origin. Everything
derived from it was therefore derived from one arbitrary visitor's
negotiation result, and later visitors — including ones whose `Accept` would
have asked the origin for something else — were served from that capture.
Such a response is now stored exactly as the origin sent it and is never
handed to the optimizer, the same treatment a response marked `no-transform`
already received. The entry records the reason, so a later request can tell a
resource that was deliberately left alone from one that has not been optimized
yet. Note the scope: this affects only resources the origin itself declares as
Accept-negotiated. Ordinary resources are optimized exactly as before, and
image format selection on our own side is unchanged.

It was ignored on the way out. A response served from cache is rebuilt from
the entry, at which point the origin's headers are long gone — so an origin
`Vary: Accept` on a stylesheet or a script was dropped from the response
entirely, while the same response passing straight through carried it. A
shared cache or CDN sitting between the install and its visitors then stored
that response keyed without `Accept`, and could hand one visitor's
representation to a visitor who asked for something else. Responses served
from cache now carry `Accept` in `Vary` whenever the entry records that the
origin negotiated on it. This is deliberately not applied across the board:
declaring `Accept` on every stylesheet and script would multiply the number of
copies every downstream cache keeps of resources that do not vary at all, so
it is declared only where the origin actually asked for it.

Fixed, separately and with wider reach than the above: an expired resource is
now re-cached after it is refreshed. The serving stage attached its own `Vary`
to the response before it was known whether the response would be served from
cache or fetched from the origin. On an expired resource it was fetched, and
the header went out with the request and came back attached to the origin's
reply, where the cache read it as though the origin had sent it. For images
that value names a field the cache deliberately refuses to store on, so the
refreshed image was judged uncacheable: it was fetched from the origin again
on the next request, and again on the one after, and was never re-optimized.

This reached images whose origin does not support conditional requests, or
whose content had changed — that is, any refresh answered with a full
response. Images from an origin that answers `304 Not Modified` were never
affected, because that exchange never consults the header in question. The
header is now attached only to responses actually served from cache.

Operators should expect a one-time revalidation for affected resources.
Entries recorded as Accept-negotiated carry a different entity tag from ones
that are not, because the two are served with different downstream cache
keying and are not interchangeable. A resource whose origin starts or stops
sending `Vary: Accept` therefore costs each visitor one full response, once,
after which conditional requests resume as normal.

A resource that was already cached and optimized before its origin began
sending `Vary: Accept` is handled too, and it is the case worth calling out
because it is the one every existing install is in. The copies built while it
was an ordinary resource are discarded rather than left in place — they would
otherwise keep being served in preference to the origin's own representation,
since a compressed copy is a better match for most visitors than an
uncompressed one. Recovery costs one origin fetch and happens the first time
the resource is refreshed after the origin changes.

The reverse direction works too: an origin that stops sending `Vary: Accept`
releases the resource back into optimization the next time it is refreshed.

Unchanged in both directions: a `Vary` this cache cannot key on — `Vary: *`,
or one naming a field such as `Cookie` or `Origin` — is still refused
outright and the response is served straight through without being stored, and
an `Accept` appearing beside such a field does not soften that refusal. Cache
entries written by earlier versions are read exactly as before; the record
this change writes uses space the entry format already reserved, so no cache
needs to be rebuilt.

Changed: version skew between the optimizer daemon and the components that
notify it is now reported instead of being silently absorbed. Notifications
have always carried a protocol version, and one carrying an unexpected version
has always been refused rather than half-read — which is the safe direction,
because acting on a message you cannot read is how a cache gets a wrong entry
written into it. What was missing was any way to tell that this was happening:
a refused notification looked exactly like a site that simply never warms up.
Refusals are now counted by cause, and a refusal caused by a version
disagreement is logged with both versions named, so the fix is readable off the
line. Three new counters appear on the statistics and Prometheus surfaces:
`rejected_version`, `rejected_malformed`, and `rejected_sentinel` (respectively:
a sender speaking another protocol version, a truncated or corrupt message from
a sender that agrees on the version, and a message naming a reserved cache
entry class that is not a valid notification trigger). The log line for a
version disagreement is rate-limited to one per minute per sending version, so
a misconfigured sender cannot flood the error log.

Added: a health check for the shared configuration file that the daemon writes
and the server module reads. If that file declares a schema version the running
build cannot read, the file is not parsed at all and the build falls back to
its compiled-in defaults — the install keeps serving and keeps optimizing, but
it does so on defaults rather than on the settings in the file. Until now that
happened silently outside the server module, which meant an install could run
for a long time in a configuration nobody had chosen. It is now reported the
moment it is observed, with both the file's version and the build's version
named and a statement of what is in effect instead, and `/v1/health` reports
`shared_config_version` as failing (overall status `degraded`) for as long as
the process has seen the mismatch. Operators running a matched set of
components will see no change.

Added: the cache gained a storage class for a response's request-independent
headers. A cache entry has always reproduced the headers the cache itself
needs — `Cache-Control`, `Content-Type`, `Content-Length`, `ETag`,
`Last-Modified`, `Expires` — and anything else the origin sent was not
reproducible from a stored entry, so serving such a response optimized would
have meant serving it with a header removed. Those responses were therefore
left alone. This class is what removes that obstacle: a per-URL block holding
the request-independent headers verbatim, exactly as the origin sent them, so
that a response can later be served optimized with its headers intact.

As with the durable-original class above, the storage and the API to use it
land first: nothing in the engine writes or serves these blocks yet, and no
serving behaviour changes in this release. What ships is the class, its
admission rules, and the C API an embedder uses to store one — so that a cache
written now is one a later version can serve from.

The headers kept are `Content-Security-Policy` (see below),
`Access-Control-Allow-Origin`, `X-Content-Type-Options`, `Referrer-Policy`,
`Cross-Origin-Opener-Policy`, `Cross-Origin-Embedder-Policy`,
`Cross-Origin-Resource-Policy`, `Permissions-Policy`, and the origin's own
`Vary` string. The list is closed and is not configurable, deliberately: a
header whose value depends on who is asking must never be replayed to the next
client, and that is not a per-deployment judgement. A response carrying
anything outside the list is still served plain — correctly, just not
optimized in place — and a count of those is kept in-process so the size of
that class can be observed.

A `Content-Security-Policy` carrying a nonce is never stored, and a resource
whose CSP carries one is never optimized in place. A nonce is per-response by
construction: replaying it to the next visitor would defeat the policy it is
part of. The detection is deliberately broad — a policy that merely mentions
`nonce-` anywhere is treated as carrying one — because the cost of being wrong
in that direction is one resource served as it always was, and the cost of
being wrong in the other is a security policy quietly disarmed.

Stored blocks are never served by ordinary cache selection: a block of headers
is not a representation of a resource and no request is ever answered with one.
A read always returns the most recent block. Where a single writer stores for a
URL, a later response replaces the block rather than adding to it; where
several writers store for the same URL concurrently, superseded blocks can
accumulate until a later version of the storage layer makes replacement
unconditional. The accumulation is bounded and logged rather than silent.

For embedders: `ps_cache_write_headers_sidecar` (PS_API 1.4) offers a
response's complete header set for storage, and `ps_headers_sidecar_classify`
answers the same admission question with no cache involved. You pass the whole
header set, not a curated one — the call is the gate, which is also why
`ps_cache_write_sentinel` refuses this entry's id. A refused response is not an
error: the call succeeds, reports what it decided, and stores nothing. Read the
header notes before building on it: the same concurrent-purge constraint that
applies to writing originals applies here, replacement is scoped to a single
writer per URL, and a missing block must be read as "this response is not
optimizable" rather than as "optimized, with no headers".

Added: the cache gained a storage class for the ORIGINAL of a resource — the
bytes the origin sent, before any optimization — held as an entry of its own
rather than in the slot an optimized variant may later take. What it buys is
rebuilding: with the original still stored, a changed optimization setting can
produce new variants without fetching the resource from the origin again. The
engine does not populate this class yet; the storage and the API to use it land
first, so that a cache written now is one a later version can read.

How long a stored original may be used is the origin's decision, not a retention
setting: it is governed by the `Cache-Control` and validators the origin sent,
evaluated the same way every other cache entry is. When the origin's response is
refreshed, the stored original is dropped in the same operation as the variants
derived from it, so a rebuild can never mix new variants with superseded bytes.

Stored originals are never served by ordinary cache selection: no request, of
any shape, is answered with the origin's own bytes merely because a copy of them
happens to be stored. They are read deliberately, by the code that wants them.

Re-recording an original replaces it, and a read always returns the most recent
one. That replacement holds for a single writer per URL; several writers storing
for the same URL at once can leave superseded copies behind, bounded by the
storage layer's chain limit but costing volume, since each copy is a whole
response body, and the copies retained are the oldest ones, reclaimed only by
normal volume eviction — so serialise them if you store originals from more
than one process. A later storage version makes replacement unconditional with no change
on your side. Note also that the replacement is not instantaneous: because the
write streams, the previous original is removed when the write starts and the
new one exists when it completes, so a stream that is abandoned, that the
storage layer refuses at close, or that exceeds the size limit leaves the URL
with no stored original. That reads as a miss — a
re-fetch, never wrong bytes.

Capacity planning, worth doing before this class is in use: storing originals
adds roughly one further copy of each recorded resource, and for image-heavy
pages the original is often the largest object in its family. Our sizing
guidance is to plan for about 2.5x your current cache volume for a typical live
set and about 3x for an image-dominated site, with 1.5x to 3.2x covering the
site shapes we have looked at; your own mix is the better guide once the class
is in use. Under-sizing costs no correctness — the volume evicts as it always
has — but it costs hit rate, so raise the volume first.

Responses larger than 16 MB are not kept as originals — above that size the
copy costs more than the rebuild it saves. The limit is a fixed property of the
build: no configuration file or directive changes it. It is enforced while the
response streams rather than after it has been buffered, so an oversized
response never costs the memory the limit exists to avoid. A response over the
limit is simply not stored as an original; nothing else about serving it
changes. There is no operator-facing metric for these skips yet: the C API
reports each one at the call that skipped it, with the limit named in the error
message, and an aggregate count exists in-process for a future stats surface.

For embedders: `ps_cache_write_original` (PS_API 1.3) stores an original through
the C API, taking the same write parameters as an ordinary write, and
`ps_cache_read_alternate` with `PS_SENTINEL_ORIGINAL` reads it back. It is a new
entry point, so nothing existing changes; `ps_cache_write_sentinel` refuses the
original's id, because the class carries entry metadata and a size limit that a
raw sentinel write would skip. Read its header notes before building on it: an
original you write is not fenced against a concurrent purge of the same URL,
which is a constraint a producer has to close rather than assume.

Changed: a cache entry now also records the origin's `Cache-Control` header
verbatim, next to the parsed form it already kept, so that anything which has to
reproduce the origin's own directives has the bytes rather than a
re-synthesis of them. Two further per-entry fields are added and left unused for
now, in preparation for upcoming work.

Entries written by earlier versions are still read, unchanged. Entries written by
this version are NOT readable by an earlier one: a downgraded build treats them
as a miss and re-records, so the cache re-warms rather than a downgraded build
misreading anything. Plan a downgrade as a cold cache, not as a rollback with no
effect. In practice 2.1 also moves the on-disk cache format, so a 2.0 build
reopens its own pre-upgrade volume and never reads an entry 2.1 wrote (see the
cache-format entry at the top of this section); the rule stated here is what
applies wherever an entry-format skew does meet inside one volume.

Note for embedders that set `max_metadata_size` explicitly: its default rises to
878 bytes, derived from the largest metadata blob the format can now produce. A
ceiling below that does not truncate anything — it refuses the write whole, so
affected entries are silently never stored, which in production looks like a
cache that never warms. Two ways to be affected, not one: every entry grows by
10 bytes regardless of what the origin sends, so a pinned ceiling can start
losing entries that were previously just inside it even with no long headers at
all; and separately, an entry can now carry up to 256 bytes of origin
Cache-Control.

Also for embedders: `ps_cache_write_sentinel` now refuses sentinel ids that are
reserved for entry classes which do not exist yet, returning the invalid-argument
error. Ids that name no class remain writable. No shipped id changes behaviour —
every id the API documents as usable stays usable.

Added: the embedding C API can now record and read back an origin's caching
state, and can answer the caching questions itself. A cache write accepts the
origin's `ETag`, `Last-Modified`, `max-age` / `s-maxage` and Cache-Control
directives, and new accessors read them back from a cache hit; three new
entry points evaluate whether an entry is still fresh, assemble the
`Cache-Control` value to send with it, and decide whether a response carrying
a given `Vary` may be cached at all. Previously an embedder had to implement
RFC 9111 a second time to answer those, and two caches disagreeing about
whether the same bytes are fresh is not a difference anyone notices until it
is serving stale content. Nothing existing changed: the write parameters grew
by appending, so code built against the previous version keeps working
unmodified, and an embedder that sets none of the new fields gets exactly the
previous behaviour.

Note for embedders: the freshness and Cache-Control entry points take a
cache-scope field whose default (zero) is SHARED, matching a proxy — the
deployment these are primarily for. An origin-private integration, such as
in-process middleware serving one application, must set it to private, or
`s-maxage` and `proxy-revalidate` will be applied where they should not be.

Note for embedders storing responses that arrive through another cache: the
insertion time is now yours to supply, and it should be the time the origin
generated the response, not the time you stored it. A response that reached
you carrying `Age: N` was already N seconds old, and recording your own clock
instead grants it N extra seconds of apparent freshness — behind a CDN, on
every response. `ps_age_adjusted_insert_time` does the arithmetic; leaving the
field at zero keeps the previous behaviour of stamping the local clock.

Added: the library's C API gains a documented stability contract
(`lib/pagespeed/ABI.md`) and the checks that enforce it. A published
structure's layout is now pinned at compile time, so moving a field fails the
build on every platform; and a separate check fails the change if an entry
point disappears, if the exported set drifts from what the header declares, or
if the version number does not move the way the change requires.

Fixed: four C API entry points — the agent-negotiation helpers, the
agent-aware cache read, and the content-binding accessor — were documented and
built but never actually exported, so any attempt to call them from a
separately compiled program failed to resolve them at load time. They are
exported now.

Added: the shared serve-statistics file gains a serve-class and worker-load
block. The optimizer now records how busy its own work queue is — a running sum,
a sample count and a high-water mark — so a monitoring tool can report average
and peak backlog over any interval instead of guessing from a single poll. The
file also reserves counters for how each response was served (optimized, or
original bytes because the optimized copy was absent, still being produced,
declined, or a version mismatch was detected), plus a count of records the
optimizer could not interpret, so a gap in those figures is visible rather than
silent; they stay at zero until a front-end that classifies serves ships. Nothing changes about how requests are
served or queued: this block is observation only. Two library entry points are
added for reading and writing it, and no existing entry point changed.

Note for operators: this bumps the serve-statistics file version, which resets
its counters once on upgrade, exactly as previous versions of that file have.
The optimizer recreates the file automatically and now logs a warning naming
both versions when it does, so a front end left on the older version — which
stops recording until it is upgraded — is visible in the log instead of only as
flat counters.

Fixed: re-optimizing a JPEG could encode it at a higher quality than the
original was saved at, producing a larger file with no visible improvement —
detail the source already discarded cannot be restored by spending more bytes
on it. A JPEG re-encode is now capped at the quality detected in the source
image, and the perceptual quality check no longer raises quality past that cap.
Sources saved at a high quality, and conversions to WebP or AVIF, are
unaffected. `--no-quality-cap` turns the cap off and now takes effect;
`--quality-cap-margin` is still accepted but no longer raises the cap.

Fixed: AVIF image candidates were shipped without the verify-and-re-encode
quality check that guards the other formats — the quality verifier could not
decode its own AVIF output, so a badly degraded AVIF encode would ship instead
of being caught and re-encoded at higher quality. AVIF candidates are now
decoded and verified like the other formats, and a verification that cannot
run now logs a loud warning instead of passing silently. Enabling the AV1
decode path also makes origin-served AVIF decodable and transcodable for the
first time (previously declined by construction); decode is capped at 50 MB of
pixels and runs in the isolated worker process, not the webserver.

For embedders: PS_API 1.9 adds size-aware entry points `ps_html_config_init_sized`
and `ps_critical_css_config_init_sized`, completing the set alongside
`ps_cache_config_init_sized`. The three legacy initializers
`ps_cache_config_init`, `ps_html_config_init` and `ps_critical_css_config_init`
now write only their PS_API 1.8 prefix (48, 128, and 88 bytes respectively),
pinned so future struct growth forces a developer decision. Callers compiled
against older headers must use the sized entry points or pass a buffer large
enough for the current library's layout.
