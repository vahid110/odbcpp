# Automated reviewer setup and recovery

The reviewer App publishes the correctness and red-team reviews for the exact
PR head and sets the protected `agent review` check. When its canary is green, Claude runs first for each
role. With only OpenAI qualified, each role goes directly to OpenAI. When its action fails with empty structured output, OpenAI gets one
attempt at that same role. A nonempty Claude response goes through the existing
verdict validation, including when it requests changes. Cancellation, invalid
responses and absent approvals keep the merge gate closed.

## Credentials and data

Use GitHub's repository **Settings → Environments → agent-review**. Keep its
deployment branch restricted to `main`. Leave the existing
`CLAUDE_CODE_OAUTH_TOKEN` and `ODBCPP_REVIEWER_PRIVATE_KEY` there. Add
`OPENAI_REVIEW_API_KEY` as an **environment secret**. For CI, use an API project's service account
authorized for review traffic, the pinned model and permission to create
Responses. Enter the key directly in GitHub or through an approved secret
manager. [OpenAI API keys and service accounts](https://developers.openai.com/api/docs/guides/api-keys)

The adapter sends the complete supported text diff, PR description and labels,
check results, the same role's prior reviews and the trusted base review
instructions to the OpenAI Responses API. It requests no tools and disables
response storage. It uses one request per fallback role with bounded input,
output and time; excessive diffs or unusable responses leave the review open.
Account-level data policies and API charges still apply. Only the fallback
request step receives this key as `OPENAI_API_KEY`. The reviewer App posts
validated results separately.

## Enable a reviewed configuration

1. Merge the workflow change through the **existing** review and CODEOWNERS
   protections. Review workflows and secrets come from trusted `main`; the PR
   that introduces a fallback cannot use its proposed code to approve itself.
2. In **Actions → agent review canary → Run workflow**, select `main`.
   The original Claude probe attempts file, tool and configuration escapes
   using planted values. Its judge checks the execution trace. In separate
   jobs the production router receives a simulated Claude failure with no
   result, and the **real OpenAI adapter** reviews two synthetic full diffs.
   A spelling correction must earn approval; an ODBC trace that prints a
   database password must receive a change request that identifies the leak.
   The canary does not post a PR review.
3. Set the repository Actions variable `AGENT_REVIEW_ENABLED` to
   `provider-canaries-v1` to opt in to automatic per-provider activation.
   An empty variable disables new reviews. Old joint fingerprints require
   migration. Each provider records its own result with the reviewer App on
   the fixed check-run anchor. OpenAI requires both test fixtures. Each
   provider also requires the offline review tests. A newer failed receipt
   disables only its provider. Changes to the shared fingerprint require
   fresh proofs. Canary jobs can finish independently.
4. Start a new review event on the PR against current main. If only OpenAI
   passed, both roles go directly to the bounded API jobs. If both passed,
   Claude goes first and OpenAI can recover an empty failed Claude action.
   A valid change request stands. The reviewer App still needs both role
   approvals on a product PR's exact head before merge.

Each provider first creates an in-progress receipt. An interrupted test or
failed publication therefore leaves it disabled. Start a fresh canary dispatch
for recovery. Reviewers recheck the receipt before calling the provider and
before publishing its verdict.

## Correct evidence on an unchanged head

After a role requests changes, the repository owner or the coordinator App (GitHub bot user ID
350232297, currently `odbcpp-agent[bot]`) can
request one additional verdict from that role on that commit.

1. **Publish the response** by submitting a GitHub **Comment** review on the blocked commit. Put the full head SHA on line one and `Review response #123: <correction and evidence>` on line two, replacing 123 with the blocking review ID. The submitting account must be the one that will apply the label. GitHub's response review submission time must be strictly later than the objection.
2. Copy the response's second line into the PR description and add `Response record #123: 321` on a separate line, replacing 321 with the GitHub-generated ID of the **submitted response review**. The description is a copy for the reviewer. The submitted review is the dated record of publication.
3. Apply `reconsider:correctness` or `reconsider:red-team`. Eligibility requires both the referenced record and its exact response text in the immutable label-event payload. The submitted response must strictly predate the PR's `updated_at` in that payload. This is a conservative cutoff: queue delays cannot admit a response published later. If timestamps coincide at GitHub’s one-second precision, update the description with the response reference in a later second, then reapply the label.

Create the label if necessary. Removing and reapplying a label after a
second verdict does not grant another review. A new commit uses the normal
review process. Daily budgets and provider activation still apply. The
original objection remains visible and blocks merge until the role approves.

OpenAI records a separate single-use reservation for an authorized
reconsideration. Both reservations are scoped to the same role, PR and commit.
If GitHub accepts a verdict and a later label or check update fails, rerun the
failed job. It recognizes the published review from that workflow run and
repairs the remaining updates without submitting the verdict again.
