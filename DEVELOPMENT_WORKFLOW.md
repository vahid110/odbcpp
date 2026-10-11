# Redshift development and qualification cadence

Human approved trial on 2026-10-11. This supersedes the 2026-10-09 24-hour
cadence, the earlier 48-hour trial, per-feature live execution, routine extra-profile
checks and mandatory separate API worker.
Product scope, independent review, live-test authority and final acceptance remain.

## Development loop

- Use one incremental macOS/iODBC UCS4 working build for each typical coherent
  candidate. Run focused tests, relevant primary regression and applicable
  architecture, staging and packaging checks. Reuse its graph. Do not routinely
  build UCS2, sanitizer, alternative providers, Windows or Linux per candidate.
- Inspect portability and affected contracts during source review; record other
  profiles as pending rather than claiming they passed. Collect their checks in
  the periodic sweep. Run an extra check early only for an actual failure, a
  concrete safety/ABI defect needing immediate diagnosis, an explicit human
  request; record the reason. Pointer/layout/Unicode
  changes alone do not automatically trigger every profile.
- Root owns implementation, qualification, integration, Git and CI as one
  continuous workflow. Independent consequential source/result/final review
  remains independent. No mandatory dedicated API worker or repeated author/
  qualification handoff. Reuse an existing worker only for a substantial,
  nonoverlapping package when it demonstrably saves time; do not create agents.
- Keep reviewed primary-qualified pushes moving. The existing primary workflow
  on pushes remains primary regression; it does not prove native Redshift.
- Author meaningful happy, boundary and recovery GoogleTests with each product
  change. Queue new and affected live cases for the next 12-hour checkpoint;
  do not execute a separate live batch or prepare paid admission per feature.
  Continue reviewed primary-qualified commits and pushes with full-CI and live
  acceptance explicitly pending. A pending qualification goal does not prevent
  implementation of the next authorized coherent product package.

## Goals and pending acceptance

Human-approved clarification on 2026-10-11: scheduled acceptance must not block
ready product development. Future implementation goals cover one coherent product
package: implementation and owning SDK/backend/adapter placement, meaningful
checked-in unit/live scenarios, independent review, primary-profile qualification,
commit/push, and a durable entry for all pending full-CI/live acceptance. These
implementation goals can close when those requirements are proved; their closure
is not full platform, live or production acceptance.

The human explicitly requests a continuous goal chain, not timer-only development.
After proving an implementation goal complete, immediately create and start the
next concrete ready authorized implementation goal in the same development flow;
do not wait for the next timer wake or periodic qualification. Keep one active
goal at a time. The existing continuation is a complementary recovery/checkpoint
trigger, not the normal handoff mechanism. If no planned package is ready, record
the actual source dependency rather than manufacturing work.

The private `continuation/acceptance-backlog.json` retains accumulated qualification
debt across completed implementation goals. Each entry binds its feature, exact source/head, authored
cases, accepted evidence, missing gates and final disposition. Debt is cleared only
by actual applicable results and independent final review. Release/product readiness
still requires the complete graph, exact-head green full CI, actual applicable live
coverage, PostgreSQL shutdown and owned-process reaps.

For the human-requested split of an existing goal, preserve its full requirements
in the acceptance backlog and distinguish the proved implementation phase from
unfinished acceptance. Use supported goal controls to redefine the implementation
objective before closing it. If those controls cannot edit or replace an unfinished
goal, report that concrete tool limitation; do not silently reinterpret an app
objective or mark the old full-acceptance goal complete. Their pending scheduled gates
must not prevent ready authorized product implementation. A scheduled due time is
pending acceptance, not a reason to mark a new implementation goal blocked. Report
actual no-progress honestly without inventing work or a live process wait. Respect
human pauses, genuine blockers, budgets and approval dependencies. The human has
explicitly authorized continued timer development and due-checkpoint qualification
for the current ACCESS_MODE goal despite its earlier cadence-only blocked label;
this does not remove its original final acceptance or require an early test run.

## Combined 12-hour qualification checkpoint

- The existing 15-minute continuation checks the durable 12-hour due time in
  `continuation/ci-cadence.json`; no additional timer or watcher. When changed
  source has accumulated, freeze a ready pushed head and the applicable pending
  test inventory. Reuse a matching completed green or active full GitHub CI run;
  otherwise dispatch one full platform/profile run on that exact head.
- First make full GitHub CI green. Collect failures, make coherent independently
  reviewed corrections, run affected local checks, push and qualify the corrected
  head. Follow one immutable run at a time; preserve original failures and avoid
  duplicate runs, automatic cancellation and unchanged accepted-gate replay.
- Only after that exact tested source head has green full CI, bind its current
  primary native binary and run one reviewed finite Redshift batch containing
  accumulated new/affected cases and applicable authorized regression. Author
  tests during development; batch their execution and admission at this checkpoint.
  Record the selected inventory, exact source/binary/head, evidence applicability,
  pending cases and admission dependencies. Parked AUTH/Excel/MySQL and tests
  outside current authority are excluded, not silently counted as passed.
- Collect live failures into coherent fixes in the same checkpoint. Review and
  test affected changes, push, restore green full CI for the corrected tested head,
  then rerun affected live cases with fresh finite admission. Retain unchanged
  evidence only where source/input applicability actually holds. Never blindly
  replay a consumed approval, force an unsafe follow-up, or loop without a finite
  execution/accounting horizon; record concrete unresolved failures for continuation.
- Keep one bounded reviewed test-heavy cluster window for genuinely ready tests
  and admitted corrections when authorized. Pause during longer development/build/
  CI gaps, at window end or spending guards. Paid execution still requires fresh
  reviewed accounting/controls, canonical lock, one-use consumption, exact source/
  binary binding, bounded execution and exact-principal cleanup. Scheduling grants
  no new authority, resource, network, spending or authorization-horizon extension.
- Do not advance full/live qualification merely to close an individual feature
  goal. Ordinary product pushes stay primary-qualified with acceptance debt until
  the checkpoint. Only an explicit human request or concrete immediate safety
  diagnosis justifies an earlier full check; earlier paid live execution requires
  explicit human scheduling direction and the same finite admission.
- Existing full-acceptance goals and final release acceptance still require their original product scope, complete
  required graph, exact-head green full CI, applicable actual live evidence,
  independent final review, PostgreSQL shutdown and owned-process reaps. Pending
  evidence remains pending; the cadence does not reduce quality requirements.
- Preserve noncancelling CI queue groups and inspect GitHub's pending-run capacity
  before dispatch. Record a real queue wait instead of duplicating watchers.

Keep the continuation, cost monitor and periodic/ad hoc cleanup active. The
trial is reversible; change it only on further human instruction. Preserve old
results, immutable candidates, original failures and unrelated dirty worktrees.
