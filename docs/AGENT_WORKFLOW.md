# Canonical Agent Workflow and GitHub Projects Specification

This document provides detailed operational instructions for autonomous and human agents working on `vahid110/odbcpp`. It implements the work-tracking and coordination decisions of [ADR 0001](adr/0001-development-process.md) and supplements [AGENTS.md](../AGENTS.md).

---

## 1. Core Principles

1. **GitHub is the Shared Source of Truth:**
   - All tasks, stages, assignments, progress reports, and blockers live on GitHub (Issues, Pull Requests, and the GitHub Project board).
   - Local files, private handoff notes, or unpushed scratchpads are never coordination mechanisms. If it is not on GitHub, it does not exist for other agents.
2. **Standard Tools over Custom Machinery:**
   - Coordination uses standard GitHub Issues, Projects v2, Pull Requests, and CI status checks. No custom daemon, central orchestration server, or custody ledger is required.
3. **Strict Attribution & Zero Credential Leaks:**
   - Automated agents act strictly under dedicated GitHub App bot identities (`odbcpp-agent[bot]` for authoring, `odbcpp-reviewer[bot]` for reviewing).
   - Personal credentials must never be used by agents, and bot tokens/private keys must never be committed or logged.

---

## 2. Worker Identity Specification

To coordinate multiple concurrent agents across different hosts, tools, and platforms without collision, every worker session must identify itself using the canonical tuple:

```
<tool>/<host>/<session>
```

> [!NOTE]
> The components `<tool>`, `<host>`, and `<session>` must contain only alphanumeric characters, hyphens, and dots (no forward slashes, colons, or whitespace) to guarantee unambiguous slash-delimited parsing across automated tooling.

### Components

- **`<tool>`**: The client agent runtime or driving interface:
  - `codex` (OpenAI Codex CLI / ChatGPT Codex)
  - `agy` (Google Antigravity CLI / IDE)
  - `claude` (Anthropic Claude Code CLI)
  - `copilot` (GitHub Copilot Agent)
  - `cursor` (Cursor IDE agent)
  - `human` (Direct human operator)
- **`<host>`**: The operating platform and machine architecture:
  - `mac-arm64` (Apple Silicon macOS)
  - `mac-x86` (Intel macOS)
  - `win-x64` (Native Windows 64-bit)
  - `wsl2-ubuntu` (Windows Subsystem for Linux - Ubuntu)
  - `linux-x64` (Native Linux 64-bit)
  - `remote-<name>` (Cloud or CI host)
- **`<session>`**: Unique session identifier, terminal PID, or conversation hash (e.g., `433005a4`, `pid-81204`, `run-20261102`).

### Examples

- `agy/mac-arm64/433005a4`
- `codex/win-x64/session-88`
- `claude/wsl2-ubuntu/worker-01`
- `human/mac-arm64/vahid110`

---

## 3. GitHub Projects Tracking & Board Structure

Work is tracked on a GitHub Project (Projects v2) linked to `vahid110/odbcpp`.

### Kanban Stages (Columns)

| Stage | Definition | Entry Condition |
| :--- | :--- | :--- |
| **Ready** | Backlog item ready for implementation | Issue created with complete acceptance criteria; unassigned. |
| **Working** | Under active implementation | Claimed by an agent worker (`Worker Identity` set); isolated `codex/` branch and worktree created. |
| **Review** | Implementation complete; undergoing review | Commits pushed to branch; PR opened as `odbcpp-agent[bot]`; local test evidence attached; reviewers requested. |
| **CI / Merge** | Reviews approved; awaiting CI / gate approval | `[correctness reviewer]` (and `[red-team reviewer]` if high-risk) approved exact PR head commit. Awaiting green CI and owner sign-off if touching protected paths. |
| **Blocked** | Progress halted | Explicit blocker comment posted detailing failed operation, error output, and minimum resolution needed. |
| **Done** | Completed and closed | PR merged into `main`; issue closed; worktree pruned. |

### Tracking Fields

