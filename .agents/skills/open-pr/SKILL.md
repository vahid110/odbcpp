---
name: open-pr
description: Commit, push and open an ODBCPP pull request as odbcpp-agent[bot] with the owner as commit author, then verify the PR's author. Use whenever you commit, push, open or update a PR, or post an issue or PR comment in this repository.
---

# Open a PR as odbcpp-agent[bot]

AGENTS.md and ADR 0001 decision 10 set the rules. This skill gives the order
that keeps them. Each step exists because skipping it once went wrong; #185
was opened as the owner's account and had to be closed and re-opened as #187.

Never use the owner's GitHub login (`vahid110`). Never print the token, read
`app.pem`, put a token in a remote URL, or write `user.*` to the shared
`.git/config`.

## 1. Worktree and identity (once per worktree)

Work in your own worktree on a new `codex/` branch from current `origin/main`,
for one issue:

```sh
git fetch origin
git worktree add -b codex/<topic> ~/.codex/worktrees/<topic>/odbcpp origin/main
cd ~/.codex/worktrees/<topic>/odbcpp
# Without this extension, --worktree writes the shared .git/config instead.
[ "$(git config --get extensions.worktreeConfig)" = true ] || { echo "worktreeConfig is off; stop" >&2; exit 1; }
git config --worktree committer.name 'odbcpp-agent[bot]'
git config --worktree committer.email '350232297+odbcpp-agent[bot]@users.noreply.github.com'
git var GIT_AUTHOR_IDENT     # must be: vahid saber <vahid.saber78@gmail.com>
git var GIT_COMMITTER_IDENT  # must be: odbcpp-agent[bot] <350232297+...>
```

If the author is not the owner, run
`git config --show-origin --show-scope --get-all user.name`. A `user.*`
written to the shared config by another session changes every worktree.
Restore it to the owner (`git config --local user.name 'vahid saber'`, and the
same for `user.email`). Say so in your report. Never set `user.*` to the bot.

## 2. Commit

Make one coherent commit, or a few. Use a subject in the imperative and a body
that says why. End the message with the attribution trailer your harness gives
(for Claude Code: `Co-Authored-By: Claude ... <noreply@anthropic.com>`). For a
bug fix, commit the failing test first, so the red run is reproducible from
history (see the `pr-evidence` skill).

## 3. Token: get it, assert it, then use it in separate commands

`gh` silently falls back to the owner's keyring login when `GH_TOKEN` is
empty. So every GitHub write starts from an asserted token, in a script that
stops at the first failure:

```sh
set -euo pipefail
T=$(~/.config/odbcpp-agent/token.sh)
[[ "$T" == ghs_* ]] || { echo "no bot token" >&2; exit 1; }
G=$(mktemp -d)   # an empty gh config: the owner's keyring login is unreachable
```

- Pass it per command: `GH_CONFIG_DIR="$G" GH_TOKEN="${T:?no bot token}" gh ...`.
  `${T:?}` stops the command if the token is empty. The empty config means
  `gh` fails instead of acting as the owner. Never `export` the token into a
  long shell, and never echo it.
- Each Bash call of an agent harness starts a fresh shell, so put this block
  at the top of every script that writes. The snippets below assume it ran in
  the same script.
- Never put a `gh` write after `;`, or after a step that can fail without
  stopping the script (`cmd || true`, a failed `&&` link followed by `;`).
  The write must run only if the token step succeeded.
- The token lasts one hour. Get a new one for each batch of writes.

## 4. Push

```sh
GH_TOKEN="${T:?no bot token}" git -c credential.helper= \
    -c 'credential.helper=!f() { echo username=x-access-token; echo "password=${GH_TOKEN:?}"; }; f' \
    push -u origin codex/<topic>
```

Keep the helper in single quotes. The helper's shell then reads the token
from its environment when git asks. In double quotes, the token would be
expanded into git's arguments, where any local process can read it (`ps`).
`credential.helper=` first clears the keychain helper, so the token is never
stored.

Use `--force-with-lease` only to update your own PR branch after a rebase. Never
force-push `main` or someone else's branch.

## 5. Create the PR

Write the body to a file in your scratchpad, not inline. Sections, from
AGENTS.md:

```markdown
Closes #<issue>

## What
## Why
## Change
## Tests
<the evidence from the pr-evidence skill: exact counts and names per product,
red-before-green for a fix, log and JUnit paths, CI runs for this head>
## Review
<which reviewers this needs; owner approval if it touches CODEOWNERS paths>
## Open debt
<everything not run or not covered, listed, not implied>
```

End the body with the PR attribution line your harness gives. Then, in the
same asserted script:

```sh
GH_CONFIG_DIR="$G" GH_TOKEN="${T:?no bot token}" gh pr create -R vahid110/odbcpp --base main \
  --head codex/<topic> --title "<title>" --body-file <scratchpad>/pr-body.md
```

`gh pr create` uses GitHub's GraphQL API, which sometimes fails with a 502 or
"Something went wrong". Before retrying, check that no PR was created:
`gh api "repos/vahid110/odbcpp/pulls?head=vahid110:codex/<topic>&state=all" --jq length`
must print `0`. If GraphQL keeps failing, create the PR through REST:

```sh
jq -n --rawfile b <scratchpad>/pr-body.md \
  '{title: "<title>", head: "codex/<topic>", base: "main", body: $b}' > <scratchpad>/pr.json
GH_CONFIG_DIR="$G" GH_TOKEN="${T:?no bot token}" gh api -X POST repos/vahid110/odbcpp/pulls --input <scratchpad>/pr.json \
  --jq '.number, .user.login'   # must print odbcpp-agent[bot]
```

Do not add the high-risk labels yourself (`security`, `auth`, `protocol`,
`transactions`, `process`). The reviewer assigns them.

## 6. Verify the author, every time

```sh
GH_CONFIG_DIR="$G" GH_TOKEN="${T:?no bot token}" gh pr view <n> -R vahid110/odbcpp --json author,commits \
  --jq '.author.login, (.commits[] | .authors[].email)'
```

The REST form, if GraphQL is failing:
`gh api repos/vahid110/odbcpp/pulls/<n> --jq .user.login` must print
`odbcpp-agent[bot]`, and `gh api repos/vahid110/odbcpp/pulls/<n>/commits
--jq '.[].commit.author.email'` must print the owner's email.

- The PR author must be `app/odbcpp-agent`. If it is `vahid110`, stop and tell
  the human. The owner cannot approve their own PR. Ask the human to close it,
  then open a new PR as the bot.
- Each commit author must be the owner's email.

## 7. Owned files and the next step

If the diff touches a path in `.github/CODEOWNERS`, the PR waits for the
owner's approval. Say so in the PR body and in your report. Otherwise the
reviewers and the required checks decide. Merge only as AGENTS.md says: every
reviewer's latest review approves the head commit, and every required check is
green. Never merge your own PR on a review of an older commit.

Comments and issues follow steps 3 and 6: an asserted token, one write per
command, and `--body-file` for anything longer than a line.
