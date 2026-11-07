#!/bin/bash
set -euo pipefail
mkdir review-context
# head_sha is the commit the start job chose and the prompt names,
# even if the pull request moved since (post then discards the result).
jq -e --arg h "$HEAD_SHA" --arg b "$BASE_SHA" '.head.sha == $h and .base.sha == $b' "$RUNNER_TEMP/pr-raw.json" > /dev/null
jq --arg h "$HEAD_SHA" '{number, title, body, user: .user.login, labels: [.labels[].name],
     base: .base.ref, head_sha: $h}' "$RUNNER_TEMP/pr-raw.json" > review-context/pr.json
# The exact diff of the reviewed commit against its merge base.
gh api -H "Accept: application/vnd.github.diff" \
  "repos/$REPO/compare/$BASE_SHA...$HEAD_SHA" > review-context/pr.diff
gh api --paginate "repos/$REPO/commits/$HEAD_SHA/check-runs" \
  --jq '.check_runs[] | {name, status, conclusion}' > review-context/checks.jsonl
# This role's own earlier reviews, so fixes can be checked; the
# other role's reviews stay hidden (independent reviews). The
# reviewed commit comes from each header, not commit_id, which
# GitHub re-points to the new head after a force-push (#210).
# gh's --jq uses Go regular expressions: no lookahead.
gh api --paginate "repos/$REPO/pulls/$PR_NUMBER/reviews" \
  --jq ".[] | select(.user.login == \"odbcpp-reviewer[bot]\")
            | select(.body | ltrimstr(\"**\") | startswith(\"[$ROLE reviewer]\"))
            | {reviewed_commit: ((.body | split(\"\\n\")[0] | capture(\" at (?<s>[0-9a-f]{40})($|[^0-9a-f])\") | .s) // \"unknown\"),
               state, body}" > review-context/previous-reviews.jsonl
ls -l review-context