- **Title / Item:** The GitHub Issue (or PR).
- **Status:** Single-select dropdown matching the six stages above.
- **Worker Identity:** Text field containing `<tool>/<host>/<session>`.
- **Linked PR:** Link or number of the active pull request.
- **Last Update:** ISO timestamp or date of the latest status comment / commit.

---

## 4. Concurrency, Claiming, and Handoff Rules

### Claiming Work

1. An agent may only claim an issue in the **Ready** column that has clear acceptance criteria.
2. Before modifying code, the agent must:
   - Post a comment on the GitHub issue with its `Worker Identity`, target branch name, baseline commit SHA, and initial approach.
   - Update the project item's **Status** to `Working` and set **Worker Identity** (or post the stage comment on the issue if board mutation is restricted).
   - Create a fresh `codex/` branch from current `origin/main` in an isolated git worktree.
3. **No Duplicate Claims:** An agent must NEVER claim or begin work on an issue that is already in `Working` or has an active worker identity.

### Stale Activity Protocol

> [!IMPORTANT]
> **Stale activity is NEVER permission to take over.**
> If an issue or branch has had no commits or updates for hours or days, an agent must NOT assume the task is abandoned or overwrite the branch. A human operator must explicitly reassign the issue, or the prior worker must post a release comment, before another session may claim it.

### Reporting Blockers

If an agent encounters a blocking condition (missing GitHub API permissions, network sandbox restrictions, compiler bugs, missing credentials, or architectural ambiguities):
1. Transition the project item status to **Blocked** (or post a blocker comment on the issue/PR).
2. Immediately post a structured blocker comment on the issue or PR containing:
   - **Failed Operation:** The exact command or API call that failed.
   - **Exact Output / Error:** Full error text or status code (redacting any credentials).
   - **Smallest Remediation:** The minimal permission grant, file change, or human decision required to proceed.
3. **Credential Scrubbing:** Before posting any error output or log snippet to an issue, PR, or comment, sanitize and redact any token pattern (`ghs_[A-Za-z0-9_]+`, `ghp_[A-Za-z0-9_]+`, JWTs, private keys, or passwords). Never include URLs with embedded access tokens in error reports.
4. Stop calling tools or idling; do not invent mock successes or switch identities.

### Handoff Protocol

When handing off work between sessions, tools, or machines:
1. Ensure all local progress is committed and pushed to `origin/codex/<branch>`.
2. Post a structured handoff comment on the issue and PR:
   - **Head Commit:** Exact commit SHA pushed to origin.
   - **Local Test Results:** Passed, skipped, failed tests with names and log paths.
   - **Acceptance Criteria Progress:** Checklist of satisfied vs remaining criteria.
   - **Next Action:** Concrete next steps for the incoming session.
3. **Unlocking Handoffs:** To resolve handoff claiming deadlocks, the departing worker resets the issue stage on GitHub to **Ready** (with note `Ready (Handoff)`), which unlocks the task so that incoming workers are authorized to claim it without violating duplicate-claim invariants.
4. The next worker claims the handoff by posting its own `Worker Identity` in a comment and updating the **Worker Identity** field on the board.

---

## 5. Bot Authentication & Cross-Platform Git Commands

Automated agents act only as `odbcpp-agent[bot]` for authoring and `odbcpp-reviewer[bot]` for reviewing. Never use the repository owner's personal identity.

### Authentication Scripts

The machine must have installation token generator scripts:
- `~/.config/odbcpp-agent/token.sh` (or `.ps1`)
- `~/.config/odbcpp-reviewer/token.sh` (or `.ps1`)

These scripts use a private GitHub App `.pem` key locally to sign a JWT and request a short-lived (1-hour) installation access token from GitHub. The private key never leaves the local machine.

### Cross-Platform Token Generation

#### macOS / Linux / WSL (Bash/Zsh)
```bash
# Obtain agent author token
export GH_TOKEN=$(~/.config/odbcpp-agent/token.sh)

# Strict token verification: prevent silent fallback to personal user accounts
[ -n "$GH_TOKEN" ] || { echo "Error: GH_TOKEN is empty" >&2; exit 1; }
gh api /installation/repositories --jq '.total_count' >/dev/null || {
  echo "Error: GH_TOKEN is not a valid GitHub App installation token" >&2
  exit 1
}
```

