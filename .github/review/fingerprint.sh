#!/bin/bash
# Prints the fingerprint of the reviewer's tested configuration: everything
# in .github/review/, the review workflows and the trusted policy inputs. The review runs
# only while the AGENT_REVIEW_ENABLED variable equals this value, which the
# canary prints when it passes, so any change switches the review off until
# the canary passes again.
set -euo pipefail
cd "$(dirname "$0")/../.."
find .github/review -type f ! -name '*.pyc' | LC_ALL=C sort |
  { cat; ls .github/workflows/agent-review.yml .github/workflows/agent-review-run.yml \
            .github/workflows/agent-review-canary.yml .github/workflows/agent-review-proof.yml AGENTS.md docs/adr/0001-development-process.md; } |
  while read -r f; do printf '%s\n' "$f"; cat "$f"; done | sha256sum | cut -c1-16
