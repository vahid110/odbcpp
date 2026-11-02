# Canonical Agent Workflow and Work Tracking Specification

This document provides concise operational instructions for autonomous and human agents working on `vahid110/odbcpp`. It implements the work-tracking and coordination decisions of [ADR 0001](adr/0001-development-process.md) and supplements [AGENTS.md](../AGENTS.md).

---

## 1. Core Principles

1. **Issues and PRs are Authoritative; GitHub is the Shared Truth:**
   - All tasks, assignments, status updates, and blockers live on GitHub.
   - GitHub Issues and Pull Requests are the authoritative operational mechanism. A GitHub Project board provides an optional Kanban visualization, but is a view rather than a gating coordination layer.
   - Local files, private handoff notes, and unpushed scratchpads are never coordination mechanisms.
2. **Standard Tools over Custom Machinery:**
   - Coordination relies on standard GitHub Issues, Pull Requests, CI checks, and standard Git commands. No custom coordination daemons, central orchestration servers, or custody ledgers are used.
3. **Strict Attribution & Zero Credential Leaks:**
   - Automated agents act strictly under dedicated GitHub App bot identities: `odbcpp-agent[bot]` for authoring, and `odbcpp-reviewer[bot]` for reviewing.
   - Personal credentials must never be used by agents. Bot tokens, keys, and authorization headers must never be committed, passed on command-line process arguments, or printed in logs.

---

## 2. Worker Identity Specification

To disambiguate concurrent workers across tools, platforms, and machines, every session identifies itself with the canonical tuple:

```
<tool>/<host>/<session>
```

- **`<tool>`**: `codex`, `agy`, `claude`, `copilot`, `cursor`, `human`
- **`<host>`**: `mac-arm64`, `mac-x86`, `win-x64`, `wsl2-ubuntu`, `linux-x64`, `remote-<name>`
- **`<session>`**: Unique session ID, terminal PID, or invocation hash (e.g., `433005a4`, `run-20261102`)

> [!NOTE]
> Components must contain only alphanumeric characters, hyphens, and dots (no forward slashes, colons, or whitespace) to ensure unambiguous delimiter parsing. Examples: `agy/mac-arm64/433005a4`, `codex/win-x64/session-88`, `claude/wsl2-ubuntu/worker-01`.

---

## 3. Work Tracking & Stages

Tasks progress through six conceptual stages on GitHub:

| Stage | Definition | State Transition |
| :--- | :--- | :--- |
| **Ready** | Unassigned task ready to be claimed | Issue has clear acceptance criteria; unassigned. |
| **Working** | Under active implementation | Claim comment posted on the issue with `Worker Identity`; isolated worktree created. |
| **Review** | Implementation complete; review in progress | PR opened by `odbcpp-agent[bot]` linking the issue; local test evidence attached; reviewers requested. |
| **CI / Merge** | Reviews approved; awaiting CI & merge | Independent reviewer approvals secured on exact head commit. Awaiting green required CI and CODEOWNERS sign-off if touching protected paths. |
| **Blocked** | Progress halted by external dependency | Blocker comment posted detailing the missing authority, credential, or decision. |
| **Done** | Acceptance criteria fully satisfied | All issue acceptance criteria verified (code merged, tests passing, open criteria closed). |

*Note on Done:* Merging a PR does not automatically mean `Done` if the issue specifies additional criteria (such as follow-up live tests or multi-platform qualification). An issue is closed only when all acceptance criteria are met.

---

## 4. Concurrency, Claiming, Recovery, and Handoff

### Claiming Work
1. An agent may claim any open issue that has clear acceptance criteria and is not currently being worked.
2. To claim:
   - Post an advisory claim comment on the GitHub issue stating your `Worker Identity`, target branch name (`codex/...`), baseline commit SHA, and initial approach.
   - Fetch latest `origin/main` and create an isolated worktree.
3. **Conflict Resolution:** Issue comments are advisory claims, not atomic distributed locks. If concurrent claims occur, the authorized phase coordinator (or repository owner) determines assignment.

### Ownership, Stale Activity & Routine Recovery
- **Anti-Hijacking:** Stale activity alone is NEVER permission to silently overwrite or take over a task.
- **Routine Coordinator Recovery:** If a worker session crashes, hangs, or becomes unreachable, the authorized coordinator (or human owner) does not need to wait indefinitely. The coordinator may:
  1. Inspect the worker's branch, worktree, and uncommitted dirty work.
  2. Preserve all existing work, failure evidence, and branch commits.
  3. Post an explicit reassignment comment on the issue transferring the task to a new `Worker Identity`.
  4. Resume execution under the new session.

