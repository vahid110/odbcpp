# Agent updates to the ODBCPP Project

The [ODBCPP Development Project](https://github.com/users/vahid110/projects/1)
is a view of issue/PR progress, never a development or merge gate. This workflow
does not run product tests, approve PRs, merge code or authorize live resources.

## Agent usage

Using the existing `odbcpp-agent[bot]` credential, post a separate issue comment
containing exactly one command, for example:

```text
/project working
```

| Command value | Project Status |
| --- | --- |
| `backlog` | Backlog |
| `ready` | Ready |
| `working` | In progress |
| `review` | In review |
| `ci-merge` | CI / Merge |
| `blocked` | Blocked |
| `done` | Done |

Keep the claim, worker identity, results and explanation in ordinary issue/PR
comments. Only the standalone command is interpreted. `done` requires the issue
already closed as completed after checking **all** acceptance criteria. Merging
a PR does not trigger Done. Ordinary test/compiler repairs remain Working;
Blocked means a documented external dependency. The automation changes only
Status; it preserves Worker Identity, priority and other project fields.

The owner can use the same issue command. PR comments, other actors, non-main
workflows, edited commands and unauthorized rerun actors are refused. An existing item is
reused; an absent issue is added. Repeating the same status is a no-op. Inspect
the Actions result for confirmation; a failed update does not stop issue work.
Queued or re-run commands are refused when a newer authorized stage command
exists. Post a fresh comment to retry or reconcile; there is no separate dispatch
path with a competing ordering clock.

## One-time owner/coordinator setup

Personal Projects are outside `GITHUB_TOKEN`'s repository scope. GitHub recommends
a classic PAT with `project` scope for user-owned Projects; an installation token
with organization-Projects permissions is for organization-owned Projects.
See [GitHub's Project automation documentation](https://docs.github.com/en/issues/planning-and-tracking-with-projects/automating-your-project/automating-projects-using-actions).

1. Create the `project-board` GitHub environment. Allow deployments from the
   **branch `main` only**, with no tag patterns. No wait timer or routine reviewer
   approval is needed. Keep the workflow protected by existing CODEOWNERS/review
   and main branch protections.
2. Create a dedicated, expiring classic PAT with **only `project` scope** and save
   it as environment secret `PROJECT_BOARD_TOKEN`. This public repository needs
   no `repo` scope. Never put the token in a comment, command argument or agent
   handoff. Agents do not receive it. Rotate before expiry using the same secret.
3. Ensure Project 1 has the Status options listed above. The tool will not create
   fields/options or silently select a different Project. Disable built-in
   workflows that set Status on item addition, PR linking/merge, or issue closure,
   and disable automatic issue closure. Automatic item addition can stay enabled.
   This keeps the explicit issue command as the single automated Status writer
   and avoids promoting merged code with unrun acceptance tests to Done.
4. After the reviewed workflow merges, use an authorized issue command and verify
   both the successful Actions result and the saved board value. Repeat it to
   prove idempotency. Until this succeeds, operational setup remains incomplete.

**Permission boundary:** classic PAT `project` scope covers the owner's personal
Projects, not just Project 1. The workflow restricts every query/mutation to
`vahid110` Project 1 and ODBCPP issues. The secret is confined to the main-only
environment and passed only to the final trusted script step. The owner-backed
credential is a narrowly authorized exception for Project metadata; authors,
reviewers and mergers continue using their separate App identities. It must never
be reused for repository operations or distributed to worker machines.

The workflow runs checked-in main code, re-fetches the command/issue, verifies
numeric actor identity and rerun identity, and refuses redirects. Event text is
data, never executable shell. Workflow and API errors redact response bodies and
credentials. No schedule, persistent watcher, custom service or required CI gate
is introduced.

## Ownership and verification

`.github/workflows/project-board.yml` owns event routing and environment binding.
`tools/ci/update_project_board.py` owns only GitHub board adaptation and depends on
Python's standard library, the read-only repository token and the isolated Project
credential. This repository tooling has no SDK/product role or dependency.

Run `python3 -B tools/ci/test_update_project_board.py -v` for offline authorization,
hostile command, pagination, idempotency and credential-redaction regressions.
Existing PR CI discovers this test under `tools/`; no product test is removed or
reclassified. Live Project access is a separate smoke test, not an offline claim.
