---
name: pr-evidence
description: Produce the evidence section of an ODBCPP pull request. Covers local builds of both products with explicit profiles, exact pass/skip/fail counts with names, whether each failure also fails on main, red-before-green for bug fixes, the changed-GTest brace check, CI runs for the exact head commit, and what remains open. Use before opening a PR, after each push, and when a reviewer asks for evidence.
---

# Evidence for an ODBCPP PR

AGENTS.md ("Evidence the author attaches to every PR") and ADR 0001 decision 5
set the rules. Every claim in the PR body must point to something below. A
claim without evidence is a review finding. Wrong counts and an unproven
"also fails on main" are the usual mistakes.

Never present these as a pass:
- an interrupted run;
- an exit code alone;
- a run of a different commit or binary.

Never put credentials in a log, report or PR.

## 0. Pin what you are testing

```sh
HEAD=$(git rev-parse HEAD); BASE=$(git merge-base HEAD origin/main)
git status --porcelain      # must be empty: test committed bytes only
E=~/.local/state/odbcpp/evidence/<topic>-${HEAD:0:7}; mkdir -p "$E"
df -h ~                     # keep at least 5 GiB free before building
```

Keep evidence outside the repository. Delete only build directories you
created, after checking nothing still reads them.

## 1. Build and run both products

Configure every build as `POSTGRESQL` and as `REDSHIFT`, with an explicit
Driver Manager, SQLWCHAR and crypto profile, using the presets in
`tools/development/COMMANDS.md`:

| Host | Presets |
| --- | --- |
| macOS | `macos-iodbc-pg`, `macos-iodbc-redshift`, with `ODBCPP_IODBC_ROOT=/opt/homebrew/opt/libiodbc` and `-DODBCPP_CRYPTO_LINKAGE=BUNDLED_STATIC` |
| Linux / WSL | `unixodbc-pg`, `unixodbc-redshift` (or `linux-iodbc-*` with `ODBCPP_IODBC_ROOT`) |
| Windows | `windows-pg`, `windows-redshift`, with `-DODBCPP_CRYPTO_ROOT=...` |

```sh
for p in pg redshift; do
  b=build-macos-iodbc-$p
  cmake --preset macos-iodbc-$p -DODBCPP_CRYPTO_LINKAGE=BUNDLED_STATIC > "$E/$p-configure.log" 2>&1 &&
  cmake --build "$b" -j8 > "$E/$p-build.log" 2>&1 &&
  ctest --test-dir "$b" -L '^unit$' --no-tests=error \
        --output-junit "$E/$p-unit.xml" > "$E/$p-unit.log" 2>&1
  echo "$p exit=$?" | tee -a "$E/exits.txt"
done
```

- Run the unit label at minimum. If the change touches Driver Manager or ODBC
  entry points, also run the `-offline` test preset. If it touches only one
  product's code, still run both.
- Use a fresh build directory when the crypto profile or preset changes.
- Run every case in a collection. Do not stop at the first failure or rerun
  only the failures.

## 2. Summarise exactly

```sh
python3 -I -B tools/development/junit_summary.py \
  "POSTGRESQL unit=$E/pg-unit.xml" "REDSHIFT unit=$E/redshift-unit.xml" | tee "$E/summary.md"
```

Paste its output into the PR unedited: counts, plus the name of every failed,
skipped and disabled case. A non-zero exit means a file was missing,
truncated or inconsistent. That is not a pass.

## 3. Does each failure also fail on `main`?

For each failed or skipped case, find out. Never assume. Use a worktree at
`$BASE`, or at current `origin/main` for a rebased branch, with the same
preset and options:

```sh
git worktree add --detach ~/.codex/worktrees/main-check/odbcpp "$BASE"
# configure + build as in step 1, then run only the cases in question:
ctest --test-dir <main-build> -R '^(test_a|test_b)$' --output-junit "$E/$p-main-check.xml"
```

Record each case as one of:
- fails on main too (pre-existing; cite the log);
- passes on main (your change broke it: fix it, don't report it);
- skips on both, with the reason.

Remove the worktree afterwards.

## 4. Bug fix: red before green

Commit the failing test before the fix. Then build that test-only commit, and
run the new test there and at the head:

```sh
git worktree add --detach ~/.codex/worktrees/red-check/odbcpp <test-only-commit>
# build, then:
ctest --test-dir <red-build> -R '^<new_test>$' --output-log "$E/red.log"    # must fail
ctest --test-dir <head-build> -R '^<new_test>$' --output-log "$E/green.log" # must pass
```

State the commits for both runs and quote the failing assertion from
`red.log`. A test that passes before the fix doesn't prove the fix.

## 5. Changed-test checks

```sh
python3 tools/development/check_changed_gtest_control_flow.py --base "$BASE"
```

Also list any assertion you changed or removed on purpose, and why. Report
anything that takes a test out of a required run as a change, with its reason:
- a dropped `unit` label;
- `WILL_FAIL` or `DISABLED_`;
- a new skip;
- a sanitizer option that does nothing.

## 6. CI for this exact head

After pushing, wait for the checks to finish. Use one watcher, not repeated
polling. Then cite only runs whose head is `$HEAD`:

```sh
GH_TOKEN="$T" gh api --paginate "repos/vahid110/odbcpp/commits/$HEAD/check-runs" \
  --jq '.check_runs[] | [.name, .conclusion, .html_url] | @tsv'
```

`$T` is the asserted bot token from the `open-pr` skill. List each required
check with its conclusion and link. A check that is missing, pending or
neutral is not green. After each new push, replace the CI section. Don't keep
links to an older commit.

## 7. Open: what nothing above covers

List it in the PR's "Open debt". Required CI does not cover (ADR 0001
decision 5):
- macOS REDSHIFT;
- sanitizers beyond unit tests;
- live behaviour;
- installers;
- the Windows GUI;
- connected Driver Manager suites;
- TSan;
- performance.

Also list every platform, profile or suite you did not run locally, and any
live test. Live AWS tests follow `DEVELOPMENT_WORKFLOW.md` and the cost limits
in AGENTS.md. They never run from this skill.

## Template

```markdown
## Tests
Head `<HEAD>`, base `<BASE>`; local <host>, presets <...>, crypto <...>.

<junit_summary.py output>

Failures and skips against main: `<case>` also fails on `<BASE>` (<log>) ...
Red before green: `<new_test>` fails at `<test-only commit>` (<assertion>), passes at `<HEAD>`.
Changed-GTest brace check: clean. Assertions changed: none / <list with reasons>.
Logs and JUnit: `<E>/`.
CI at `<HEAD>`: <check: conclusion (link)> ...

## Open debt
<not covered / not run, one per line>
```
