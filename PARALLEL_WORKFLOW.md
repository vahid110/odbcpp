# Parallel development and integration

Human approved on 2026-10-13. The objective is faster accepted implementation
with the existing architecture, independent review and acceptance requirements.
This supersedes root-only implementation/qualification, blanket root authorship
of shared files and the earlier prohibition on creating implementation agents.
It does not supersede cloud authority, qualification cadence or retained evidence.

## Local development reset — human directed 2026-10-13

The human instruction “Stop everything. Fix it and resume the work” replaces the
initial lane-specific baseline-controller/admission workflow for future ordinary
local development. Preserve every historical attempt, STOP, failure and receipt;
none is cleared, replayed or promoted to qualification. Fresh ordinary local
commands use new owned development builds. This change grants no paid/cloud,
SQL/live, private PostgreSQL or final release authority.

- `odbcpp-1` is the sole operational coordinator. It alone sends package-owner
  assignments, leases resources and owns integration. The user-facing chat may
  inspect/report progress and perform an explicitly requested workflow reset;
  after reset it routes steering through `odbcpp-1` rather than directing workers.
  A workflow-maintenance handoff temporarily stops that coordinator before another
  author changes policy or shared state; only one coordinator acts at a time.
- Package owners resume their already reviewed feature contracts and exact file
  leases. They implement and author GoogleTests now; a pristine pre-change
  executable baseline is useful evidence, not a universal prerequisite to coding.
  Where needed, demonstrate the regression on original source in the same owned
  graph before applying the fix, or by a preserved test-only source comparison.
  Record an unrun baseline honestly; candidate tests still must actually pass.
- Use the existing common `tools/development/with_build_slot.py`, an owned
  macOS/iODBC UCS4 CMake build, `--parallel 2`, actual command logs/exit codes and
  CTest timeouts. The owner supervises the command with a recorded finite deadline,
  stops an overrun and verifies its owned process tree before releasing resources.
  Check measured free space before/after each heavy command, reserve at least
  5 GiB free and measured peak plus 2 GiB reserve. Do not scan mutable CMake scratch
  files as a continuously enforced admission gate. A real disk/process/tool error
  remains a failed command to investigate; never relabel it as a pass.
- Bind local evidence to repository HEAD plus exact working diff, explicit CMake
  options/cache, toolchain and dependency versions/source overrides, test inventory,
  executable hash and actual results. Capture each command's source identity before
  launch and keep its source inputs unchanged until completion; concurrent coding
  uses a separate source snapshot/worktree. Retain `compile_commands.json` and applicable
  architecture/header-staging checks. Ordinary generated SDK include units, CMake
  compiler-ID/ABI probes and platform shared-cache images do not require separate
  source-category approval, full expanded traces or syscall/loader proof before
  a local configure/build. This scope is ordinary local feature qualification;
  explicit security tests and protected final/live requirements still apply.
- Retire the bespoke BIT/routine baseline controllers and their forensic input
  disposition pipeline from the active execution path. Preserve their artifacts
  as historical evidence. Stop diagnostics-only successors, new guardians,
  manifest-category reviews and synthetic proof work unless an actual product
  failure specifically requires a bounded tooling repair.
- Review a complete candidate with its focused qualification and ownership table
  in one finite independent pass. Follow up only on concrete affected findings.
  Do not invent separate reviews/admission documents for each routine local
  command or generated-input category. The coordinator may approve a package's
  bounded configure/build/focused-regression sequence as one resource assignment.
  Full CI/live/final acceptance debt stays recorded under the existing cadence.

## Two lanes and one integration coordinator

Start with two complete package owners, automated qualification and a shared
independent reviewer pool. A package owner implements, authors meaningful tests,
investigates failures and resolves review findings. Qualification runners execute
prescribed checks and retain their actual results; an additional agent is not
required merely to relay a command or result. Independent reviewers assess ready
candidates without authoring their implementation. Review findings go directly
back to the package owner, and concrete fixes receive the necessary independent
follow-up review.

The coordinator selects dependency-compatible packages, leases files/contracts,
schedules constrained resources, checks combined behavior and integrates reviewed
exact bytes. It owns the integration branch and its commits/pushes, full CI,
live admission/accounting, acceptance backlog and cleanup. It does not repeat a
lane's entire implementation or focused qualification investigation.

| Action | Package owner | Independent reviewer | Coordinator |
|---|---|---|---|
| Implement and author unit/live tests | Own assigned package | Assess candidate | Set dependencies and scope |
| Run focused checks and investigate failures | Own; use automated runners | Assess evidence and limits | Schedule shared capacity |
| Change shared SDK/CMake/CI/crypto/ODBC files | Allowed under exact bounded lease/interface plan | Assess physical ownership and compatibility | Grant/sequence lease and integrate |
| Commit/push and open a candidate PR | Assigned worker branch only | Read candidate | Integration branch only; merge/integrate |
| Run combined regression/full CI | Supply candidate evidence; investigate assigned failures | Assess applicable results | Own execution and exact-head binding |
| Paid live work, principal/resource cleanup, finance | No authority from lane assignment | Independent admission/result review when assigned | Existing protected operator authority only |

