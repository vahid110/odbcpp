#!/bin/bash
# Posts the required "agent review" check run for $HEAD_SHA.
# Usage: post-check.sh <success|failure|pending> <summary>
#        post-check.sh   (reads check-status.txt written by review_gate.py)
# Needs GH_TOKEN (reviewer App, checks: write), REPO and HEAD_SHA.
set -euo pipefail
if [[ $# -eq 0 ]]; then
  { read -r conclusion; read -r summary; } < check-status.txt
else
  conclusion=$1 summary=$2
fi
if [[ "$conclusion" == pending ]]; then
  body=$(jq -n --arg sha "$HEAD_SHA" --arg s "$summary" \
    '{name: "agent review", head_sha: $sha, status: "in_progress", output: {title: $s, summary: $s}}')
else
  body=$(jq -n --arg sha "$HEAD_SHA" --arg c "$conclusion" --arg s "$summary" \
    '{name: "agent review", head_sha: $sha, status: "completed", conclusion: $c, output: {title: $s, summary: $s}}')
fi
gh api -X POST "repos/$REPO/check-runs" --input - <<< "$body" > /dev/null
echo "agent review: $conclusion ($summary) for ${HEAD_SHA:0:7}"
