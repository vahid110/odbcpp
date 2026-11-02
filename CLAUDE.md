# Agent Instructions for Claude Code

This repository follows the canonical autonomous agent development workflow.
Before claiming an issue or executing commands, read:
1. [AGENTS.md](AGENTS.md) - Architecture placement, evidence requirements, and reviewer invariants.
2. [docs/AGENT_WORKFLOW.md](docs/AGENT_WORKFLOW.md) - Operational rules, bot authentication, worker identity, claiming, and handoffs.

## Essential Invariants for Claude Sessions
- **Worker Identity:** Identify as `claude/<host>/<session>` (e.g. `claude/mac-arm64/<session-id>`).
- **Claiming & Recovery:** Follow canonical claiming, handoff, and coordinator recovery rules in `docs/AGENT_WORKFLOW.md`. GitHub Issues and PRs are authoritative.
- **Bot Identities:** Use `odbcpp-agent[bot]` for authoring and `odbcpp-reviewer[bot]` for reviews (`GH_TOKEN=$(~/.config/.../token.sh)`). Never use personal GitHub credentials.
- **Worktrees:** Work exclusively in isolated `codex/` branches in their own worktree: create a fresh branch from latest `main` for new tasks, or resume the existing agreed branch/worktree when continuing an explicit handoff or coordinator recovery.
- **Testing & Quality:** Write a failing test first for bug fixes. Ensure control flow bodies containing GoogleTest macros have braces.
- **Review Gate:** Attach exact local test counts to PRs. Independent review (and blind red-team review for high-risk categories) is required before merge. Changes touching gate files require repository owner approval.
