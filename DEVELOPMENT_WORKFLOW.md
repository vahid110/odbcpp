> **Superseded** on 2026-10-30 by [ADR 0001](docs/adr/0001-development-process.md)
> and [AGENTS.md](AGENTS.md). Kept for history. Only its live-test rules stay in
> force until ADR 0001 migration step 2 is complete.

# ODBCPP development workflow

This is the canonical workflow, approved 2026-10-20. It replaces the historical
policies linked from [the archive](docs/history/workflow-2026-10-19/README.md).
[AGENTS.md](AGENTS.md) carries durable architecture, evidence and authority rules.
Current human task authorizations and holds govern execution, as described in
AGENTS.md. Workflow readiness alone never resumes a held task.

## One bounded task

One Windows coordinator takes a task from source inspection through focused
qualification, one substantive independent diff/result review, and integration
when authorized. Use a short task chat and a separate clean `codex/` worktree at
the actual chosen baseline. State the problem, leased files/contracts, completion
condition and required checks before coding. Preserve unrelated changes.
Delegate independent implementation only when it saves work without overlapping
ownership; give the exact baseline, files, allowed actions, deliverable and stop
condition. There are no permanent coder/qualifier/reviewer lanes or supervisor loops.

Implement a coherent change, preserving assertions and initial failures. Run
focused tests while investigating, then the relevant regression once per coherent
batch. Use [the command matrix](tools/development/COMMANDS.md), standard CMake
presets and CTest. Keep source unchanged during its build/test; concurrent edits
need another snapshot. Reuse the build that produced an artifact for its tests.
Windows GUI checks run locally in the actual interactive Windows desktop, using
the checked-in GUI runner; never use SSH, Session0 or scheduled-task experiments.
Privileged System DSN checks still require an approved elevated local token and
exact owned fixture cleanup; do not disable endpoint protection or weaken checks.

The reviewer inspects actual diff, architecture placement, assertions, diagnostic
reasoning, results and remaining debt. One review covers the complete candidate
and evidence; fixes to findings receive a focused follow-up, not a new review
ceremony for each command/category. Source inspection, compile/link, offline
controls, local runtime and real database execution are distinct evidence.

## PR and checkpoint qualification

Every **product PR** retains at least one actual macOS, Linux and Windows build
bound to its source/profile. Reuse those exact artifacts for their tests. Avoid
duplicate push/PR/postmerge builds only when exact-tree applicability and branch
protection permit; do not bypass required checks or infer them from another head.
Full required platform/profile suites, sanitizer/Unicode/provider, architecture,
staging/packaging and live acceptance remain checkpoint/release requirements.
Record unrun gates as debt. Prose-only edits need targeted link/command review,
not a blanket product-suite replay. Control/preset changes need their focused
offline tests and a meaningful command smoke.

When running a full suite, execute the complete applicable inventory through
ordinary assertion failures and isolated crashes before repairing anything.
CTest owns selection, scheduling, timeouts, fixtures and process outcomes.
Do not use stop-on-failure or previously-passed omissions. Report actual
PASS/FAIL/SKIP and precise unavailable/unrun boundaries from CTest and native
GoogleTest XML; process exit0 alone does not prove test bodies passed. Preserve
raw logs, JUnit/XML, source/config/image/inventory bindings and driver diagnostics.
AI diagnoses the failures and groups related repairs; it does not manually
reconstruct counts or pins already available in standard artifacts. The small
checked-in evidence helper summarizes facts without running tests or admitting resources.

## Periodic complete live checkpoint

Periodic live testing remains part of development. After explicit product resume,
the coordinator records active-development time and a due timestamp in the one
compact handoff: a checkpoint becomes due after **12 hours of active product
development** since the last accepted checkpoint. Pause the active-time clock
during project holds; on resume derive `live_due_at` from remaining active hours.
Check due state at ordinary task boundaries, not with a timer, watcher, AI polling
loop or new goal. Also require a checkpoint before release and when execution,
fixtures, ABI or driver-manager changes warrant one. Coalesce ready work.

A due checkpoint is scheduling debt, not permission to launch. Prepare source,
builds, complete applicability inventory and inputs while the resource is PAUSED.
Normally obtain green applicable CI on the exact tested source before live launch.
Any diagnostic sequencing exception requires explicit human direction; none is
granted by this workflow. No live launch during workflow replacement or paused
development, and no automatic cluster resume.

An initiated checkpoint covers **all existing applicable live tests across their
owning binaries and supported applicable profiles**. Use the checked-in standalone
CTest registrar and independently sourced expected inventory. Finite execution
partitions preserve that whole inventory; they are not feature selection. Collect
all PASS/FAIL/SKIP, setup failures, crashes and unrun cases before grouped repairs.
Focused repair checks never replace final complete acceptance. Explicitly approved
reuse for GUI/docs-only changes needs source/input/profile applicability proof;
the accepted historical 1,548 live passes are preserved, not fresh-binary execution.
Retain the vendor-reference discrepancy as a historical non-blocking vendor result,
without weakening its assertion or claiming vendor conformance.

Small lifecycle helpers own external resource resume/pause, private input custody,
fixture preparation/cleanup and owned process cleanup; they do not schedule cases
or parse feature results. CTest fixture setup/cleanup may express test-local
dependencies; always supervise external resource cleanup even after CTest failure.
Reuse the checked-in live registrar and normal `live_checkpoint.py` entry, which
composes the canonical resource/fixture/input modules. Windows uses local WSL for
resource ownership and the native Windows custody/job/CTest backend. The
command matrix records supported boundaries and pending real execution proof.
Ordinary local work has no private
multi-version script prerequisite. Do not replace resource controls with a generic
privileged executor, silently discard security gates, or recreate billing probes.
Existing finite operational/identity/TLS/network/credential/resource/caps and exact
principal/query/input/process cleanup remain. Human billing stays manual. Keep the
cluster PAUSED during preparation/build/CI gaps and at the end of every admission.

## Handoff and closure

Keep one compact private handoff containing **head/tree and task; actual results
and evidence path; qualification debt/blocker; next step and authority**. Include
`live_due_at`/remaining active hours when development resumes. Archive superseded
handoffs outside default context, preserving their bytes and sensitive access.
Do not copy historical instructions into each chat, create extra boards, status-only
commits or permanent supervisor loops. A handoff cannot resume a human hold.

Before closure, verify owned process trees, resource pause/shutdown, fixture/input
cleanup and leases. Independently verify private PostgreSQL stopped when used.
Preserve unresolved cleanup as concrete debt. Inventory retained build artifacts
and exact obsolete retirement candidates; do not delete evidence to simplify state.
Integrate reviewed exact bytes only when authorized and the intended gates pass.
Report the source, changed behavior, actual checks, remaining limits and next step.
At closure, preserve any holds outside the authorized task.

Historical migration snapshot recorded for the 2026-10-20 workflow replacement
(not current qualification status): main `0b330e4` remained separate from candidate
`a243001`; six builds and five complete local suites passed, while the final native
PostgreSQL suite was interrupted after 121/127 and its replacement never started.
Its exact registry fixtures remain installed pending separately authorized cleanup.
The original 1,548 live passes and accepted GUI-only reuse proof are retained;
that workflow refactor did not claim its new controls actually executed live.
Use the compact current private handoff for current source, results and unresolved
debt; it grants no resource authority.