Worker branch commits, pushes and PRs are deliverables, not integration acceptance.
Do not enable automatic integration merges. Existing finite authority still governs
cloud work even when a tool exposes controls for it. Reviewers do not inherit
permission to execute paid scenarios from a review assignment.

## Package selection and isolation

Use the product plans and actual handoff to select complete deliverables, normally
three to five related scenarios where compatible. Do not manufacture work to fill
a lane. Inspect SDK/core, Redshift catalog/types, MySQL and authentication at
continuation checkpoints; parked scope remains parked until authorized.

Each assignment must state:

- Observable completion condition, exact integration baseline and worker branch.
- Exact leased files and affected contracts, with planned interface/dependency changes.
- Permitted actions, owned worktree/build directory, prescribed focused checks.
- Deliverable and stopping condition, including what remains pending.

Disjoint directory names alone do not establish independence. If packages require
the same contract/lifecycle/conversion change, implement and review that prerequisite
first, integrate it, then dispatch dependent packages at its exact new baseline.
Do not hide a shared SDK foundation in a backend to avoid a lease collision.
Workers may author bounded shared changes; exclusive integration authority does
not imply exclusive shared-file authorship.

Prefer attached managed worktrees for independent implementation. Inspect baseline
and local changes before reuse; preserve historical dirty worktrees. Any Agent
Orchestrator project/worktree must explicitly use `codex/transport-foundation` as
its integration ref and record the resolved commit. Do not silently use remote
`main` or the tool's default branch. Branches use the `codex/` prefix. Integration
rebases/conflict repairs invalidate affected review/evidence where bytes or inputs
change; preserve original failures and independently review the concrete successor.

## Resource scheduling and qualification

This host has 8 GiB RAM. Allow one heavy local configure/build/regression operation
at a time. Package owners use the common cooperative build slot, for example:

```sh
python3 tools/development/with_build_slot.py --wait-seconds 600 -- cmake --build <owned-build> --parallel 2
```

The host-wide lock is private continuation state; do not create a separate lock
per worktree. This advisory lock coordinates participating runners. It does not
stop arbitrary commands, impose a command timeout, authorize resource admission
or prove a test result. Retain existing bounded test/deadline/process controls.
The migration resource release (`heavy_builds_released=true`) applies before
new product builds, including through the wrapper. The legacy owner can release
resources while its immutable live checkpoint remains active; participating owners
then queue through the common slot. Do not infer release from low current CPU use
or acquire a new lock around an uncoordinated legacy build. Lightweight offline
tooling setup/regression checks may proceed during migration.

Use distinct owned incremental macOS/iODBC UCS4 build directories as required.
Reuse each current graph and retain source/configuration/executable identities,
test inventory, original failures and actual exit status. Do not copy a complete
historical graph per candidate or infer qualification from another lane's binary.
Source work, lightweight inspection and independent review may run concurrently
with the single heavy build. Check actual disk reserve/admission and register
artifacts under `AGENTS.md`; the coordinator owns finite reviewed retirement.

Keep meaningful feature/live assertions in checked-in GoogleTests. Python may
orchestrate builds, scheduling and offline safety checks. Keep SDK physical
ownership, dependency/staging/header-owner/public-first-include checks and semantic
ownership review. Developer-only orchestration belongs under `tools/development/`,
not in the product SDK: it is not a reusable runtime transport/session contract,
must not link into the driver and requires its own offline checks/reviewer
assessment. Candidate ownership tables must state that rationale when applicable.

Run focused checks during development, relevant primary regression once per
coherent package, and combined checks for the integrated candidate where affected
inputs require them. A worker's primary pass does not prove combined behavior.
Do not rerun unchanged applicable evidence merely to fill a transition.
`DEVELOPMENT_WORKFLOW.md` retains the 12-hour full-CI-first/live-batch checkpoint,
one immutable exact-head full run, original full-acceptance goal requirements and
all final release gates. Additional lanes do not create additional live sessions,
cloud admission, resources, principals, budget or authorization horizons.

Worker PRs target `codex/transport-foundation` and receive the primary workflow.
The full `ci.yml` graph excludes PRs targeting that intermediate branch; the
coordinator dispatches it on the exact frozen integration head at the checkpoint.
PRs targeting other branches retain automatic full CI. Keep every full job,
manual-dispatch input and final acceptance requirement; primary-only PR status
does not establish full/live/final qualification. Scheduling-tool offline tests
run in the primary workflow as well as during changes to that tooling.

## Board, candidate handoff and integration

