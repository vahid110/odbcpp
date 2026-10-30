# ADR 0001: Development process

Status: Approved by the owner in conversation on 2026-10-30; takes effect when
the owner merges this change personally (an agent merge does not count). Supersedes `DEVELOPMENT_WORKFLOW.md`,
`PARALLEL_WORKFLOW.md`, `ORCHESTRATION_PLAN.md`, `DEVELOPMENT_HANDOFF.md`,
`.agents/skills/odbcpp-development-batch` and the long-form rules in
`AGENTS.md`, except that the live-test rules of `DEVELOPMENT_WORKFLOW.md` stay
in force until migration step 2 is complete.

## Context

The process grew by accretion. In the 200 commits before 2026-10-28, 36
touched product source while 79 changed only tooling or CI and 32 only
documentation. `tools/` (about 35k lines) outweighs half the product (about
52k lines), and 35 root documents (about 17k lines) are mostly dated
journals. Status lives in private handoffs, so the repository does not show
the real state, and stale plans have caused real mistakes. Meanwhile the
safeguards that matter most were thin: pull-request CI ran no unit tests on
Linux or Windows, never tested the Redshift product, and `main` has no branch
protection.

The goals stay the same: coordinated work by autonomous agents, independent
review of every change, trustworthy tests, autonomous and frugal live tests,
and firm cost limits. Only the mechanism changes: standard tools and CI
replace hand-made evidence and custom ceremony.

## Decision

1. **Work tracking.** Each task is a GitHub issue with acceptance criteria.
   Milestones group phases; a project board shows status. Private handoffs
   and journals are retired. Decisions with lasting consequences are short
   ADRs in `docs/adr/`.
