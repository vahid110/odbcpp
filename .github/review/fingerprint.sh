#!/bin/bash
# Prints the fingerprint of the reviewer's tested configuration: its tool
# rules, the review workflow that prepares its input, and the canary. The agent review runs only while
# the AGENT_REVIEW_ENABLED variable equals this value, which the canary
# prints when it passes.
set -euo pipefail
cd "$(dirname "$0")/../.."
cat .github/review/claude-args.txt .github/review/fingerprint.sh \
    .github/workflows/agent-review-run.yml .github/workflows/agent-review-canary.yml |
  sha256sum | cut -c1-16
