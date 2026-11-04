# Provenance: agentic-actions-auditor

The files in this directory, apart from this one, are copied from Trail of
Bits' skills repository. All but one are unmodified. The exception is one
line of `SKILL.md`, described under "Local change".

- Repository: https://github.com/trailofbits/skills
- Commit: `711af095215ad4ecde5b90cd1d0124b3f1c4ca9e` (plugin version 1.3.2)
- Source paths:
  - `plugins/agentic-actions-auditor/skills/agentic-actions-auditor/SKILL.md`
  - `plugins/agentic-actions-auditor/skills/agentic-actions-auditor/references/*.md`
  - `LICENSE` (repository root)
- Authors: Emilio López and Will Vandevanter, Trail of Bits.
- Licence: Creative Commons Attribution-ShareAlike 4.0 International
  (`LICENSE`). These files, including the changed `SKILL.md`, stay under that
  licence. The rest of ODBCPP is unaffected.
- Copied on 2026-11-04 for #193. The author states that every file was read in
  full before it was copied. What can be checked is below: the upstream blob
  ids, the hashes and the scans.

Left out:
- `assets/trail-of-bits-mark.svg`: a logo. SVG can carry scripts, and the
  skill doesn't need it.
- `agents/openai.yaml`: Codex display metadata that only points at the logo.
- The plugin's `README.md` and `.claude-plugin/plugin.json`: marketplace
  packaging, not used here.

What it does: static review of `.github/workflows/*.yml` for paths by which
attacker-controlled input reaches an AI agent in CI. It reports findings and
changes no files. Its instructions use the shell only for read-only `gh api`
fetches in remote mode. That is advice to the model, not a limit.

## Local change

`SKILL.md` line 4: upstream has `allowed-tools: Read Grep Glob Bash`. Here it
is `allowed-tools: Read Grep Glob`.

In Claude Code, `allowed-tools` lets an active skill use those tools without a
permission prompt, and a bare `Bash` allows any command. This skill reads
workflow files that an attacker may control, and a prompt injection in them
could then run a shell command unasked. Even `Bash(gh api:*)` would allow
`gh api -X POST`. Without `Bash` in the list, `gh api` still works, but
through the session's normal permission prompt. Nothing else is changed.

## Known errata (upstream, not changed here)

`references/action-profiles.md` lines 170-173 label
`RESULT='${{ steps.inference.outputs.result }}'` inside `run:` as SAFE. It is
not safe. The expression is pasted into the script before bash parses it, so
output containing `'` breaks out of the quotes. That is vectors B and G of
this same skill. In this repository, AI output reaches a `run:` script only
through `env:` and a quoted `"$VAR"`, as `.github/workflows/agent-review-run.yml`
does. Never copy the upstream example.

## Verifying this copy

From this directory:

```sh
C=711af095215ad4ecde5b90cd1d0124b3f1c4ca9e
P=plugins/agentic-actions-auditor/skills/agentic-actions-auditor
U=$(mktemp)
gh api "repos/trailofbits/skills/git/trees/$C?recursive=1" \
  --jq '.tree[] | select(.type=="blob") | "\(.sha)  \(.path)"' > "$U"
# Every unmodified file's blob id is in the upstream tree at its path:
for f in LICENSE references/*.md; do
  u=$P/$f; [ "$f" = LICENSE ] && u=LICENSE
  grep -qxF "$(git hash-object "$f")  $u" "$U" && echo "ok $f" || echo "MISMATCH $f"
done
# SKILL.md: upstream's blob is the listed one, and only line 4 differs:
grep -qxF "efdeacdf470d5bd1d714ca231d6276f086214971  $P/SKILL.md" "$U" && echo "ok upstream SKILL.md"
gh api repos/trailofbits/skills/git/blobs/efdeacdf470d5bd1d714ca231d6276f086214971 \
  --jq .content | base64 -d | diff - SKILL.md
# The local hashes:
sed -n '/^## SHA-256/,$p' PROVENANCE.md | grep -E '^[0-9a-f]{64}  ' | shasum -a 256 -c
# Hidden content. Expected: the first scan lists only ./LICENSE (the licence's
# typographic quotes) and ./PROVENANCE.md (the "ó" in López); the second
# prints nothing.
LC_ALL=C grep -rl '[^[:print:][:space:]]' .
grep -rl --exclude=PROVENANCE.md '<!--' .
```

