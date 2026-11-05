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
`gh api -X POST`. Nothing else is changed.

This only removes the grant the skill itself makes in Claude Code. It does not
contain the session:
- Codex ignores `allowed-tools`. Its sandbox and approval policy decide what
  runs.
- In Claude Code, user or project allow rules and auto or bypass modes still
  apply.

To audit workflows that someone else controls (a fork's PR, a remote
repository), use a read-only or approval-required session.

## Known errata (upstream, not changed here)

Do not copy these remediations from `references/action-profiles.md`:
- **Lines 170-173** label `RESULT='${{ steps.inference.outputs.result }}'` in
  `run:` as SAFE. It is not safe. The expression is pasted into the script
  before bash parses it, so output containing `'` breaks out of the quotes.
  That is vectors B and G of this same skill. AI output may reach a `run:`
  script only through `env:` and a quoted `"$VAR"`, as
  `.github/workflows/agent-review-run.yml` does.
- **Lines 29-33** give `--allowedTools "Bash(npm test:*) Bash(git diff:*)"` as
  a way to "restrict shell access". It is not a boundary:
  - `npm test` runs `package.json` scripts, which are attacker code when a PR
    head is checked out;
  - `git diff --output=<path>` writes files, and `--ext-diff` runs programs.
  - Prefix rules (`Bash(x:*)`) never restrict arguments. Allow no shell, or
    only exact commands that cannot run PR-controlled code.
- **Line 182** repeats `Bash(specific:*)`. It also lists Codex
  `sandbox: workspace-write` as restricting shell access, but that setting is
  a filesystem limit, not a command limit.

## Verifying this copy

Run it with bash from this directory, in a checkout of a reviewed commit. It
prints `PASS` and exits 0 only if every check passes. The exit code is what
counts: the files are untrusted, so the output of checks that show their text
goes through `cat -v`, which makes terminal control sequences visible instead
of running them.

What a PASS shows: the files here, as they are on disk, are the upstream files
at the pinned commit, apart from the one local change, with the hashes and the
character allowlist this file states. What it does not show:
- That this file is right. The script, the hashes and the allowlist live in
  it, so anyone who edits it can make the script pass. A change to it is
  checked by reviewing its diff, and `.agents/` needs the owner's approval.
- That the checkout is the reviewed commit; check `git rev-parse HEAD`.
- That nothing else shadows the skill: another skill with the same `name:`
  elsewhere (for example under `.claude/skills/`), or a repository
  `.gitattributes` that changes what git writes to disk (check 5 still sees
  the bytes).
- That the tools are honest: `git`, `gh` (and its host and configuration,
  such as `GH_HOST`), `python3`, `shasum` and `PATH` are trusted.
- That no text is hidden from a reader. Check 6 limits characters and refuses
  HTML comments; other markdown that a renderer hides still passes, so read
  the files as plain text.

