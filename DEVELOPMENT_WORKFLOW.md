# Redshift development and qualification cadence

Human approved trial on 2026-10-11. This supersedes the 2026-10-09 24-hour
cadence, the earlier 48-hour trial, per-feature live execution, routine extra-profile
checks and mandatory separate API worker.
Product scope, independent review, live-test authority and final acceptance remain.

Human-approved parallel execution on 2026-10-13 supersedes the root-only
implementation/qualification and no-new-agents restrictions of the 2026-10-11
trial. [PARALLEL_WORKFLOW.md](PARALLEL_WORKFLOW.md) defines four continuing product owners,
automated qualification, shared independent review, coordinator integration and
the separate source, resource and integration/live migration releases. The 12-hour cadence and all protected gates remain.

## Human-directed local workflow correction — 2026-10-13

Apply the local development reset in `PARALLEL_WORKFLOW.md` to both existing
packages and future ordinary local work. One operational coordinator (`odbcpp-1`)
directs four implementation owners. Owners 2/3 use ordinary owned CMake/CTest
builds through the shared local slot; owner 4 uses the reviewed one-job standard
free hosted Ubuntu workflow under automatic coordinator dispatch. Retain measured resource reserve, finite supervised commands and actual
source/configuration/binary/test evidence. Retained custom-controller failures do
not block a fresh ordinary build. Do not require sequential generated-input or
compiler-probe category approvals, full trace/loader/syscall closure, or a new
controller proof before coding an already reviewed contract. One complete
candidate/evidence review follows meaningful focused qualification. Original
failures, architecture checks and every full/platform/live/final gate remain.

## Development loop

- Owners 2/3 use one owned incremental macOS/iODBC UCS4 working build for each active
  coherent package where needed. Serialize heavy local configure/build/regression
  work through `tools/development/with_build_slot.py` on this 8 GiB Mac; source
  work and independent review can proceed concurrently. During migration, product
  builds require the legacy owner's recorded resource release. Run focused tests, relevant
  primary regression and applicable architecture, staging and packaging checks. Reuse its graph. Do not routinely
  build UCS2, sanitizer, alternative providers, Windows or Linux per candidate.
- Inspect portability and affected contracts during source review; record other
  profiles as pending rather than claiming they passed. Collect their checks in
  the periodic sweep. Run an extra check early only for an actual failure, a
  concrete safety/ABI defect needing immediate diagnosis, an explicit human
  request; record the reason. Pointer/layout/Unicode
  changes alone do not automatically trigger every profile.
- Four continuing product owners implement complete bounded deliverables, author tests,
  investigate automated focused qualification and resolve independent review
  findings. A shared reviewer pool reviews ready candidates independently.
  The coordinator owns integration branch Git, combined/full CI, live admission,
  accounting and cleanup. Workers can commit/push their leased worker branches
  and open PRs; only the coordinator integrates. Shared-file authorship follows
  exact package leases and interface plans rather than a blanket root-only rule.
  Keep the handoff concise; do not add a dedicated API worker or permanent
  qualifier/reviewer to each lane.
- Keep reviewed primary-qualified integration pushes moving. A worker branch
  push or PR alone does not establish accepted implementation delivery. The
  existing primary workflow on pushes remains primary regression; it does not prove native Redshift.
- Author meaningful happy, boundary and recovery GoogleTests with each product
  change. Queue new and affected live cases for the next 12-hour checkpoint;
  do not execute a separate live batch or prepare paid admission per feature.
  Continue reviewed primary-qualified commits and pushes with full-CI and live
  acceptance explicitly pending. A pending qualification goal does not prevent
  implementation of the next authorized coherent product package.

## Third-owner focused Ubuntu loop

Owner 4 qualifies product changes with the default-off normal mode described in
`PARALLEL_WORKFLOW.md`: one fresh standard Ubuntu 24.04 x64 runner, j2/30 minutes,
reviewed immutable workflow W separate from product P, allowlisted owning tests,
six architecture checks and 23 SDK headers. The coordinator automatically submits
one bounded manual dispatch after review; no owner dispatch or human-click gate.
Run the cheap changed-diff brace checker first. Reuse only exact compatible
compiler objects with actual owning producer provenance, never binaries/graphs,
and retain independent fresh-link/XML/source evidence and original failures.
Bounds remain 256 MiB cache, 20 MiB source-only evidence/job, three-day retention,
one global noncancelling Ubuntu heavy group; ordinary CI queues remain separate.

