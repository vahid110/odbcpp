#!/bin/bash
# Writes runs.json: every "agent review run (<role>) #<PR>" check run the
# reviewer App recorded for this PR. Review jobs record one before the model
# starts, so timeouts, cancellations and failures count too. Every run is
# recorded on one fixed commit, the repository's first commit (RUN_ANCHOR),
# so amending or force-pushing the PR does not drop earlier runs. Check runs
# cannot be deleted, and only the reviewer App (whose key only main's
# agent-review environment holds) can create them; the query is filtered to
# that App. Needs GH_TOKEN with checks: read, REPO and PR_NUMBER.
#
# Every review run of every PR lands on that one commit. If GitHub ever limits
# the check runs a commit can hold, recording fails and the review job stops
# before the model starts, so reviews stop rather than run unbudgeted. The
# fallback is a new anchor: commit a new SHA to .github/review/run-anchor
# (which restarts every PR's 24-hour count).
set -euo pipefail
anchor=$(cat .github/review/run-anchor)
app_id=$(cat .github/review/reviewer-app-id)
: > runs.jsonl
for role in correctness red-team; do
  gh api --paginate --method GET "repos/$REPO/commits/$anchor/check-runs" \
    -f filter=all -f "app_id=$app_id" -f "check_name=agent review run ($role) #$PR_NUMBER" \
    --jq '.check_runs[] | {name, started_at, app_id: .app.id}' >> runs.jsonl
done
jq -s . runs.jsonl > runs.json
echo "Recorded review runs for #$PR_NUMBER: $(jq length runs.json)"