#### Windows Native (PowerShell)
```powershell
# Obtain agent author token via PowerShell script or WSL execution
$env:GH_TOKEN = & "$HOME\.config\odbcpp-agent\token.ps1"
# Or invoke bash via Git Bash / WSL:
# $env:GH_TOKEN = (bash -c '~/.config/odbcpp-agent/token.sh')

# Strict token verification: prevent silent fallback to personal user accounts
if (-not $env:GH_TOKEN) { Write-Error "GH_TOKEN is empty"; exit 1 }
gh api /installation/repositories --jq '.total_count' | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Error "GH_TOKEN is not a valid GitHub App installation token"
  exit 1
}
```

> [!WARNING]
> Do NOT use `gh api /repos/vahid110/odbcpp` without verifying `$GH_TOKEN`. If `$GH_TOKEN` is unset or empty, `gh` silently falls back to the host machine's personal user credentials, violating repository bot identity separation.

### Git Author Configuration

In every worktree, configure the bot identity so commits are properly attributed:

```bash
git config user.name "odbcpp-agent[bot]"
git config user.email "350232297+odbcpp-agent[bot]@users.noreply.github.com"
```

### Authenticated Git Push (Cross-Platform)

> [!CAUTION]
> Never embed the access token into the remote URL (e.g. `https://x-access-token:...@github.com`). If git encounters a network drop or rejected push, it echoes the full remote URL containing the token to stderr, creating a risk of accidental token leakage.

Inject the token dynamically via HTTP authorization headers instead:

#### Bash / Zsh (macOS / Linux / WSL):
```bash
git -c http.extraheader="AUTHORIZATION: basic $(printf 'x-access-token:%s' "$GH_TOKEN" | base64)" \
  push origin codex/<branch-name>
```

#### PowerShell (Windows Native):
```powershell
$auth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("x-access-token:$($env:GH_TOKEN)"))
git -c http.extraheader="AUTHORIZATION: basic $auth" push origin codex/<branch-name>
```

---

## 6. Development & Worktree Workflow

1. **Worktrees:**
   Always create an isolated worktree from current `origin/main`:
   ```bash
   git fetch origin main
   git worktree add -b codex/issue<number>-<slug> ../worktrees/issue<number> origin/main
   cd ../worktrees/issue<number>
   git config user.name "odbcpp-agent[bot]"
   git config user.email "350232297+odbcpp-agent[bot]@users.noreply.github.com"
   ```
2. **Test-Driven Changes:**
   - For bug fixes, write a failing GoogleTest first. Show that it fails before applying the fix.
   - For C++ control flow changes containing GoogleTest macros, enclose all control bodies in braces:
     ```bash
     python3 tools/development/check_changed_gtest_control_flow.py --base origin/main
     ```
3. **Local Testing:**
   Run relevant unit tests for `POSTGRESQL` and `REDSHIFT` profiles locally before opening a PR.
4. **Worktree Teardown & Cleanup:**
   Once the task is completed and the PR is merged or closed, prune the isolated worktree:
   ```bash
   git worktree remove ../worktrees/issue<number>
   git worktree prune
   ```

---

## 7. Pull Request & Independent Review Protocol

### Opening the PR

Open the PR using `odbcpp-agent[bot]`:
```bash
GH_TOKEN=$(~/.config/odbcpp-agent/token.sh) gh pr create \
  --title "<category>: <summary> (#<issue>)" \
  --body-file pr_description.md
```

PR description template:
- **What:** Concise summary of what was done.
- **Why:** Context and issue reference.
- **Change:** Breakdown of modified components and files.
- **Tests:** Exact test counts (passed, skipped, failed with names) and log paths.
- **Review:** Requested reviewers and risk category.
- **Open debt:** Explicit list of anything unverified or open.

### Independent Review Process

1. **Reviewer Separation:**
   - The agent that authored the PR must NOT review it.
   - A separate reviewer agent session performs the review using `odbcpp-reviewer[bot]`.
