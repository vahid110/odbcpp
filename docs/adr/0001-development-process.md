# ADR 0001: Development process

Status: Proposed (2026-10-29). Replaces `DEVELOPMENT_WORKFLOW.md`,
`PARALLEL_WORKFLOW.md`, `ORCHESTRATION_PLAN.md`, `DEVELOPMENT_HANDOFF.md` and
the long-form rules in `AGENTS.md` once accepted.

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
   reviewer that tries to break the change and does not see the first review,
   and need the owner's approval. Fixes get a focused follow-up review from the
   reviewer that raised them.
5. **Evidence is CI.** Required checks on every PR: build and unit tests on
   macOS, Linux and Windows for every product, the offline Driver Manager
   tests, ASan/UBSan, and CodeQL. CI artifacts (JUnit, sanitizer logs) are the
   evidence; no hand-assembled evidence packages, custody ledgers or binding
   files. `main` is protected and requires these checks.
6. **Nightly.** Full profile matrix, TSan, longer fuzzing and the connected
   Driver Manager suites run nightly on `main`.
7. **Live tests.** One workflow runs the registered live CTest set against
   AWS, triggered by a `run-live` label or nightly when `main` changed. It
   uses short-lived OIDC credentials (no stored keys), resumes the workgroup,
   runs the tests, and always pauses it afterwards, including on failure or
   cancellation. Costs are enforced in AWS (budget alarm, maximum run time,
   automatic pause), not by in-repo admission files.
8. **Cost and safety rules are unchanged**: USD 100 compute / USD 150 total /
   USD 50 other, human-only billing, the cluster paused between runs and at
   the end, no credentials in the repository or reports.

## Migration

No safety mechanism is removed before its replacement works.

1. Accept this ADR and the new `AGENTS.md`; enable branch protection with the
   required checks (after the per-PR unit and sanitizer jobs are green).
2. Build the live-test workflow and prove pause-on-failure and the budget
   guard on a deliberate failing run. Only then retire the admission,
   custody and operator-session tooling under `tools/development`.
3. Move plans and journals to `docs/history/`; open issues for every open
   item they contain; write `ARCHITECTURE.md` from the 2026-10-28 review.
4. Prune `tools/` to the load-bearing set (versioning, packaging, TLS
   fixtures, the test fixture factory, export and SQLWCHAR checks) and delete
   checks that only verify evidence formatting.

Estimated effort: 1–1.5 person-weeks, mostly steps 2 and 3.

## Consequences

- Status and history are visible in the repository and on GitHub.
- Reviews stay independent, and CI becomes the single source of evidence.
- Live runs need no manual admission, which removes friction but places
  trust in the AWS-side guards; step 2 must prove them before switching.
- Some institutional detail in the old plans is archived rather than kept
  current; anything still open becomes an issue.
