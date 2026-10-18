# ODBCPP development guidance

For implementation, qualification and integration batches, use the project skill
at [.agents/skills/odbcpp-development-batch/SKILL.md](.agents/skills/odbcpp-development-batch/SKILL.md).
It defines the repeatable workflow; this file defines durable repository rules.
The human-approved 2026-10-13 parallel execution model is defined in
[PARALLEL_WORKFLOW.md](PARALLEL_WORKFLOW.md). It supersedes root-only implementation
and blanket shared-file author restrictions; architecture and acceptance remain.

## Temporary local live-test workflow — human directed 2026-10-18

For the current checkpoint, use CTest on the local iMac to run the complete
applicable existing live suite and collect all failures before starting product
or test repairs. Ordinary assertion failures and isolated test-process crashes
must not stop unrelated tests. Preserve raw GoogleTest and CTest results; report
skips, setup failures and unrun cases truthfully. Python owns resource resume/pause
and fixture/input/resource cleanup, not per-case scheduling or feature-result
parsing. Use available driver logs and ODBC diagnostics alongside test results,
keeping private credentials out of reports.

Hold GitHub pushes, PR creation and hosted/multiplatform builds during local
collection and repair. Keep fixes in local commits. After the complete applicable
local live suite passes, raise reviewed PR(s) and resume required platform builds
and final CI qualification. This supersedes the CI-first and per-PR sequencing
below for this phase; local green does not establish cross-platform acceptance.
Existing identity, TLS, resource, finite runtime, cleanup and standing fixture
authority remain. Billing probes and timers remain disabled; general feature,
Auth and roadmap work stays held. Codex /root is the sole coordinator following
the recorded AO handover.

## Current human guidance — 2026-10-15

This guidance supersedes conflicting earlier billing, timer and selected-live-batch
instructions. [DEVELOPMENT_WORKFLOW.md](DEVELOPMENT_WORKFLOW.md) defines the checkpoint rules.
General feature, AUTH and roadmap successors remain **HELD**; the standing parallel
model does not dispatch new product work during that hold.

- Every PR requires at least one actual macOS, Ubuntu/Linux and Windows build.
  Full cross-platform tests remain a checkpoint requirement, not a per-PR test matrix.
- Every initiated live checkpoint covers **all existing applicable live tests**
  across their owning binaries and supported applicable profiles. Finite execution
  partitions must preserve the full inventory, not select features or omit prior
  passes. Report actual PASS/FAIL/SKIP and genuine unavailable boundaries; same-checkpoint
  evidence is reusable only with explicit source/input/profile applicability proof.
- The human checks billing manually daily. Remove automated billing/cost/budget
  monitors and probes, including pre-test Cost Explorer, Price List and Serverless
  budget probes and repeated financial-data/admission recalculation gates. Do not
  recreate them under a different label. Existing USD100 compute/USD150 total/USD50
  other/tax limits and all historical liabilities/consumptions remain unchanged.
- Preserve finite operational one-use admission, source/image/full-CI binding,
  identity, verified TLS, network, fixtures and exact principal/query/input/process
  cleanup. Keep the cluster PAUSED during preparation/build/CI gaps and at the end.
  Existing standing routine-fixture authority remains; this changes no principal,
  infrastructure, access, runtime or budget scope.
- The supervisor deleted the 12-hour cost monitor. The legacy 15-minute development
  continuation and 10-minute oversight remain PAUSED. Do not restart or recreate
  timers, watchers or monitors.

## Active workflow correction — 2026-10-13

The human-directed local development reset in `PARALLEL_WORKFLOW.md` supersedes
historical lane briefs and bespoke local baseline-controller admissions for new
ordinary local work. `odbcpp-1` is the sole operational coordinator; route worker
steering through it. Owners may implement their reviewed leases while qualifying
with ordinary CMake/CTest under the shared slot. Whole-toolchain forensic closure,
per-category generated-input reviews and pristine pre-code execution are not local
feature admission requirements. Preserve historical stopped attempts as failures,
use fresh owned builds, retain source/configuration/binary/test binding and actual
process cleanup, and keep all protected cloud/live/final acceptance requirements.

