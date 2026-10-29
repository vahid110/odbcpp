# ODBCPP rules for agents

The process is defined in [ADR 0001](docs/adr/0001-development-process.md).
Current human instructions decide the active task; nothing here grants
authority beyond them.

## Working on a task

- Each task is a GitHub issue with acceptance criteria. Work on a new
  `codex/` branch from current `main`, in its own worktree; never reset or
  discard someone else's checkout or uncommitted work.
- One PR per issue. Description: what, why, change, tests, review, open debt.
  Report test results exactly (passed / skipped / failed, with names).
- Write a failing test first for every bug fix and show it failed before the
  fix. Feature assertions live in checked-in GoogleTests; Python may only
  orchestrate. Do not weaken or delete a valid assertion; when behaviour
  changes on purpose, say which assertions changed and why.
- Every changed control body containing GoogleTest macros has braces; run
  `tools/development/check_changed_gtest_control_flow.py --base <base>`.
- Configure products as `POSTGRESQL` or `REDSHIFT` and keep the explicit
  Driver Manager, SQLWCHAR and crypto profiles.

## Architecture

- Follow `ARCHITECTURE.md` (until it exists: `SDK_ARCHITECTURE.md` and the
  2026-10-28 architecture review) and `SECURITY_MODEL.md`. Provider-neutral
  contracts and reusable transport, security, deadline and lifecycle code
  live in the SDK; wire protocol, framing and backend state stay in the
  backend; ODBC handles, diagnostics and keyword spelling stay in the
  adapter. Authentication and the database SDK never depend on each other.
- Do not evade an architecture or build check to land a change; if a rule
  blocks a legitimate design, raise it.
- On changes that touch architecture, review the surrounding design and
  report shortcomings with proposals instead of patching around them.

## Review checklist (independent reviewer agent)

Correctness and edge cases; placement against the architecture; security
(secrets, input validation, file and network effects); tests prove the change
(red before green, no weakened assertions); compatibility and documentation;
the platforms and products actually built. Findings are ranked blocking /
should-fix / nit with `file:line` and a concrete failure scenario. Fixes get a
focused follow-up review. PRs labelled `security`, `auth`, `protocol`,
`transactions` or `process` also need the owner's approval.

## Evidence

CI is the evidence: required PR checks build and unit-test every product on
macOS, Linux and Windows, run the offline Driver Manager tests, ASan/UBSan and
CodeQL. Local runs are for development. Never present an interrupted run or a
different binary's result as a pass.

## Live tests and money

- Live AWS tests run only through the live-test workflow (OIDC credentials,
  automatic pause, AWS-side budget guard). Until it is proven, the existing
  live tooling and its admission rules remain in force.
- Limits are unchanged: USD 100 compute, USD 150 total, USD 50 other; billing
  is human-only. Keep the cluster paused between runs and at the end. Never
  put credentials in the repository, logs or reports.
- Check free disk space before heavy builds (keep at least 5 GiB) and delete
  only artifacts you created and no longer need.