## Updating

Never edit these files in place, apart from re-applying the local change. To
update:
1. Pick a new upstream commit by its full 40-character SHA. Never use a branch,
   a tag or a short SHA.
2. Read the upstream diff in full. Call out in the PR any change to frontmatter
   (`allowed-tools`, `description`) and any new executable, script, hook, MCP
   configuration or `agents/` file.
3. Copy the files again, and re-apply the local change if it is still needed.
4. Update the commit, the blob ids, the hashes and this file. Run the
   verification above and put its output in the PR.
5. Open a PR. It needs the owner's approval, because `.agents/` is in CODEOWNERS.

## Upstream git blob ids at the commit

```
3b7b82d0da2db857eda1a798dbd908ea136f07b5  LICENSE
efdeacdf470d5bd1d714ca231d6276f086214971  SKILL.md (upstream; the local copy differs on line 4)
8b6afc4920e4a71802764e72f3ec7f83f0e530ce  references/action-profiles.md
1ec95b5505f71def1c5f9895493bf87e719264c5  references/cross-file-resolution.md
a8a7397da09643dce6211739602549bf247fa414  references/foundations.md
77388fc7ae219976adc2cb4ca5719f6664db3ef5  references/vector-a-env-var-intermediary.md
75e6a7fd961a74937d6e4bdf3b3a611978a8b5dc  references/vector-b-direct-expression-injection.md
630d9e4f14d22fb7adf810176d8436546333f72b  references/vector-c-cli-data-fetch.md
659d2f60fc9b9b7b2a847e625b5307308e281d86  references/vector-d-pr-target-checkout.md
681dd7ae2485c031fd5210cacd42e6b5562cb109  references/vector-e-error-log-injection.md
5ca7365843780b0d50cecd22154efcfa99c4c9d8  references/vector-f-subshell-expansion.md
7e2bbfb63819ac5a25a61695fc594493a17207cf  references/vector-g-eval-of-ai-output.md
e28e4ace81fb01346a8984100f53c2dac7d2b9e6  references/vector-h-dangerous-sandbox-configs.md
881ee57e23a8bb6dee9b00b53838d727a8e3abb0  references/vector-i-wildcard-allowlists.md
```

## SHA-256 of each file here

```
7abe19ec9bb73b36141b999b861d24ad855e808bafe0f81e84cce28556f6c297  LICENSE
32d2ad759338acc3e38d146507c6f84623c1d3d0ae967aa020ce516ffd0fe265  SKILL.md
bbfaf6abe69b57e6289948340aa649482378b53f752a25cba92e7d4a740c4324  references/action-profiles.md
b7d0d31fa24f1b987953ef0c11dc22ad48e81675b34de65c65ff79a74a3e6084  references/cross-file-resolution.md
722ee7ea1dc06af8fa8cbb6cacb62f3c7c70139c8ed402752dd12c851a195d0e  references/foundations.md
7903992f54409760aae64ddb45bb3c26032223443f746ac6e3d8fd281bda877d  references/vector-a-env-var-intermediary.md
be6e5baff68d6770665ce6c7b7995c92dfdec3866b96ea21330c94b340499631  references/vector-b-direct-expression-injection.md
f11d2333f2b3800215311047c0b0ad35a1020c79dbb77614ff66a8c1fef16252  references/vector-c-cli-data-fetch.md
fc029037bb219aed5e130708eb4911ad235e47f1394e2155b5e40175ddb2d187  references/vector-d-pr-target-checkout.md
2c93623d96f49c4ab70bc2dd92e111b2cfca68ff70aaf579efd86812b0df15a9  references/vector-e-error-log-injection.md
d6ede1d6a7769e1b75faed3dfeba7d212e0115fb9ddb09d39145bf65d062ba7f  references/vector-f-subshell-expansion.md
b9a47a41dd1470cc469cc030cd6cd6059b35741c78ba9f494c1d18b2fd65b686  references/vector-g-eval-of-ai-output.md
892cb478d91c2e155b4084c5cf707bea5045bb4e2a11c7a5a1131b6212690b86  references/vector-h-dangerous-sandbox-configs.md
68a4cd774ad829e535a8ec15ee492caefbae8e46f115f345b7f2bf2fecf88f86  references/vector-i-wildcard-allowlists.md
```
