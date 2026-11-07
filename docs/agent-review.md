# Automated reviewer setup and recovery

The reviewer App publishes the correctness and red-team reviews for the exact
PR head and sets the protected `agent review` check. Claude runs first for each
role. When its action fails with empty structured output, OpenAI gets one
attempt at that same role. A nonempty Claude response goes through the existing
verdict validation, including when it requests changes. Cancellation, invalid
responses and absent approvals keep the merge gate closed.

## Credentials and data

Use GitHub's repository **Settings → Environments → agent-review**. Keep its
deployment branch restricted to `main`. Leave the existing
`CLAUDE_CODE_OAUTH_TOKEN` and `ODBCPP_REVIEWER_PRIVATE_KEY` there. Add
`OPENAI_REVIEW_API_KEY` as an **environment secret** for an OpenAI API project
authorized for review traffic and the workflow's pinned model. A ChatGPT login
does not supply API billing or an API key. Give the key only the endpoint access
needed to create Responses. Enter it directly in GitHub or through an approved
secret manager; keep its value out of PRs, shell arguments and chat.

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
3. Only after **all** these jobs pass, copy the fingerprint from the final
   job to the repository Actions variable `AGENT_REVIEW_ENABLED`. Confirm
   the run tested the current configuration on `main`. Any later change to
   the protected review configuration requires a fresh passing canary.
4. For an existing PR, use the documented failed-jobs rerun on its review
   workflow, and confirm it is using the enabled configuration on `main`.
   A rerun of an old workflow uses its original workflow revision. If that
   revision predates the fallback, use a new review event (for example mark
   the PR draft, then ready again) to select the current base workflow.
   Check both posted verdicts refer to the current PR head and the protected
   `agent review` check is green before merging.

## When a provider is unavailable

If Claude's weekly allowance expires **before this change lands**, the
original main-only workflow still owns the bootstrap PR's reviews. Wait for
verified Claude availability, then rerun the failed review jobs. If the
allowance expires **during the canary**, the two fallback jobs can still
demonstrate OpenAI availability. Their success does not produce an enablement
fingerprint by itself. After Claude recovers, rerun the failed canary jobs on
the same revision, or run a new complete canary if the configuration changed.
Never enable a fingerprint copied from an earlier configuration or computed
by hand.

Once the jointly tested configuration is enabled, an eligible Claude
failure can use OpenAI automatically. If a fallback fails, check the sanitized
step error, the API project's service access and the environment secret's
configuration. Verify recovery before retrying. A missing verdict is
different from a code finding; an existing change request needs a repair
and a review of the resulting head.