## Architecture and ownership

- Follow `SDK_ARCHITECTURE.md`, `SECURITY_MODEL.md`, `SDK_PRODUCT_PLAN.md` and the
  relevant backend/authentication plan. Concrete backend framing and native types
  stay backend-owned; ODBC adapts normalized, owning SDK contracts.
- Every implementation batch must classify new or changed contracts, classes
  and helpers by physical owner before coding: reusable provider-neutral
  contracts, transport/security/deadline primitives and shared session lifecycle
  belong in `sdk/`; native protocol/framing/key/state machinery remains backend
  owned; ODBC handles, diagnostics and API adaptation remain adapter owned.
  A feature starting in Redshift does not justify keeping its common foundation
  in the driver or postponing SDK ownership to an unspecified later refactor.
- Include a concise ownership table in each candidate report: component, exact
  owner file, reuse rationale, allowed dependencies and relevant proof. Generic
  functionality left outside the SDK requires an explicit reason and independent
  reviewer disposition. Do not move protocol semantics into the SDK or invent a
  generic framework/public API merely to satisfy the table.
- Final integration review must inspect actual foundation placement and reuse,
  not only filenames or passing tests. Verify header-owner/staging manifests and
  applicable architecture/public-first-include checks. These mechanical checks
  support, but do not replace, the semantic ownership review.
- One coordinator owns the integration branch, integration commits/pushes,
  combined qualification, live admission/accounting and cleanup. Four continuing product
  owners (`odbcpp-2`, `odbcpp-3`, `odbcpp-4`, `odbcpp-5`) may implement, test, investigate failures and commit/push their assigned
  worker branches and open candidate PRs. Finite assignments state exact baseline,
  leased files/contracts, permitted actions, checks, deliverable and stop condition.
  SDK/CMake/CI/crypto/ODBC authorship is allowed in a bounded package with an exact
  lease and interface plan; overlapping edits require sequencing or reassignment.
  Package owners do not merge into or push the integration branch.
- Inspect actual agent state. A delivered package is not continuing work. Check
  SDK/core, Redshift catalog/types, MySQL and authentication at continuation
  checkpoints; distinguish active work from a named prerequisite or wait.
- Apply `PARALLEL_WORKFLOW.md` and `DEVELOPMENT_WORKFLOW.md`: package owners
  finish implementation and focused qualification; automated runners execute
  checks; a shared independent reviewer pool reviews ready candidates. The
  coordinator schedules dependencies/resources and integrates exact reviewed
  bytes. Do not create a permanent developer/qualifier/reviewer trio per lane or
  repeated handoffs for small coherent changes.
- Follow the goal distinction in `DEVELOPMENT_WORKFLOW.md`: future product
  implementation goals close after reviewed primary-qualified commit/push and
  recorded acceptance debt. The periodic checkpoint and final release still own
  all full-CI/live/final gates. Existing full-acceptance goals retain their scope;
  scheduled acceptance must not stop ready authorized implementation.
  Immediately dispatch the next ready authorized package after verified closure
  and applicable source-lane release; do not wait for a timer tick. Keep package state on the
  work board, without per-lane app goals or timers. Preserve all accumulated gates
  in the private acceptance backlog, with truthful supported app-goal transitions.
- Every continuing owner has an exact assignment, a named wait with owner and
  resumption event/deadline, or a promptly reported genuine human decision. After
  verified closure/lease release, assign the next authorized package immediately;
  six-package evaluation and infrastructure closure are not project stops.
- Before ending a development turn, reconcile actual workers and the next
  product task with the handoff. Completed, selection-only or idle workers are
  not active implementation. Do not invent work to fill a lane or let routine
  qualification prevent ready product progress.
- Prefer attached managed Git worktrees for independent implementation. Reuse a
  suitable checkout only after checking its baseline and local changes. Preserve
  historical dirty worktrees; do not reset them to make room for a new task.
  Read-only reviews need no additional checkout. Existing immutable scratch
  candidates need not move during an active qualification run.

## Validation and evidence

- Feature and live assertions belong in checked-in GoogleTests. Python supplies
  orchestration and offline safety tests, not substitute feature assertions.