Use Agent Orchestrator issues/workers as the package work board. Record the exact
baseline, ownership lease and lifecycle state there; a compact private metrics
ledger may retain timestamps and aggregate measurements. Do not introduce a
second custom board framework. States must reflect actual work: selected, waiting
on dependency/build/review, implementing, ready for integration, integrated or
blocked with a concrete cause. Delivered, completed or idle workers are not
active implementation. Use a named dependency/resource wait rather than an
invented active task. Keep at most one existing authorized coordinator app goal;
use no per-lane app goals, timers or duplicate watchers.

A candidate handoff contains only what the next owner needs:

- Package/branch, baseline, exact candidate head/diff and observable result.
- Component, exact owner file, reuse rationale, allowed dependencies and proof.
- Checks actually completed, source/configuration/executable/test identities,
  result locations and preserved original failures.
- Independent findings/disposition and exact reviewed bytes.
- Integration dependencies, remaining full/platform/live gates and authored cases.

The coordinator verifies leases/dependencies, current baseline, unrelated changes
and actual reviewed bytes before integration. Record combined primary results and
independent integration review. Accepted implementation requires reviewed
primary-qualified integration commit/push and durable acceptance debt; a worker
PR is insufficient. Distinguish that delivery from full/live/final acceptance.
Resolve integration findings in a preserved successor, then start the next ready
authorized package without waiting for a timer tick. The coordinator replaces
current facts in the handoff, records debt in the existing acceptance backlog and
reconciles actual workers before ending a development turn.

## Six-package evaluation

Evaluate the first six real dependency-compatible packages. Do not count setup,
empty lanes or status-only changes as product packages. Record package-ready,
implementation start, candidate-ready, review, build and integration timestamps
and the periods waiting on review, builds or coordinator attention. Measure:

- Elapsed time to integrated primary-qualified implementation and accepted output.
- Review/build/coordinator waiting and coordinator intervention time.
- Integration rework and failures discovered after lane review.
- Outstanding full/platform/live acceptance debt and its age.

Compare against available recent serial-package evidence without inventing missing
baseline timings. Keep quality gates unchanged. Expand beyond two lanes only when
measured accepted throughput improves without worse rework or growing acceptance
debt; changing the agreed concurrency model requires human steering. A tool's
running-agent count or patches produced is not delivery throughput.

## Migration cutover and current live checkpoint

Setup proceeds now in an isolated workflow candidate. The original development
chat retains its in-flight exact `c3a7ab5244083173189c5c99cad48750cd09e4f2` live
checkpoint, binary/control identities, one-use admission and financial/cleanup
ownership. Do not migrate or restart that owner, mutate its warm source/build or
protected artifacts, replay consumed approval or change its current execution on
behalf of this setup. The user has authorized this two-lane migration and bounded
isolated source implementation; parked product scope and paid limits remain.

The private `~/.local/state/odbcpp/parallel-workflow/setup/cutover-state.json`
records three distinct coordinator releases:

| Release | Required condition | Work enabled |
|---|---|---|
| `source_lanes_released=true` | Existing live owner's scope/lease acknowledgment; exact immutable checkpoint, isolated worktrees, bounded nonoverlapping assignments and protected artifact boundaries recorded | Up to two isolated source packages, lightweight checks and independent source review |
| `heavy_builds_released=true` | Existing owner's recorded resource release; participating heavy operations use the common slot, owned builds and normal reserve/deadline controls | Queued focused product builds/qualification without touching the legacy warm graph |
| `integration_live_released=true` | Actual live owner terminal, owned-process reaps, exact-principal/resource cleanup and pause results, protected accounting/evidence retention and reconciled handoff | Integration-branch mutations and live ownership transfer under existing finite authority |

These conditions serve different resources. Isolated source lanes need neither
live completion nor full live acceptance once their scope is acknowledged. Heavy
build resources may be released while live tests remain active, but source-lane
release alone does not admit a heavy build. Integration/live transfer requires
actual terminal/cleanup/accounting release; a test assertion failure with preserved
pending acceptance is distinct from an unresolved cleanup/ownership condition.
A successful wrapper command, tool status, elapsed time or admission expiry is
not a release. Independently assess actual cleanup under existing rules. Keep
unresolved protected cleanup with its owner while authorized isolated source
work continues within its acknowledged boundary.

After source-lane release, dispatch the first two compatible bounded packages at
recorded immutable integration baselines. Their worker branches may advance;
the integration branch and live checkpoint stay owned by the existing chat until
integration/live release. After that release, the coordinator integrates the
independently reviewed workflow/tooling candidate and ready product candidates,
checks the resulting integration head and records remaining acceptance debt.
Do not label pending full/live/final gates complete to enable the transition.

The existing continuation and separate cost monitor retain their actual state;
this setup creates neither a timer nor an automatic restart of the currently
paused continuation. Preserve the legacy warm-source/build pause; this newly
authorized isolated source workflow uses its own recorded releases. Paid/cloud
execution still requires separately reviewed finite admission. No setup milestone,
work-board entry or app-goal transition grants live authority.
