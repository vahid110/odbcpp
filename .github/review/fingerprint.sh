#!/bin/bash
# Prints the fingerprint of the reviewer's tested configuration: its tool
# rules and the pinned claude-code-action. The agent review runs only while
# the AGENT_REVIEW_ENABLED variable equals this value, which the canary
# prints when it passes.
set -euo pipefail
cd "$(dirname "$0")/../.."
{ cat .github/review/claude-args.txt
  grep -ho 'anthropics/claude-code-action@[0-9a-f]*' \
    .github/workflows/agent-review-run.yml .github/workflows/agent-review-canary.yml | sort -u
} | sha256sum | cut -c1-16
