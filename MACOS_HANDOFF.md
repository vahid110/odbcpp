# macOS handoff: Unicode follow-up 014 (PR #96), temporary file

**Audience:** an agent or engineer on a macOS machine.

**Finish condition:** run your part, report it in PR #96, then delete this file in a commit. Do not merge the PR.

Background state, commit history and the gap list are in
`docs/handoffs/unicode-followup014.md`. The PR description holds the
authoritative evidence. Follow `AGENTS.md` and `DEVELOPMENT_WORKFLOW.md`.

## What is already done
| Profile | Offline | Live Redshift |
|---|---|---|
| Windows ODBC 2/2 | full preset, 122/122 CTest | 10/10 |
| WSL unixODBC 2/2 | full preset, 120/120 | 10/10 |
| WSL iODBC 4/4 | full preset, 120/120 | 10/10 |
| macOS PR CI (`primary.yml`) | POSTGRESQL product only: unit 121/121, PostgreSQL integration 12/12 | — |

Live Redshift ran against the Serverless endpoint.

## Your part (macOS, iODBC, 4-byte SQLWCHAR, REDSHIFT product)
Do the steps in order. Record each command's actual exit code; don't infer a pass.

### 1. Source
```sh
git fetch origin
git checkout codex/unicode-contract-followup014
git pull --ff-only
git rev-parse HEAD   # record it; the work must be on this exact head
```
Use a clean checkout. Don't reset, rebase or force-push.

### 2. Toolchain identity (record it)
`sw_vers`, `uname -m`, `cmake --version`, `ninja --version`, `clang++ --version`, `openssl version`, the iODBC version and its path.

Example setup: `brew install libiodbc ninja cmake openssl@3`, then
`export ODBCPP_IODBC_ROOT="$(brew --prefix libiodbc)"`. The preset reads
`$ODBCPP_IODBC_ROOT/include` and `$ODBCPP_IODBC_ROOT/lib/libiodbc.dylib`.

Check free space first: at least 5 GiB, per `AGENTS.md`.

### 3. Pre-build check
```sh
python3 tools/development/check_changed_gtest_control_flow.py \
  --base b5063e1fe0ae1e0a3fc3bf96fe88824411889a2f
```
It must report 0 findings.

### 4. Offline REDSHIFT suite (required)
```sh
cmake --preset macos-iodbc-redshift
cmake --build --preset macos-iodbc-redshift
GTEST_OUTPUT=xml:"$PWD/macos-gtest/" \
  ctest --preset macos-iodbc-redshift-offline --output-junit macos-redshift-offline-junit.xml
```
The build directory is `build-macos-iodbc-redshift`.

Report:
- the CTest count passed/total;
- the GoogleTest case total, failures and skips, with skip reasons;
- the configure/build/test exit codes.

Expected:
- Windows skipped 6 macOS-only trust-anchor tests. Those should **run** here, so report their result explicitly.
- The Unix runs skipped 8 `test_platform_trust NativeChainEvaluationTest` cases. Report what macOS does with them.

Suites most relevant to this PR: `test_backend_contract`, `test_diagnostics_apis`, `test_pg_protocol_parser`, `test_unicode`, `test_attribute_apis`, `test_descriptor_apis`. Name them in the report if any fail.

If anything fails, keep the original failure output. Classify it as a product defect, a fixture or input defect, or an environment defect, with evidence: GoogleTest XML, ODBC diagnostics, driver logs. Don't weaken assertions.

### 5. Live Redshift, the 10 cases (required if AWS access is available)
**Endpoint**
- Redshift Serverless workgroup `odbcpp-redshift-pilot`, account `385207570137`, region `eu-north-1`.
- Host `odbcpp-redshift-pilot.385207570137.eu-north-1.redshift-serverless.amazonaws.com`, port 5439, database `odbcpp_pilot`.
- Base and max capacity are 4 RPU. A monthly usage limit (deactivate on breach) is in place. Don't change capacity or limits, and don't create or delete resources.

**Credentials**
```sh
aws redshift-serverless get-credentials --workgroup-name odbcpp-redshift-pilot \
  --db-name odbcpp_pilot --duration-seconds 3600 --region eu-north-1 --profile <profile>
```
- Write `dbUser` and `dbPassword` only into a 0600 file inside a 0700 directory. Never print them, commit them or put them in the PR.
- If you have no working AWS identity, **skip step 5**. Report "live macOS not run: no AWS access" and ask the human for access in the PR. Don't block steps 4 and 6 on it.