This is normal development qualification, not another three-job benchmark. Actual
Linux owning results do not inherit historical source results or clear required
macOS/iODBC primary, full/platform/native/live/final gates. Review candidate
ownership/evidence once, then assess exact current integration applicability;
repair concrete affected findings without replaying unchanged accepted audits.
Use one finite exact-head CI/native-review completion route with verified cleanup.
No third local heavy graph, paid/storage expansion, secrets or live authority.

## Fourth-owner focused Windows loop

Owner 5 owns continuing substantial Windows-primary packages under exact leases,
not permanent qualification or coordination. Use the default-off normal Windows
mode in `PARALLEL_WORKFLOW.md`: one standard free public Windows 2025 x64/MSVC
job, j2/30 minutes, immutable W separate from P, POSTGRESQL Release multiconfig,
Windows ODBC width2/OpenSSL BUNDLED_SHARED. The coordinator automatically submits
one job after independent workflow/safety review. First run is cold with no cache;
no final binaries/graphs are restored. Preserve actual MSVC flags/dependencies,
new objects/core libs/DLL/exe, SDK23/six architecture checks, complete owning
inventory/XML, failures and source binding in <=20 MiB text for three days.
One global noncancelling Windows group may overlap the separate Ubuntu group;
local j2 remains one slot. No services, registry/MSI/native SQL, paid runners,
secrets, goals/timers/watchers or cloud/live/privatePG authority. Direct-process
peak memory is not aggregate peak. Setup is not operational adoption or feature
throughput; actual Windows evidence and independent applicability review remain
required. A compiler/runtime finding needs a narrow root product lease before
fixing it; full Windows/native/live/package acceptance remains debt.

## Goals and pending acceptance

Human-approved clarification on 2026-10-11: scheduled acceptance must not block
ready product development. Future implementation goals cover one coherent product
package: implementation and owning SDK/backend/adapter placement, meaningful
checked-in unit/live scenarios, independent review, primary-profile qualification,
commit/push, and a durable entry for all pending full-CI/live acceptance. These
implementation goals can close when those requirements are proved; their closure
is not full platform, live or production acceptance.

The human explicitly requests a continuous goal chain, not timer-only development.
After proving an implementation goal complete, immediately select and dispatch
ready authorized packages after recorded source-lane release in the same flow;
do not wait for the next timer wake or periodic qualification. Keep at most one
coordinator app goal, only within existing explicit goal authorization. Package
entries on the work board track parallel work; do not create per-lane app goals
or timers. Preserve the original scope of any existing app goal. The existing
continuation is a complementary recovery/checkpoint trigger, not the normal handoff mechanism. If no planned package is ready, record
the actual source dependency rather than manufacturing work.

The coordinator records reviewed primary-qualified integration, not merely a
worker's local completion, as accepted implementation throughput. The first six
real dependency-compatible packages evaluate elapsed delivery time, review/build/
coordinator waits, integration rework, later failures and acceptance-debt growth.
The human-approved 2026-10-14 four-owner continuation supersedes the two-lane
trial. Six packages are an evaluation milestone, not a project stop. Every owner
has an exact active lease, a named dependency/resource wait with owner and
resumption event/deadline, or a genuine human decision promptly reported. Resolve
routine repairs within existing authority. Preserve parked scope; do not invent
filler. Reuse stable AO IDs, rename/read back current package names and align
stale supported native titles.

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

- The coordinator checks the durable 12-hour due time in
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
  CI gaps in that admitted test session, at window end or spending guards.
  A genuinely executing bounded live batch may overlap acknowledged isolated
  source lanes. Paid execution still requires fresh
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

Keep the legacy 15-minute continuation PAUSED and existing 12-hour cost monitor
ACTIVE, with the existing human 5-minute supervision; no new timer, goal or watcher.
Continuous assignment does not resume the paused continuation. Preserve their authorized state;
the migration does not restart a human-paused continuation. The human-approved
isolated source lanes can overlap the immutable live checkpoint after its owner's
scope/lease acknowledgment and `source_lanes_released=true`; heavy builds require
resource release, and integration/live transfer requires terminal/cleanup/accounting
release. Preserve the legacy warm-source/build pause and existing timer state;
new lanes do not await full live acceptance. Keep
periodic/ad hoc cleanup under coordinator ownership. The parallel trial is
reversible; change it only on further human instruction. Preserve old
results, immutable candidates, original failures and unrelated dirty worktrees.
