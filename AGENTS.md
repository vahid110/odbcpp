# ODBCPP repository rules

Use [DEVELOPMENT_WORKFLOW.md](DEVELOPMENT_WORKFLOW.md) as the canonical workflow
and the [development skill](.agents/skills/odbcpp-development-batch/SKILL.md) for
bounded implementation or qualification. Earlier policies are
[historical evidence](docs/history/workflow-2026-10-19/README.md), not active instructions.

## Current authority

Product, feature, Auth, roadmap work and the unfinished migration integration
are **PAUSED**. Only the human-authorized workflow replacement is active.
Workflow readiness does not resume product work. No pushes, PRs, cloud/Auth/live
execution, new timers, watchers or billing probes in this batch.
Keep the existing migration candidate, DSN GUI changes, builds, failures and
protected inputs intact. The compact private handoff records current source and
qualification debt; a goal or handoff grants no resource authority.

## Ownership and correctness

- Follow `SDK_ARCHITECTURE.md`, `SECURITY_MODEL.md`, `SDK_PRODUCT_PLAN.md` and
  relevant backend plans. Provider-neutral contracts, reusable transport/security/
  deadline primitives and shared lifecycle belong in `sdk/`; native protocol,
  framing, keys and backend state stay backend-owned; ODBC handles, diagnostics
  and API adaptation stay adapter-owned. Do not move protocol semantics into SDK
  or invent an abstraction solely to satisfy a location rule.
- For changed contracts/classes/helpers, record component, exact owner file,
  reuse rationale, permitted dependencies and proof in the candidate report.
  Explain generic functionality outside SDK; the independent reviewer assesses
  actual placement. Run applicable staging/architecture/first-include checks.
- Preserve valid GoogleTest assertions and original failures. Feature assertions
  belong in checked-in GoogleTests; Python may orchestrate resources and check
  offline controls. Diagnose with driver logs and ODBC diagnostics; distinguish
  product defects from fixture, input and execution-environment defects.
- Every changed control body containing GoogleTest macros has explicit braces,
  including enclosing controls. Keep controls and assertion bodies readable on
  separate lines. Run `tools/development/check_changed_gtest_control_flow.py`
  against the exact batch base before expensive builds when C++ tests change.
- Configure actual products as `POSTGRESQL` or `REDSHIFT`; MySQL product builds
  remain unsupported. Preserve explicit driver-manager/SQLWCHAR/crypto profiles.

## Evidence and resources

- One Windows coordinator owns integration, resource admission and cleanup.
  Isolate new work on a `codex/` branch; inspect existing worktrees first and
  preserve dirty or historical checkouts. Never reset unrelated source.
- Keep source/diff, configuration, toolchain, runtime image identity, inventory,
  actual exit, CTest/JUnit, GoogleTest XML and relevant logs together. Do not
  promote an interrupted command or another source/binary's pass to qualification.
- Every product PR requires actual macOS, Linux and Windows builds. Full required
  platform/profile suites remain checkpoint/release gates. Evidence reuse needs
  explicit source/input/profile applicability; full acceptance debt stays visible.
- Check free space before heavy work and at closure. Target at least 5 GiB free;
  before a full graph allow measured additional peak plus 2 GiB reserve. Reuse one
  current build per configuration. Retain original failures, reference images,
  private PG data and unresolved dependencies. Delete only exact reviewed obsolete
  artifacts after checking readers/references; keep a deletion ledger. No blanket cleanup.
- Private/cloud work retains existing identity, verified TLS, network, finite
  operational admission, one-use scope, runtime/caps, standing fixture authority
  and exact principal/query/input/process cleanup. Human billing is manual; the
  USD100 compute/USD150 total/USD50 other/tax limits and history remain unchanged.
  Keep the cluster PAUSED during gaps and at the end. No new financial or per-case
  admission machinery, automatic cloud resume, or credential values in reports.