### Blocker Escalation vs. Normal Development
- **Normal Development:** Transient compiler errors, failing tests, or local tool quirks encountered while coding are normal development activity; they must be diagnosed and safely repaired by the session without aborting the task.
- **External Blockers:** Escalate to `Blocked` ONLY when progress is halted by genuinely missing external decisions, unavailable repository permissions, or unresolvable environment boundaries.
- **Reporting:** Post a concise blocker comment stating: (1) exact failed operation, (2) exact error message (sanitized), and (3) the smallest remediation needed to proceed.
- **Credential Scrubbing:** Always sanitize and redact any token pattern (`ghs_*`, `ghp_*`, private keys, passwords) before posting comments or logs.

### Explicit Handoff Protocol
When transferring work between sessions:
1. Ensure all work is committed and pushed to `origin/codex/<branch>`.
2. Post a structured handoff comment on the issue and PR containing:
   - **Head Commit:** Exact commit SHA pushed to origin.
   - **Local Test Results:** Passed, skipped, failed tests with names and log paths.
   - **Acceptance Criteria Progress:** Checklist of satisfied vs remaining criteria.
   - **Next Action:** Clear next steps for the incoming worker.
3. Explicitly transfer or clear ownership in the comment so the incoming worker can claim the task cleanly without conflicting locks.

---

## 5. Secure Bot Authentication & Git Operations

Automated agents act only as `odbcpp-agent[bot]` for authoring and `odbcpp-reviewer[bot]` for reviewing. Never use personal credentials.

### Installation Tokens
Local token scripts use local private keys to generate short-lived (1-hour) GitHub App installation tokens:
- `~/.config/odbcpp-agent/token.sh` (`app_id="5540186"`)
- `~/.config/odbcpp-reviewer/token.sh` (`app_id="5545952"`)

### Token Validation
Strictly verify tokens before use to prevent silent fallback to personal user accounts, validating both token spelling and server-verified actor identity:

```bash
export GH_TOKEN=$(~/.config/odbcpp-agent/token.sh)
[ -n "$GH_TOKEN" ] || { echo "Error: GH_TOKEN is empty" >&2; exit 1; }

# Verify token belongs to the expected odbcpp-agent App (App ID 5540186)
case "$GH_TOKEN" in
  ghs_5540186_*) ;;
  *) echo "Error: GH_TOKEN does not match odbcpp-agent App ID 5540186" >&2; exit 1 ;;
esac

# Server-verified actor check: ensures GitHub recognizes the token as the intended bot identity
actor=$(gh api graphql -f query='query { viewer { login } }' --jq '.data.viewer.login')
[ "$actor" = "odbcpp-agent[bot]" ] || { echo "Error: Expected odbcpp-agent[bot], got $actor" >&2; exit 1; }
```

*(For reviewer validation, verify prefix `ghs_5545952_*` and expected actor `odbcpp-reviewer[bot]`).*

### Git Push via Credential Helper & Fail-Closed Transport Defense
> [!IMPORTANT]
> Never embed tokens into remote URLs or pass them as command-line arguments (e.g. `http.extraheader`), which are visible in process tables (`ps`). Instead, use Git's built-in credential helper powered by `gh auth git-credential`.
>
> **SSH Transport Guard:** Git's credential helpers only intercept HTTP/HTTPS transports. If a clone has an SSH `origin`, an SSH `pushurl`, or a Git `url.<base>.insteadOf` rewrite configured, Git bypasses `credential.helper` and attempts authentication via local SSH keys (risking personal identity leaks). Bot pushes must therefore:
> 1. Bind to the explicit canonical HTTPS URL (`https://github.com/vahid110/odbcpp.git`).
> 2. Verify that the effective resolved push URL is HTTPS (detecting and failing closed on SSH rewrites).
> 3. Pass `-c core.sshCommand="false"` to fail closed if Git attempts any SSH invocation under any configuration.
>
> Offline contracts proving this protection are tested in `tools/development/test_safe_bot_push.py`.

#### POSIX Shell (macOS, Linux, WSL):
```bash
# 1. Verify target repository resolves to HTTPS (detects SSH rewrites / non-HTTPS remotes)
TARGET_REPO="https://github.com/vahid110/odbcpp.git"
EFFECTIVE_URL=$(git ls-remote --get-url "$TARGET_REPO")
case "$EFFECTIVE_URL" in
  https://github.com/vahid110/odbcpp* | https://*.github.com/vahid110/odbcpp*) ;;
  *) echo "Error: Push target resolved to non-HTTPS ($EFFECTIVE_URL). Prohibited for bot push." >&2; exit 1 ;;
esac

# 2. Push over verified HTTPS with fail-closed SSH guard and ephemeral credential helper
git -c credential.helper= -c credential.helper='!gh auth git-credential' \
    -c core.sshCommand="false" \
    push "$TARGET_REPO" codex/<branch-name>
```