**Network**
- The security group `sg-06227a706295e3f05` must allow this Mac's public IP on 5439. Check with `aws ec2 describe-security-groups`.
- If it doesn't, you may add one `/32` rule for this IP only, with the human's approval, and **remove it afterwards**. Record both changes.

**TLS:** use a CA bundle containing the Amazon roots, for example `/etc/ssl/cert.pem` on macOS or the Amazon Trust Services roots. Verification must stay on.

**Environment** (set only for the test process):
```
ODBCPP_REDSHIFT_TEST_CONNECTION=SERVER=<host>;PORT=5439;DATABASE=odbcpp_pilot;UID=<dbUser>;PWD=<dbPassword>;SSL=1;SSLCAFILE=<ca bundle>;
ODBCPP_REDSHIFT_UNICODE_ALIAS_METADATA_ADMISSION=unicode-alias-metadata-v1
ODBCPP_REDSHIFT_UNICODE_CHUNK_ADMISSION=unicode-chunk-result-v1
ODBCPP_REDSHIFT_WARMUP_SECONDS=120
```

**Cases:** run one process per case, with a finite timeout of about 200 s each:
```sh
build-macos-iodbc-redshift/tests/it_redshift_real --gtest_filter=<case> --gtest_output=xml:macos-live/<n>.xml --gtest_color=no
```
1. `RedshiftRealTest.ConnectionTest`
2. `RedshiftRealTest.ErrorHandling`
3. `RedshiftRealTest.UnicodeRoundTrip`
4. `RedshiftRealTest.WideDiagnosticsCarryServerUnicodeErrorText`
5. `RedshiftRealTest.LegacySqlErrorWDrainsServerErrorThenNoData`
6. `RedshiftRealTest.WideZeroCapacityAndDescriptorUnicodeColumnNames`
7. `RedshiftRealTest.WideDriverConnectZeroCapacityOutputReportsTruncation`
8. `RedshiftUnicodeAliasMetadataRealTest.DirectAndPreparedUnicodeAliasNamesAndRecovery`
9. `RedshiftUnicodeChunkRealTest.PreparedWideChunksBoundTruncationAndNull`
10. `RedshiftDeferredInputRealTest.ChunkedUnicodeOwnsPiecesAndReturnsServerValue`

If you use CTest or the live registrar (`tests/development/live_suite`), keep `Testing/Temporary/LastTest.log` private, because it records the environment.

**Report:**
- the passed count;
- each failure with its GoogleTest output;
- the `redshift_warmup_ready`, `redshift_warmup_attempts` and `redshift_warmup_ms` properties from the XML;
- whether a cold start happened. If the first case's warm-up took more than about 2 s or needed more than one attempt, that is the first real evidence of the warm-up rescuing a cold start, so say so.

Before publishing, scan everything you will publish for the password and remove any file that contains it.

### 6. Report in PR #96
Edit the PR description with `gh pr edit 96 --body-file <file>`. Keep every existing section and add `### macOS (iODBC 4/4, REDSHIFT)` under "Test results". It should contain:
- the exact head, toolchain identity and exit codes;
- offline counts, skips and reasons;
- the live result, or why it didn't run;
- any cloud or network changes and their removal.

Update the "Gaps" list: mark gap 1 done or partially done, with the reason. Keep it honest. A partial run is not a pass, and a skipped live run stays listed as debt.

Optionally also post a short PR comment summarizing the macOS result.

### 7. Remove this handoff and push
```sh
git rm MACOS_HANDOFF.md
# Update docs/handoffs/unicode-followup014.md: "Verified so far" and gap 1.
git commit -m "Record macOS REDSHIFT results and remove the macOS handoff"
git push origin codex/unicode-contract-followup014
```
- Use a normal push. Never force-push or push to `main`.
- Don't commit build output, evidence containing credentials, or private files.
- Then confirm the PR head equals your pushed commit.

## Limits
- No merge.
- No changes to other branches, PRs, global settings or cloud resources beyond the optional `/32` rule above.
- No product code changes unless a macOS failure proves a product defect. If it does, report it first. Any fix needs failing-before/passing-after GoogleTest evidence and a fresh macOS run.
