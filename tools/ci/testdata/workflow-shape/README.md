# Workflow-shape gate fixtures

Fixtures for `tools/ci/test_check_workflow_shape.sh`. Each subdirectory is a
tree root passed to `tools/ci/check-workflow-shape.sh`:

- `good/` must pass (exit 0). It exercises a thin caller with an aggregate
  gate job, a `ci-dev` caller, a reusable workflow with every
  routing-expression variant, the three fleet guards, a hosted report job with a write permission, and the allowlist path (`legacy.yml` is listed
  in `good/tools/ci/workflow-shape-allowlist.txt`).
- `bad-no-top-perms/` (a reusable workflow without a top-level `permissions:`
  block), `bad-write-quoted/` and `bad-write-single-quoted/` (a quoted `write`
  on a fleet job) and `bad-write-flow/` (the one-line flow form) cover the
  gaps of rule f.
- `good/` also has jobs on the switch routing expression (no guard, or the
  path filter alone) and the `!= 'false'` form of the fleet-only guard;
  `bad-switch-guard/` (a guard on a switch-routed job), `bad-switch-drift/`,
  `bad-switch-iis-class/` (the switch expression on a class with no hosted
  twin), `bad-switch-path-drift/` and `bad-guard-only-switch-drift/` (the
  switch form without the fork term) break them.
- `bad-*/` must each fail (exit 1) and name the rule in the message.

The bad trees violate the gate on purpose; the token scan excludes this
directory (see the exclusion list in `check-workflow-shape.sh`).
- `good-queue-guard/` is the accepted merge-queue guard layout of `pipeline-pr.yml`: the guard step right after the checkout step, the fixed keys, `if:`, env and `run:` block, and `vendor` with `needs: changes` and no `if:`. Each `bad-queue-guard-*` tree breaks one rule of the `queue-guard-reads-default-branch` check (the queued tree's copy, an extra or trailing command, an interpolated, truncated, globbed, braced, `find`-ed or sourced read of the script, a `GITHUB_ENV` write, an absent, renamed or misplaced step, a neutering or quoted key, a changed `if:`, a step not right after the checkout, env on the checkout, a job-level or extra step env key, `PATH`/`BASH_ENV`/`GIT_CONFIG*`, and a `vendor` job without `needs: changes`, with an `if:`, or missing). `good/` carries the guard step in its `changes` job too.
