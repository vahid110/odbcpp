# ODBCPP agent and timer operating plan

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
