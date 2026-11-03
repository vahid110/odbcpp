#!/bin/bash
# Writes runs.json: every "agent review run (<role>)" check run the reviewer
# App recorded on any commit of the PR. Review jobs record one before the
# model starts, so timeouts, cancellations and failures count too; check runs
# cannot be deleted, and only the reviewer App (whose key only main's
# agent-review environment holds) can create them. Needs GH_TOKEN with
# checks: read and pull-requests: read, REPO and PR_NUMBER.
set -euo pipefail
gh api --paginate "repos/$REPO/pulls/$PR_NUMBER/commits" --jq '.[].sha' > pr-commits.txt
while read -r sha; do
  gh api --paginate "repos/$REPO/commits/$sha/check-runs" \
    --jq '.check_runs[] | select(.name | startswith("agent review run (")) | {name, started_at, app_id: .app.id}'
done < pr-commits.txt | jq -s . > runs.json
echo "Recorded review runs: $(jq length runs.json)"