#### Windows Native (PowerShell):
```powershell
# 1. Verify target repository resolves to HTTPS (detects SSH rewrites / non-HTTPS remotes)
$TargetRepo = "https://github.com/vahid110/odbcpp.git"
$EffectiveUrl = (git ls-remote --get-url $TargetRepo).Trim()
if ($EffectiveUrl -notlike "https://github.com/vahid110/odbcpp*") {
    Write-Error "Error: Push target resolved to non-HTTPS ($EffectiveUrl). Prohibited for bot push."
    exit 1
}

# 2. Push over verified HTTPS with fail-closed SSH guard and ephemeral credential helper
git -c credential.helper= -c credential.helper="!gh auth git-credential" `
    -c core.sshCommand="false" `
    push $TargetRepo codex/<branch-name>
```

`gh auth git-credential` automatically reads `GH_TOKEN` from the process environment and supplies it over standard input/output without command-line exposure.

---

## 6. Development, Worktrees, and Testing

1. **Isolated Worktrees:**
   For **new tasks**, create a fresh isolated worktree from latest `origin/main`:
   ```bash
   git fetch origin main
   git worktree add -b codex/issue<number>-<slug> ../worktrees/issue<number> origin/main
   cd ../worktrees/issue<number>
   git config user.name "odbcpp-agent[bot]"
   git config user.email "350232297+odbcpp-agent[bot]@users.noreply.github.com"
   ```
   When **continuing an explicit handoff** or coordinator recovery, resume and preserve the existing agreed `codex/` branch and worktree, rather than restarting from main.
2. **Test-Driven Changes & Control Flow:**
   - For bug fixes, write a failing GoogleTest first.
   - If changing C++ control flow bodies containing GoogleTest macros, enclose all bodies in braces and verify with `python3 tools/development/check_changed_gtest_control_flow.py --base origin/main`.
3. **Focused Local Testing:**
   Run focused local tests relevant to the modified code. Do not mandate running the full matrix of both products locally; required CI checks provide the canonical qualification evidence per ADR 0001 Decision 5.
4. **Safe Worktree Cleanup:**
   After the issue's acceptance criteria are fully satisfied and no active processes or evidence readers need the directory, prune the worktree:
   ```bash
   git worktree remove ../worktrees/issue<number>
   git worktree prune
   ```

---

## 7. Pull Request & Independent Review Protocol

### Opening the PR
Open the PR as `odbcpp-agent[bot]` with description covering: What, Why, Change, Tests, Review, and Open debt.

### Substantive Review Independence
Output-shape checks (such as distinct review IDs or timestamps) are secondary heuristics and not proof of independence. True independence requires:
1. **Separation of Roles:** The author session must never review its own PR.
2. **Correctness Review:** An independent session acting as `odbcpp-reviewer[bot]` reads the code and evidence, evaluates correctness, architecture placement, and test validity, and submits a review starting with `[correctness reviewer]`.
3. **Red-Team Review (High-Risk Categories):** PRs touching `security`, `auth`, `protocol`, `transactions`, `process`, or gate files require a second independent red-team reviewer session. The red-team reviewer must operate in a blind context (not seeing the correctness review), actively attempt to break the change, and submit a review starting with `[red-team reviewer]`.
4. **Exact Head Approval:** Approvals cover the exact head commit SHA. Any new push requires re-review.
5. **Merge Gates:**
   - Product PRs can be merged by agents once independent reviews approve and required CI checks pass.
   - Changes touching gate files (`.github/CODEOWNERS`, `AGENTS.md`, `CLAUDE.md`, `.cursorrules`, `docs/AGENT_WORKFLOW.md`, `docs/adr/`, `.agents/`, `tools/`, `tests/security/`, `cmake/LiveSuite.cmake`, `DEVELOPMENT_WORKFLOW.md`) require repository owner (`@vahid110`) sign-off per `CODEOWNERS`.

---

## 8. Appendix: GitHub Projects Setup (Optional Reference)

If the repository owner chooses to maintain a GitHub Projects v2 Kanban view linked to `vahid110/odbcpp`:
1. Create a Project at `https://github.com/users/vahid110/projects/new` named `ODBCPP Development`.
2. Link repository `vahid110/odbcpp`.
3. Configure `Status` single-select options: `Ready`, `Working`, `Review`, `CI / Merge`, `Blocked`, `Done`.
4. Add custom text field: `Worker Identity`.
5. *Note on Automation:* Do not manually duplicate linked PRs or timestamps that GitHub already tracks automatically. The board is a visualization aid; GitHub Issues and PRs remain the operational source of truth.
