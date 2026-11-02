# ADR 0002: Verifiable Exact-Head Independent Review Gate

## Context

Per [ADR 0001](0001-development-process.md) (decision 10) and [AGENTS.md](../../AGENTS.md):
- Authors act on GitHub as `odbcpp-agent[bot]`.
- Reviewers act on GitHub as `odbcpp-reviewer[bot]`.
- Every PR requires an independent correctness review of the exact head commit.
- Changes touching high-risk categories (`security`, `auth`, `protocol`, `transactions`, `process`) also require a second, independent red-team reviewer on the exact head commit.

Issue #161 identified a governance gap where past PRs were merged based on code-owner approvals without verifiable records of independent agent reviews on the exact head commit, and ADR 0001 allowed an enforceable check to be added.

## Decision

1. **Automated Review Gate (`tools/ci/verify_pr_reviews.py`)**:
   - Evaluates GitHub PR metadata and review records against the PR's exact `head.sha`.
   - Requires an `APPROVED` review from `odbcpp-reviewer[bot]` with role tag `[correctness reviewer]` matching the exact head SHA.
   - For high-risk PRs (carrying labels `security`, `auth`, `protocol`, `transactions`, or `process`, or modifying files in designated high-risk paths), requires a second `APPROVED` review with role tag `[red-team reviewer]` matching the exact head SHA.
   - Any review submitted on an earlier commit is classified as stale and cannot satisfy the gate.
   - Any subsequent `CHANGES_REQUESTED` or `DISMISSED` review invalidates approval for that role.
   - Reviewer independence is enforced: the two reviews must have distinct IDs, distinct timestamps, and non-identical bodies.

2. **Reviewer Independence Model**:
   - The shared GitHub App account (`odbcpp-reviewer[bot]`) is an identity conduit; independence is established at the execution level:
     - Reviewer agents are spawned in isolated subagent execution contexts without shared memory or context with the author agent.
     - Red-team reviewers are prompted independently with adversarial verification instructions and do not observe the correctness review transcript.
     - Review role headers (`[correctness reviewer]` and `[red-team reviewer]`) attribute the distinct responsibilities.

3. **Continuous Integration Gate (`.github/workflows/review-gate.yml`)**:
   - Triggers on `pull_request` (open, sync, label) and `pull_request_review` (submit, edit, dismiss).
   - Serves as a required status check before merge.

4. **Human CODEOWNERS Separation**:
   - Human code-owner approvals (`@vahid110`) required by `.github/CODEOWNERS` are preserved as a distinct requirement and do not substitute for or waive the independent agent review contract.

## Consequences

- All PR merges are provably verified to have exact-head independent reviews.
- Force-pushes or commit updates immediately invalidate prior approvals, requiring re-review.
- High-risk changes cannot merge without documented adversarial review.
