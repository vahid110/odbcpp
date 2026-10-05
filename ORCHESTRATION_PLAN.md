# ODBCPP agent and timer operating plan

Current reusable operating guidance is in [AGENTS.md](AGENTS.md) and the
[development batch skill](.agents/skills/odbcpp-development-batch/SKILL.md).
The sections below retain dated observations and overrides; their agent states,
allowances and timer observations are historical, not current authority. Read
the private current handoff before continuation or any live admission. Keep
changing statuses out of this operating plan.

Reviewed 2026-10-02 against the actual agent tree, Git worktrees and saved
automation definitions. This is coordination policy; it does not change release
gates, authorize additional AWS spending or qualify a crypto profile.

## Observed state

The integration owner works on `codex/transport-foundation`. Three child agents
are currently visible and completed:

| Agent | Delivered work | Current state / next use |
| --- | --- | --- |
| `dependency_audit` | Independent MySQL authentication review, including cleanup ownership and partial credential-write failures | Dormant; reuse for a finite review of the next relevant change at its actual candidate commit |
| `redshift_parity_preparation` | Parity inventory and fetch lifecycle review | Dormant; source-only preparation is complete for these assignments; further work needs a new bounded question |
| `s2c_qualification_strategy` | Windows bundled OpenSSL prevention and qualification strategy | Dormant; a strategy report, not implemented or qualified S2C work |

Older agent names in conversation/environment context do not establish running
workers. Verify the live tree before reporting activity. Completed agents are
not automatically continuing their tracks.

The Redshift and S2C research worktrees remain at `77b0a44`. Their three report
files are byte-identical to the versions integrated in the primary checkout.
Retain these worktrees dormant for now. Before assigning more work, inspect
their status, preserve any newly discovered changes and refresh them to the
chosen integration baseline. Do not mistake an old worktree for current code.

## Assignment and integration rules

- Keep short or dependent steps in the integration chat. Delegate independent
  substantial work with a finite deliverable; do not delegate merely to fill slots.
- Reuse a suitable completed agent. For a new agent, supply only the relevant
  context and repository documents instead of the entire conversation history.
- Each assignment names its baseline commit, question, owned files, allowed
  mutations, required evidence and stopping condition. A report must distinguish
  source findings, proposals, implementation and qualification.
- The main chat owns shared interfaces, CMake, CI, crypto integration, release
  documents, commits, pushes and acceptance. An isolated implementation worker
  must have non-overlapping ownership; read-only review does not need a branch.
- Use the four available slots selectively: main implementation, an independent
  S2C or Redshift work package where useful, and review capacity. Do not run
  concurrent builds or fixtures against the same build directory or service.
- Review returned diffs/evidence before integration. Record the resulting commit
  and required checks. Stop completed research rather than extending it into
  speculative implementation. Repeated code reviews must target new changes or
  unresolved findings, not re-review unchanged code.

MySQL S3 remains the current primary implementation track. S2C T13/T14 can have
a separate bounded Windows implementation package after the integration owner
selects and scopes the strategy; its present completed report is not evidence
that such a package is running. Redshift may have an independent offline shared
runner or parity task. Paid live work requires the activation gate in
[REDSHIFT_LIVE_TEST_PLAN.md](REDSHIFT_LIVE_TEST_PLAN.md), including the global
USD 15 allowance, qualified shared lock/ledger runner and verified cleanup.

## Timers

The audit found all three ODBCPP automations paused. The legacy implementation
timer and application-checkpoint reminder stay paused; unrelated project timers
are outside this audit's mutation scope. The reason for the SDK timer's existing
pause was not established, so this review does not resume it.

The SDK continuation timer is updated for current S3 work, explicit S2C batches
and Redshift infrastructure/runner readiness rather than expired account access.
Its interval is now 30 minutes and its status remains paused. This interval is
an engineering choice to reduce redundant wakeups; it does not change testing.

When enabled, keep one implementation continuation timer for this chat. Do not
add separate timers for each agent or CI polling. Continue an existing CI run
before starting another pushed batch. Use focused development tests and the
complete required gates once per code batch; documentation-only coordination
updates do not justify a new full CI run. Preserve quiet notifications except
completed work, blocking failure or required user action. Reconcile the timer
prompt at milestone transitions and pause it at objective completion or on the
user's stop request.

