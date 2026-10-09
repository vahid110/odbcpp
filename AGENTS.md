# ODBCPP development guidance

For implementation, qualification and integration batches, use the project skill
at [.agents/skills/odbcpp-development-batch/SKILL.md](.agents/skills/odbcpp-development-batch/SKILL.md).
It defines the repeatable workflow; this file defines durable repository rules.

## Architecture and ownership

- Follow `SDK_ARCHITECTURE.md`, `SECURITY_MODEL.md`, `SDK_PRODUCT_PLAN.md` and the
  relevant backend/authentication plan. Concrete backend framing and native types
  stay backend-owned; ODBC adapts normalized, owning SDK contracts.
- One integration owner controls shared SDK/CMake/CI/crypto/ODBC files, commits
  and pushes. Delegate finite nonoverlapping packages with an exact baseline,
  owned files, checks, deliverable and stopping condition.
- Inspect actual agent state. A delivered package is not continuing work. Check
  SDK/core, Redshift catalog/types, MySQL and authentication at continuation
  checkpoints; distinguish active work from a named prerequisite or wait.
- Before ending a development turn, reconcile every authorized worker's actual
  state with the handoff. For a completed worker, dispatch the next ready bounded
  package in that turn, or record why none is ready and the next action that can
  unblock it. Do not leave an idle worker represented as active development.
- Keep the current API worker's assignment, completion condition and next queued
  product task in the handoff. Select the successor while the current package is
  being reviewed or tested. Packaging, diagnosis and source selection must be
  labeled as such; they do not count as API implementation. If selection finds
  no defect, root must choose the next applicable planned task or explicitly
  record an idle lane; do not manufacture tests or silently await another timer.
- Keep the authorized Redshift API/feature lane and qualification lane separate
  and concurrent when ready. Reserve the API worker for selecting and implementing
  product work; do not reassign it to packaging, qualification, CI watching or
  reviews. Root and existing reviewers own those tasks. Qualification continues
  with required safeguards, but a qualification wait does not silently idle or
  replace ready feature work. Record each lane's actual state independently.
- Prefer attached managed Git worktrees for independent implementation. Reuse a
  suitable checkout only after checking its baseline and local changes. Preserve
  historical dirty worktrees; do not reset them to make room for a new task.
  Read-only reviews need no additional checkout. Existing immutable scratch
  candidates need not move during an active qualification run.

## Validation and evidence

- Feature and live assertions belong in checked-in GoogleTests. Python supplies
  orchestration and offline safety tests, not substitute feature assertions.
- Configure the actual product with `TARGET_DATABASE=POSTGRESQL` or `REDSHIFT`;
  current CMake rejects a MySQL product build. Inspect real options and target
  inventories rather than assuming every historical preset is valid.
- Use distinct owned build directories. Run relevant focused checks during
  development, then required shared gates once per coherent source batch.
  Inspect `.github/workflows/` and `cmake/Tests.cmake` for the current graph.
- Preserve original failures. Separate source inspection, compile/link,
  synthetic runtime, native local proof and platform CI. Bind results to actual
  source, executable, configuration and inventory; never inherit qualification
  from an older binary or another candidate.
- Reap gate processes and independently verify private PostgreSQL shutdown
  before final acceptance. Integrate reviewed exact bytes, then follow one
  exact-head CI run to completion. No status-only pushes or duplicate watchers.

## Continuity and authority

On the authorized development host, read
`~/.local/state/odbcpp/continuation/current-handoff.txt` for changing tasks,
evidence and protected admission constraints. Replace current facts there;
archive superseded evidence without copying full history into timer prompts.
The handoff and a goal do not create new cloud authority.

Keep the existing 15-minute continuation and separate cost monitor active until
an explicit stop or their actual completion condition. No per-agent timers.
Notify on meaningful completion, failure, material blocker or required action;
otherwise keep scheduled checks quiet.

Paid/cloud work requires the protected operator handoff and separately reviewed
finite admission. Preserve the cumulative allowance and old uncertain exposure;
never infer budget, credentials, SQL coverage or a horizon extension from a
timer, synthetic pass, goal or billing estimate. Actual AWS SDK acquisition and
verified-TLS Redshift login remain separate completion requirements.

## Batch size and diagnostic execution — human agreed 2026-10-07

Default to coherent workflow batches, normally three to five related scenarios
when ready and compatible, rather than a separate integration cycle per case.
Run focused checks while developing, then the required shared regression graph,
independent integration review, push and exact-head CI once per coherent batch.
Preserve each scenario's assertions and result; never add unrelated work merely
to fill a batch or replay already qualified milestones.

CI and real Redshift tests provide distinct evidence. A bounded diagnostic run
may precede final CI when its exact candidate source, actual binary, relevant
local checks and finite cloud controls have received independent review. Record
it as diagnostic evidence; final acceptance still requires all applicable local
gates, independent review, exact-head green CI and actual live proof. This does
not bypass an existing launcher that requires CI: any diagnostic-specific control
change must be independently reviewed before use. Finish the current Unicode
alias batch under its already selected controls.

Group ready, reviewed live scenarios into one finite cluster session with explicit
per-case and aggregate statement/time bounds, durable one-use admission and
verified exact-principal cleanup and pause. Keep the cluster paused during coding,
builds, CI and longer gaps; do not keep it running to await speculative readiness.
Spending, resource, principal, network and authorization horizons are unchanged.
