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