- Every new or changed `if`, `else`, `for` or `while` body containing GoogleTest
  macros must have explicit braces, including an outer body containing a nested
  assertion control. Keep control statements and assertion-containing bodies
  on readable separate lines; never join sibling controls on one line. Before
  expensive builds and during candidate review, run
  `python3 -B tools/development/check_changed_gtest_control_flow.py --base <exact-batch-base> <changed-C++-paths>`;
  reviewers can add `--head <exact-candidate-head>`. Run the checker’s focused
  offline tests when changing it. This changed-diff lexical check preserves
  whole-file context, ignores unrelated historical blocks, and flags unsupported
  changed scopes for explicit manual review. It is not C++ or GCC qualification;
  retain actual compiler checks and original failures.
- Configure the actual product with `TARGET_DATABASE=POSTGRESQL` or `REDSHIFT`;
  current CMake rejects a MySQL product build. Inspect real options and target
  inventories rather than assuming every historical preset is valid.
- Use distinct owned build directories. For Redshift, follow [DEVELOPMENT_WORKFLOW.md](DEVELOPMENT_WORKFLOW.md):
  owned incremental macOS/iODBC UCS4 builds for owners 2/3 where needed and
  12-hour full qualification. Owner 4 uses the reviewed normal Ubuntu owning-job
  workflow under coordinator dispatch, not a local heavy graph. Owner 5 uses the
  separate default-off Windows owning job described in `PARALLEL_WORKFLOW.md`;
  it is a continuing product owner, not a permanent qualifier. This 8 GiB Mac has one heavy-build
  slot, acquired through `tools/development/with_build_slot.py`; migration product
  builds require the legacy owner's recorded resource release. Run focused checks during
  development and relevant primary regression once per coherent batch. Keep full final acceptance gates.
  Inspect `.github/workflows/` and `cmake/Tests.cmake` for the current graph.
- Preserve original failures. Separate source inspection, compile/link,
  synthetic runtime, native local proof and platform CI. Bind results to actual
  source, executable, configuration and inventory; never inherit qualification
  from an older binary or another candidate.
- Reap gate processes and independently verify private PostgreSQL shutdown
  before final acceptance. Integrate reviewed exact bytes. Primary-qualified intermediate Redshift commits
  may precede periodic full CI; label full cross-platform tests and live acceptance pending. Follow one
  immutable exact-head full run to completion before closure of existing full-acceptance goals or final release acceptance.
  Every PR also needs the three-platform builds above; a primary-only result is insufficient.
  No status-only pushes, automatic cancellation or duplicate watchers.

## Build storage and periodic cleanup

The coordinator owns cleanup across the lanes. Use the existing
coordinator continuation state and closure callbacks, with no separate cleanup
agent or timer; the legacy 15-minute continuation remains paused.

- Check actual free space at each continuation. Inventory obsolete project build
  artifacts once daily, at qualification batch closure, and before a build when
  free space is below 5 GiB. Avoid a full disk scan on every wake.
- Target at least 5 GiB free. Before a new full build graph, require a measured
  additional peak estimate plus a 2 GiB reserve. Existing admitted continuations
  retain their recorded guards; no passed gate is invalidated or replayed.
- Keep a compact private artifact register in the continuation state: canonical
  path, owner, source/config identity, role, last use, allocated size, retention
  reason and exact retirement disposition. APFS logical sizes do not establish
  how much space deletion will recover.
- Reuse one current working build per required configuration where applicable.
  Retain required accepted evidence/reference images without copying an entire
  historical graph for each focused change. Record a retention reason and an
  eventual retirement condition whenever creating a build.
- At batch closure, retire independently reviewed obsolete loose objects and
  redundant recreatable build outputs under existing cleanup authorization.
  Required original failures, immutable candidates, reference images/archives,
  native inputs, private PostgreSQL data, source and dirty worktrees remain
  protected while their retention reason or dependency is unresolved. Age or a
  directory name alone never establishes that an artifact is obsolete.
