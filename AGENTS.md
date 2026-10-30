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

## Evidence the author attaches to every PR

Exact local test results per product (passed / skipped / failed, with the
names of anything skipped or failed and whether it also fails on `main`); for
a bug fix, the new test failing before the fix and passing after; paths to the
logs and JUnit files; and the CI results once they exist. Anything not run
(platforms, live tests, manual jobs) is listed as open, not implied.

## Review

Every PR gets one independent reviewer agent that did not write it. It reads
the code and the evidence; it does not build or run anything. It checks:
correctness and edge cases; placement against the architecture; security
(secrets, input validation, file and network effects); that the tests prove the
change (red before green, no weakened assertions); compatibility and
documentation; and that every claim is backed by the evidence (an unsupported
claim is a finding). Findings are ranked blocking / should-fix / nit with
`file:line` and a concrete failure scenario. A change that removes a test
from a required run (dropping its `unit` label, `WILL_FAIL`, `DISABLED`, a
new skip, or a no-op sanitizer option) is blocking unless the PR explains
it.

The reviewer, not the author, assigns the high-risk labels. PRs labelled
`security`, `auth`, `protocol`, `transactions` or `process` also get a second,
independent red-team reviewer: it tries to break the change
(hostile input, misuse, races, downgrade, resource exhaustion, bypassing the
new checks), does not see the first review, and reports the same way.

Reviews are posted on the PR. The author fixes and pushes; the reviewers
review the new head again; repeat until every reviewer approves. Approval
covers one commit. Merge when the reviewers have approved the head commit and
the required checks are green; only changes to the files in `.github/CODEOWNERS`
wait for the owner.

Authors act on GitHub only as `odbcpp-agent[bot]`
(`GH_TOKEN=$(~/.config/odbcpp-agent/token.sh)`); reviewers post their verdict
and the `independent-review` check as `odbcpp-reviewer[bot]` (until that App
exists, reviewers post as `odbcpp-agent[bot]` and the check is not enforced).
Never use the owner's login. The owner does not author PRs that touch owned
files, because GitHub does not let an author approve their own PR.

## Evidence

CI is the evidence. The required checks and what they do not cover are listed
in ADR 0001 (decision 5); state anything not covered as open. Cite CI runs
whose head commit equals the PR head. Local runs are for development. Never
present an interrupted run, a process exit code alone, or a different commit's
or binary's result as a pass. Keep the safeguards in ADR 0001 decision 9.

## Live tests and money

- Live AWS tests run only through the live-test workflow once it meets ADR
  0001 decision 7. Until then the live-test rules of `DEVELOPMENT_WORKFLOW.md`
  and the existing live tooling remain in force.
- Limits are unchanged: USD 100 compute, USD 150 total, USD 50 other; billing
  is human-only. Keep the cluster paused between runs and at the end. Never
  put credentials in the repository, logs or reports.
- Check free disk space before heavy builds (keep at least 5 GiB). Delete
  only artifacts you created, after checking nothing still reads them.
