#!/bin/bash
# Bind each provider receipt to the tested workflows, helpers and policy.
set -euo pipefail
cd "$(dirname "$0")/../.."
find .github/review -type f ! -name '*.pyc' | LC_ALL=C sort |
  { cat; ls .github/workflows/agent-review.yml .github/workflows/agent-review-run.yml \
            .github/workflows/agent-review-canary.yml .github/workflows/agent-review-proof.yml AGENTS.md docs/adr/0001-development-process.md; } |
  while read -r f; do printf '%s\n' "$f"; cat "$f"; done | sha256sum | cut -c1-16
