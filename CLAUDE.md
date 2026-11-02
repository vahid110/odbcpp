# Agent Instructions for Claude Code

This repository follows the canonical autonomous agent development workflow.
Before claiming an issue or executing commands, read:
1. [AGENTS.md](AGENTS.md) - Architecture placement, evidence requirements, and reviewer invariants.
2. [docs/AGENT_WORKFLOW.md](docs/AGENT_WORKFLOW.md) - Operational rules, bot authentication, worker identity, claiming, and handoffs.

## Essential Invariants for Claude Sessions
- **Worker Identity:** Identify as `claude/<host>/<session>` (e.g. `claude/mac-arm64/<session-id>`).
- **Claiming:** GitHub is the shared source of truth. Check the issue and project board before starting; never claim an issue in `Working`, and never treat stale activity as permission to take over.
- **Bot Identities:** Use `odbcpp-agent[bot]` for PR authoring and `odbcpp-reviewer[bot]` for PR reviews (`GH_TOKEN=$(~/.config/.../token.sh)`). Never use personal GitHub credentials.
- **Worktrees:** Work exclusively in isolated `codex/` branches created from latest `main` in their own worktree.
- **Testing & Quality:** Write a failing test first for bug fixes. Ensure control flow bodies containing GoogleTest macros have braces.
- **Review Gate:** Attach exact local test counts to PRs. Independent review (and red-team review for high-risk categories) is required before merge. Changes touching gate files require repository owner approval.
