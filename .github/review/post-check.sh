#!/bin/bash
# Posts the required "agent review" check run for $HEAD_SHA.
# Usage: post-check.sh <success|failure|pending> <summary>
# Needs GH_TOKEN (reviewer App, checks: write), REPO and HEAD_SHA.
set -euo pipefail
conclusion=$1 summary=$2
if [[ "$conclusion" == pending ]]; then
  body=$(jq -n --arg sha "$HEAD_SHA" --arg s "$summary" \
    '{name: "agent review", head_sha: $sha, status: "in_progress", output: {title: $s, summary: $s}}')
else
  body=$(jq -n --arg sha "$HEAD_SHA" --arg c "$conclusion" --arg s "$summary" \
    '{name: "agent review", head_sha: $sha, status: "completed", conclusion: $c, output: {title: $s, summary: $s}}')
fi
gh api -X POST "repos/$REPO/check-runs" --input - <<< "$body" > /dev/null
echo "agent review: $conclusion ($summary) for ${HEAD_SHA:0:7}"
