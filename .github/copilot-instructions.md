# GitHub Copilot Instructions

This repository enforces a canonical development process for all agents and contributors:
- [AGENTS.md](../AGENTS.md)
- [docs/AGENT_WORKFLOW.md](../docs/AGENT_WORKFLOW.md)
- [docs/adr/0001-development-process.md](../docs/adr/0001-development-process.md)

## Core Guidelines
- Operate on dedicated `codex/` branches in isolated worktrees created from latest `main`.
- Adhere to the canonical worker identity tuple: `copilot/<host>/<session>`.
- Use prescribed GitHub App bot tokens (`odbcpp-agent[bot]`), never personal credentials.
- Preserve existing tests and assertions.
- Ensure all changed control flow blocks with GoogleTest macros are enclosed in braces.
- PRs require independent agent reviews (and red-team review for high-risk categories).