2. **Documentation.** Root documents: `README.md`, `ARCHITECTURE.md`,
   `SECURITY.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, plus `AGENTS.md` (about
   one page). User documentation (including the connection-keyword
   reference) lives in `docs/`. Existing plans and journals move to
   `docs/history/` unchanged.
3. **Coordination.** A coordinator agent per phase assigns issues to
   implementer sessions. One branch and one PR per issue, with a short
   description: what, why, change, tests, review, open debt.
4. **Independent review.** The author attaches the evidence to the PR (see
   `AGENTS.md`). Every PR gets a reviewer agent that did not write it; it reads
   the code and the evidence but does not build or run anything, and treats an
   unsupported claim as a finding. PRs labelled `security`, `auth`, `protocol`,
   `transactions` or `process` also get a second, independent red-team
   reviewer that tries to break the change and does not see the first review.
   Findings are posted on the PR; the author fixes and pushes, and the
   reviewers review again, until every reviewer approves. Approval covers one
   commit: any new push needs a new review. Agents merge their own PRs once
   the reviewers have approved the head commit and the required checks are
   green; only changes to the gate files of decision 10 wait for the owner
   (amended 2026-10-30).
5. **Evidence is CI.** Required checks on every PR (exactly, as of
   2026-10-30): Ubuntu and Windows build + unit + offline Driver Manager tests
   for POSTGRESQL and REDSHIFT; Ubuntu ASan/UBSan unit tests for both
   products; macOS build + unit + offline Driver Manager tests for
   POSTGRESQL (iODBC UCS4) only; and CodeQL. Not covered by required checks
   and therefore open: macOS REDSHIFT, sanitizers beyond unit tests, live
   behaviour, installers, the Windows GUI, connected Driver Manager suites,
   TSan and performance. CI artifacts (JUnit, sanitizer logs) are the
   evidence; no hand-assembled evidence packages, custody ledgers or binding
   files. Evidence cites CI runs whose head commit equals the PR head.
6. **Nightly and release.** A nightly workflow on `main` (full profile matrix,
   TSan, longer fuzzing, connected Driver Manager suites) is still to be built
   (migration step 2); once it exists, a red nightly is a blocking issue. A
   release is tagged only on a commit with green nightly and live runs, so no
   release can be tagged until both workflows exist.
7. **Live tests.** One workflow runs the registered live CTest set against
   AWS with short-lived OIDC credentials (no stored keys). Acceptance
   criteria, all required before it replaces the current live tooling:
   - Runs only on commits of this repository whose exact head already passed
     the required checks; never on fork code, never with
     `pull_request_target` checking out PR code, never from `workflow_run`.
   - Uses a protected `live` environment that requires the owner's approval
     (or a schedule on `main`); the OIDC trust is pinned to that repository
     and environment, and the IAM role can only resume, pause and connect to
     the one pilot workgroup.
   - One concurrency group, at most one scheduled run per day, a hard job
     timeout, a pause step that runs even on failure or cancellation, and an
     independent scheduled pause.
   - An AWS Budgets action that stops the workgroup at the limit (not only an
     alarm), proven on a deliberate failing run.
   - Labels never trigger a live run.
   - Prerequisite: agents have their own GitHub identity (decision 10), the
     live workflow files are owned by the owner through CODEOWNERS, and only
     the owner can approve the `live` environment.
8. **Cost and safety rules are unchanged**: USD 100 compute / USD 150 total /
   USD 50 other, human-only billing, the cluster paused between runs and at
   the end, no credentials in the repository or reports. Resuming cloud
   resources outside the workflow needs the owner's standing or explicit
   authorization.
9. **Kept from the previous process**: run full suites through failures (no
   stop-on-failure) and report from CTest/JUnit, never from a process exit
   code alone; distinguish product defects from fixture, input and
   environment defects; keep original failures and protected inputs; check who
   reads an artifact before deleting it; stop private PostgreSQL and verify
   cleanup after live or fixture work; Windows GUI checks run on a real
   interactive desktop, and elevated System DSN checks clean up their exact
   fixtures.
10. **Enforcement** (amended 2026-10-30). Agents act on GitHub as the
    `odbcpp-agent` GitHub App, not the owner's account; reviewers post their
    verdict as PR comments under the same identity. Branch protection on
    `main` requires the CI checks (up to date, applied to administrators).
    The review rule of decision 4 is followed by agents, not enforced by
    GitHub; a separate reviewer App with a required check can enforce it
    later if it is ever skipped. No general human approval is required. The owner
    approves, through CODEOWNERS, only changes to what defines the gates:
    `.github/` (CI, CODEOWNERS), `AGENTS.md`, `docs/adr/`, `.agents/`,
    `tools/`, `tests/security/`, the live suite (`cmake/LiveSuite.cmake`) and
    `DEVELOPMENT_WORKFLOW.md` while its live rules apply. Release tags
    (`v*`) can be created only by the owner, so nothing reaches users without
    the owner. The reviewer, not the author, assigns the high-risk labels.

## Migration

No safety mechanism is removed before its replacement works.

1. The owner merges this ADR and the new `AGENTS.md`. Branch protection with
   the required checks, up-to-date checks and enforcement for administrators
   is on (done 2026-10-30). Still to add: a required check that fails when a
   PR touches the high-risk paths (auth, protocol, security, transactions)
   without the matching label, and one that fails when the number of tests in
   a required run drops without an explanation in the PR.
1a. Give agents their own GitHub identity (done 2026-10-30: `odbcpp-agent`),
   add the CODEOWNERS file and the `v*` tag rule (decision 10). Required before step 2.
2. Build the nightly workflow and the live-test workflow; prove
   pause-on-failure and the budget guard on a deliberate failing run. Only
   then retire the admission, custody and operator-session tooling under
   `tools/development`.
3. Move plans and journals to `docs/history/`; open issues for every open
   item they contain; write `ARCHITECTURE.md` from the 2026-10-28 review.
4. Prune `tools/` to the load-bearing set (versioning, packaging, TLS
   fixtures, the test fixture factory, export and SQLWCHAR checks, and
   `check_dm_offline_evidence.py`, which required jobs use) and delete checks
   that only verify evidence formatting.

Estimated effort: 1–1.5 person-weeks, mostly steps 2 and 3.

## Consequences

- Status and history are visible in the repository and on GitHub.
- Reviews stay independent, and CI becomes the single source of evidence.
- Product code, including auth and protocol, reaches `main` without human
  review; the agent reviewers, CI and owner-only release tags are the
  safeguards. Test registration in `cmake/` is not owner-owned, so a dropped
  test is caught by review (and later by the test-count check), not by
  CODEOWNERS.
- Live runs need no manual admission, which removes friction but places
  trust in the AWS-side guards; step 2 must prove them before switching.
- Some institutional detail in the old plans is archived rather than kept
  current; anything still open becomes an issue.