## Assessment

The independent reviews and isolated research produced useful evidence, including
a real cleanup defect, and avoided conflicting core edits. Maintenance lagged:
the timer scope was stale, worktrees were not refreshed after integration, and
completed research could be mistaken for ongoing parallel development. More
always-running agents would not fix those issues. Finite ownership, current
baselines and explicit completion/integration records are the improvement.

## User scheduling override — 2026-10-02

After the audit, the user explicitly resumed the SDK continuation timer and
requested 15-minute intervals. The saved timer is now ACTIVE at that interval;
this supersedes the earlier observed pause and 30-minute audit setting. The
other ODBCPP timers remain paused. Notification and batching policy is unchanged.

## Parallel Redshift kickoff — 2026-10-02

At integration baseline `d403241`, the user authorized the bounded R0–R3
activation sequence in `REDSHIFT_LIVE_TEST_PLAN.md`. The existing Redshift agent
is assigned only the R0 offline preflight and lock package in refreshed worktree
`/Users/vahidsbr/odbcpp-redshift-parity`, branch `codex/redshift-live-runner`.
Its previous untracked research was preserved before switching baselines.
It owns only `tools/redshift/pilot_preflight.py` and its offline test file;
shared interfaces, build/CI files, commits and integration remain central.
The existing S2C strategy agent performs a finite read-only runner/fixture
activation review, without changing S2C scope or qualifying any provider.

The SDK timer is temporarily paused during this kickoff. Its updated prompt
includes the authorized parallel activation packages and retains the 15-minute
interval, global allowance and fail-closed live gates. Re-enable one continuation
timer once ownership and the next safe package are established; environment
readiness must be independently recorded before that timer admits a live test.
No per-agent or CI polling timer is added.

After establishing the two finite assignments and recording the next safe
packages, the same 15-minute timer was resumed with the updated prompt. It may
continue offline R0/R1 and MySQL work while infrastructure is unavailable;
paid Redshift admission remains blocked until R1 and R2 are independently
qualified. This avoids leaving parallel work waiting for another manual nudge.


## User-authorized resumption and MySQL isolation — 2026-10-03

The user explicitly resumed development timers and requested parallel MySQL work. Keep one SDK continuation timer ACTIVE at15-minute intervals; duplicate legacy implementation/reminder timers remain paused. This supersedes the earlier MySQL deferral for bounded parallel packages. The12-hour shared Redshift cost monitor remains active independently.

The live tree had no active MySQL worker at verification. Root assigned `mysql_parallel` one finite MySQL-specific correctness/regression package in managed worktree `/Users/vahidsbr/.codex/worktrees/mysql-parallel/odbcpp`, baseline5862a4d. It owns only MySQL backend and MySQL-specific tests, no shared SDK/CMake/CI/crypto edits, cloud operations, commits or pushes. Root owns review/integration and required gates. Completion stops that assignment; subsequent work needs a fresh finite scope, and dormant workers must not be reported as progressing.

Redshift feature qualification stays in checked-in GoogleTests shared with future live CI; Python orchestrates protected execution and independent cleanup. Local cloud proof and platform CI results remain separately reported until explicit cloud jobs are integrated.


## Continuous parallel MySQL lane — 2026-10-03

The user requires MySQL to remain included in unattended continuation, rather
than being covered only by integrated CI gates. The active fifteen-minute SDK
timer checks actual agent state, reviews and integrates completed finite
packages, and assigns the next bounded nonoverlapping MySQL S3 package without
waiting for another user prompt. Redshift retains priority; shared interfaces,
CMake, CI, crypto and ODBC stay owned by the integration owner. No additional
agent timer or CI watcher is created.

The next MySQL package starts from exact baseline2def252 in managed worktree
`/Users/vahidsbr/.codex/worktrees/mysql-s3-next/odbcpp`. The earlier worktree has
local changes and remains preserved. The assignment owns only MySQL-specific
tests, investigates evidenced prepared-result boundary coverage, and produces
a finite patch and focused checks. A production defect must be reported with
evidence rather than hidden by weakened assertions. Completion stops that
assignment; root verifies status and deliberately selects the next package.