```bash
C=711af095215ad4ecde5b90cd1d0124b3f1c4ca9e
P=plugins/agentic-actions-auditor/skills/agentic-actions-auditor
R=repos/trailofbits/skills
U=$(mktemp); trap 'rm -f "$U"' EXIT; bad=0
# 1. The commit is on upstream main. GitHub also serves commits that exist
#    only in a fork through the parent's URLs, so the SHA alone proves nothing.
#    compare/$C...main is "identical" or "ahead" only if $C is an ancestor of main.
case "$(gh api "$R/compare/$C...main" --jq .status)" in identical|ahead) ;; *) echo "FAIL not on main"; bad=1;; esac
[ "$(gh api "$R/commits/$C" --jq .commit.verification.verified)" = true ] || { echo "FAIL unverified"; bad=1; }
# 2. Exactly the expected files, and on disk exactly what is committed: each
#    a regular, non-executable file (index mode 100644, and on disk no symlink
#    and no execute bit), with no change, untracked or ignored file here, so
#    an added script or config, or a swapped-in symlink, is caught.
printf '%s\n' LICENSE PROVENANCE.md SKILL.md references/action-profiles.md \
  references/cross-file-resolution.md references/foundations.md \
  references/vector-{a-env-var-intermediary,b-direct-expression-injection,c-cli-data-fetch,d-pr-target-checkout,e-error-log-injection,f-subshell-expansion,g-eval-of-ai-output,h-dangerous-sandbox-configs,i-wildcard-allowlists}.md \
  | sort | diff - <(git ls-files . | sort) >/dev/null || { echo "FAIL file set"; bad=1; }
s=$(git ls-files -s .) || { echo "FAIL git ls-files"; bad=1; }
m=$(printf '%s\n' "$s" | awk '$1 != "100644"')
[ -z "$m" ] || { printf 'FAIL mode\n%s\n' "$m" | cat -v; bad=1; }
st=$(git status --porcelain --ignored --untracked-files=all -- .) || { echo "FAIL git status"; bad=1; }
[ -z "$st" ] || { printf 'FAIL working tree\n%s\n' "$st" | cat -v; bad=1; }
while IFS= read -r f; do
  if [ -L "$f" ] || [ -x "$f" ] || [ ! -f "$f" ]; then echo "FAIL on disk $f" | cat -v; bad=1; fi
done < <(git ls-files .)
# 3. Every unmodified file's blob id is in the upstream tree at its path.
gh api "$R/git/trees/$C?recursive=1" --jq '.tree[] | select(.type=="blob") | "\(.sha)  \(.path)"' > "$U"
for f in LICENSE references/*.md; do
  u=$P/$f; [ "$f" = LICENSE ] && u=LICENSE
  grep -qxF "$(git hash-object "$f")  $u" "$U" || { echo "FAIL blob $f" | cat -v; bad=1; }
done
# 4. SKILL.md: upstream's blob is the listed one, and only line 4 differs, as
#    the local change. allowed-tools must stay within Read Grep Glob.
grep -qxF "efdeacdf470d5bd1d714ca231d6276f086214971  $P/SKILL.md" "$U" || { echo "FAIL upstream SKILL.md"; bad=1; }
d=$(gh api "$R/git/blobs/efdeacdf470d5bd1d714ca231d6276f086214971" --jq .content | base64 -d | diff - SKILL.md | cat -v)
[ "$d" = "$(printf '4c4\n< allowed-tools: Read Grep Glob Bash\n---\n> allowed-tools: Read Grep Glob')" ] || { echo "FAIL SKILL.md diff"; bad=1; }
# 5. The local SHA-256 list: one line for every file but this one, and each
#    matches. (A deleted line would otherwise drop that file's check.)
l=$(sed -n '/^## SHA-256/,$p' PROVENANCE.md | grep -E '^[0-9a-f]{64}  ')
[ "$(printf '%s\n' "$l" | cut -c67- | sort)" = "$(git ls-files . | grep -vx PROVENANCE.md | sort)" ] \
  || { echo "FAIL sha256 list"; bad=1; }
h=$(printf '%s\n' "$l" | shasum -a 256 -c --quiet 2>&1) \
  || { printf 'FAIL sha256\n%s\n' "$h" | cat -v; bad=1; }
# 6. Characters, per file. Each file may contain only the non-ASCII
#    characters listed for it (the allowlist is per file, not per line), and
#    no control characters other than tab and newline: no CR (the bytes are
#    decoded directly, so a CR is not turned into a newline first) and no
#    HTML comments. ('<!-''-' keeps this file from matching itself.)
python3 -I -c '
import pathlib, sys
allowed = {"LICENSE": {"\u201c", "\u201d"}, "PROVENANCE.md": {"\u00f3"}}
bad = 0
for f in sorted(pathlib.Path(".").rglob("*")):
    if f.is_file():
        text = f.read_bytes().decode("utf-8")
        for n, line in enumerate(text.split("\n"), 1):
            for ch in line:
                if (ord(ch) > 126 and ch not in allowed.get(str(f), ())) or (ord(ch) < 32 and ch != "\t"):
                    print(f"FAIL {ascii(str(f))}:{n} U+{ord(ch):04X}"); bad = 1
sys.exit(bad)' || bad=1
# grep exits 1 when nothing matches, 0 on a match and 2 on an error; only 1 passes.
c=$(grep -rn '<!-''-' . 2>&1); rc=$?
[ $rc = 1 ] || { printf 'FAIL html comment or grep error (%s)\n%s\n' "$rc" "$c" | cat -v; bad=1; }
[ $bad = 0 ] && echo PASS || { echo FAIL; false; }
```

## Updating

Never edit these files in place, apart from the local change. To update:
1. Pick a new upstream commit by its full 40-character SHA. Never use a
   branch, a tag or a short SHA. Check 1 must show that it is on upstream
   `main` and verified.
2. Read the upstream diff in full. In the PR, call out any change to
   frontmatter (`allowed-tools`, `description`) and any new executable,
   script, hook, MCP configuration or `agents/` file.
3. Copy the files again and re-apply the local change. `allowed-tools` stays
   within `Read Grep Glob` unless the PR explicitly asks the owner to accept a
   wider grant. Re-check the errata against the new text.
4. Update the commit, the file list, the blob ids, the hashes and the allowed
   characters in the verification. Run it and put its output in the PR.
5. Open a PR. It needs the owner's approval, because `.agents/` is in
   CODEOWNERS.

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