- Before deletion, verify processes, open files and candidate/control references.
  Use exact reviewed inventories, held file/parent identities and a durable
  deletion ledger. Record measured free space afterward. Never blanket-delete
  /private/tmp or repeatedly ask for already authorized cleanup.
- If eligible cleanup cannot restore the reserve, record the concrete retained
  dependency and next action once. Continue ready source implementation without
  launching builds that predictably exhaust disk space. Review finite retirement
  of redundant retained outputs rather than keeping every build forever or
  repeatedly freeing only enough for the next command. Do not create a cleanup
  goal or create a separate housekeeping worker.

## Continuity and authority

On the authorized development host, read
`~/.local/state/odbcpp/continuation/current-handoff.txt` for changing tasks,
evidence and protected admission constraints. Replace current facts there;
archive superseded evidence without copying full history into timer prompts.
The handoff and a goal do not create new cloud authority. Read the work board for
package leases and actual lane states. Historical migration source/resource/live
release flags and accounting receipts remain in `PARALLEL_WORKFLOW.md`; do not
reenact their superseded financial gates. Preserve immutable source/binary/control
checkpoints and actual terminal/reap/principal-cleanup/ownership releases. Pending
live acceptance alone does not block separately authorized isolated source work;
the current human HOLD still applies.

Keep the legacy 15-minute development continuation and 10-minute oversight PAUSED.
The supervisor has deleted the 12-hour cost monitor; do not restore it or create
replacement billing probes, timers, goals or watchers. Current callbacks do not
resume product/AUTH work held by the human.
Notify on meaningful completion, failure, material blocker or required action;
otherwise keep scheduled checks quiet.

Paid/cloud work requires the protected operator handoff and separately reviewed
finite operational admission. Preserve the unchanged limits, historical uncertain
exposure and consumed nonces. Automated billing probes and repeated financial
recalculation are not admission prerequisites; the human checks billing manually.
Never infer credentials, SQL coverage or a scope/horizon extension from a timer,
synthetic pass, goal or estimate. Actual AWS SDK acquisition and verified-TLS
Redshift login remain separate completion requirements.

## Current Redshift cadence — human approved 12-hour batching 2026-10-11

[DEVELOPMENT_WORKFLOW.md](DEVELOPMENT_WORKFLOW.md) supersedes the older per-batch
full-graph/CI sequencing below. The coordinator retains the 12-hour full
CI-first/live-batch cadence and durable private ci-cadence.json; no new timer.
Author live tests during development and execute the full applicable live inventory
at each initiated checkpoint, collecting failures into coherent reviewed fixes.
Product implementation may overlap qualification only when authorized; the current
feature/AUTH/roadmap HOLD remains. All final scope, review, live authority, security
and cleanup requirements remain.

## Batch size and diagnostic execution — human agreed 2026-10-07

Default to coherent workflow batches, normally three to five related scenarios
when ready and compatible, rather than a separate integration cycle per case.
Run focused checks while developing, then relevant coherent primary regression
and independent integration review. Intermediate pushes may precede the periodic
full graph and full CI under DEVELOPMENT_WORKFLOW.md; full final acceptance stays
mandatory.
Preserve each scenario's assertions and result; never add unrelated product work
merely to fill a batch. The older no-replay rule does not omit previously passing
cases from a newly initiated full live checkpoint.

CI and real Redshift tests provide distinct evidence. Under the human-approved
12-hour policy, first make full GitHub CI green on the exact tested head, then
execute the complete applicable live suite. Earlier diagnostic live execution requires
explicit human scheduling direction and unchanged finite authority/admission;
ordinary feature completion is not an exception. Preserve original failures,
source/binary binding, independent review and exact-principal cleanup.

Partition the full applicable live suite into finite execution units with explicit
per-case and aggregate statement/time bounds, durable operational one-use admission
and verified exact-principal cleanup and pause. Partitions are execution bounds,
not semantic feature selection or permission to omit older passing cases. Keep the
cluster paused during gaps
in its admitted test session, including coding/builds/CI needed for that session;
do not keep it running to await speculative readiness. A genuinely executing
bounded live batch may overlap acknowledged isolated source lanes. Spending,
resource, principal, network and authorization horizons are unchanged.
