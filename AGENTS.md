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
