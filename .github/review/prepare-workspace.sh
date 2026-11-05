#!/bin/bash
# Makes the workspace safe for the reviewer, in both the review and the
# canary: removes every symbolic link (a link could point at /proc or the
# runner's files) and all agent configuration, at any depth, outside .git.
set -euo pipefail
cd "${1:-.}"
echo "Links removed: $(find . -path ./.git -prune -o -type l -print | wc -l)"
# -exec, not -delete: GNU find refuses -delete together with -prune.
find . -path ./.git -prune -o -type l -exec rm -f {} +
rm -rf pr-head/.git
find . -path ./.git -prune -o \( -name .claude -o -name .claude.json -o -name .mcp.json \
       -o -name CLAUDE.md -o -name CLAUDE.local.md \) -prune -print -exec rm -rf {} +