2. **Correctness Review:**
   - Reads code and attached evidence. Verifies that claims match evidence.
   - Checks edge cases, architectural placement, security, and assertions.
   - Ranks findings: `blocking`, `should-fix`, `nit` with `file:line`.
   - Posts verdict using `odbcpp-reviewer[bot]`:
     ```bash
     GH_TOKEN=$(~/.config/odbcpp-reviewer/token.sh) gh pr review <pr-number> \
       --approve --body "[correctness reviewer] Verdict: APPROVE. ..."
     ```
3. **Red-Team Review (High-Risk Changes):**
   - Mandatory for PRs touching `security`, `auth`, `protocol`, `transactions`, `process`, or files under `tools/`, `.github/`, `docs/adr/`.
   - A second independent review session acts as `[red-team reviewer]`.
   - Attempts to break the change (hostile inputs, race conditions, privilege bypass, regression).
   - Posts verdict using `odbcpp-reviewer[bot]`:
     ```bash
     GH_TOKEN=$(~/.config/odbcpp-reviewer/token.sh) gh pr review <pr-number> \
       --approve --body "[red-team reviewer] Verdict: APPROVE. ..."
     ```
4. **Independence Invariants:**
   - Correctness and red-team reviews must have distinct review IDs, distinct timestamps, and non-identical review text bodies.
   - Reviews must be on the exact head commit SHA of the PR. Any new push invalidates past approvals and requires a new review.
5. **Merge Safeguards:**
   - Product PRs may be merged by agents once independent reviews approve and required CI checks pass.
   - Changes touching gate files (`.github/`, `AGENTS.md`, `CLAUDE.md`, `.cursorrules`, `docs/AGENT_WORKFLOW.md`, `docs/adr/`, `.agents/`, `tools/`, `tests/security/`, `cmake/LiveSuite.cmake`, `DEVELOPMENT_WORKFLOW.md`) wait for the repository owner (`@vahid110`)'s personal approval per `CODEOWNERS`.

---

## 8. GitHub Projects Manual Setup & Safe Automation

### Personal User Account Permission Boundaries

GitHub Projects (v2) on personal user accounts (e.g., `vahid110`) have specific permission constraints:
- GitHub App installation access tokens (server-to-server) cannot execute `createProjectV2` on personal user accounts (returns `GraphQL: odbcpp-agent[bot] does not have permission to create projects on ownerId ...`).
- Therefore, initial project board creation on a user account is performed by the human owner via the GitHub web UI.
- **Avoiding Mutation Deadlocks:** If direct GraphQL or CLI project card mutations fail due to GitHub App permission boundaries on personal accounts, agents must NOT treat this as a fatal blocker that halts development. Agents record the stage transition via a comment on the GitHub issue/PR and proceed with the development, testing, and review phases normally.

### Manual Setup Steps for the Repository Owner

1. Navigate to: `https://github.com/users/vahid110/projects/new`.
2. Choose **Board** (Kanban) template and name the project `ODBCPP Development`.
3. Link the repository:
   - Under Project Settings -> **Manage access** / **Linked repositories**, select `vahid110/odbcpp`.
4. Configure Stages in the `Status` field:
   - `Ready`
   - `Working`
   - `Review`
   - `CI / Merge`
   - `Blocked`
   - `Done`
5. Add Custom Fields:
   - `Worker Identity` (Type: Text)
   - `Linked PR` (Type: Text or linked pull request)
   - `Last Update` (Type: Date or Text)
6. Save the default Kanban view.

### Safe Status Automation & Importing Work

- When importing existing open issues and PRs into the board:
  - Do NOT infer active ownership. Place unworked open issues into **Ready** with an empty `Worker Identity`.
  - Only place items into **Working** if an active worker session has explicitly claimed them on GitHub.
  - Active PRs with reviews in progress are placed in **Review**.
- When status automation is active, agents update item fields via GraphQL API or `gh project item-edit` using authorized tokens. When direct API mutations are restricted, agents post explicit stage update comments on the issue/PR so that human operators or automation workflows can synchronize board state.
