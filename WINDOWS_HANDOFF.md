# Windows handoff: Unicode follow-up 014 (PR #96), temporary file

**Audience:** the Windows coordinator (native Windows plus WSL).

**Finish condition:** run your part, report it in PR #96, then delete this file in a commit and push. Do not merge the PR.

Background, commit history and the gap list are in
`docs/handoffs/unicode-followup014.md`. The PR description holds the
authoritative evidence, including the new `### macOS (iODBC 4/4, REDSHIFT)`
section. Follow `AGENTS.md` and `DEVELOPMENT_WORKFLOW.md`.

## What macOS did (already in the PR)
- **Offline REDSHIFT suites:** iODBC 4/4 and unixODBC 2/2 at `a1d9639`. CTest 119 passed, 1 skipped, 0 failed; GoogleTest 2388 cases, 0 failures.
- **Live Redshift, driver linked directly:** the 10 cases, 10/10 on both.
- **Live Redshift through the driver manager** (`it_driver_manager_unicode`, REDSHIFT driver registered in private ini files, at `87f3dd6`):
  - unixODBC: 8/8.
  - iODBC: 6 passed, 2 skipped. iODBC drops every driver-connect warning record, and a probe confirmed this.
- **New tests:**
  - `a1d9639` (offline, `test_backend_contract`):
    - `WideUserNameInfoCountsBytesAndKeepsSupplementaryScalarsWhole`
    - `WideGetDataChunksSplitCombiningSequencesAtScalarBoundaries`
    - `AnsiDsnEchoReplacesInvalidBytesSoWideDiagnosticsSucceed`. It was renamed in `31e5410`; its Windows branch from `2b53ff6` is unchanged.
  - `4d523d3` (`it_driver_manager_unicode`):
    - a buffer fix in `WideDriverConnectTruncatedOutputReportsFullLength`;
    - the new case `AnsiLatin1KeywordWarningStateStaysReadable`.

- **Owner decisions, implemented in `31e5410`:**
  - Diagnostic posting replaces invalid UTF-8 with U+FFFD.
  - `SQLColAttributeW` returns 01004 for a non-NULL zero-capacity buffer.
  - The changed tests are in `test_backend_contract` and `test_diagnostics_apis`. Both macOS presets pass at `31e5410`.

## Your part
Record each command's actual exit code; don't infer a pass. Work on the exact PR head; record `git rev-parse HEAD`.

### 1. Offline: run the new and changed tests on every Windows-side profile (required)
The `a1d9639` and `31e5410` tests have run only on macOS, apart from the Windows DSN-echo branch you checked. `31e5410` changes product code, so this step is a full offline preset run, not just the affected executables.
- Native Windows: `windows-redshift-offline`.
- WSL: `unixodbc-redshift-offline` and `linux-iodbc-redshift-offline`.
- Report the CTest and GoogleTest totals.
- Name these cases explicitly:
  - the three `a1d9639` cases;
  - `EveryPostingPathReplacesInvalidUtf8SoWideReadsSucceed`;
  - `SQLErrorW_MalformedPostedTextIsReplacedAndLegacyRecordsDrain`;
  - `SQLGetDiagRec_MalformedPostedTextIsReplacedForWideAndAnsiReaders`;
  - `ColumnAttributeNullCharacterPreservesValidationAndNumericDomains`.
- If you only run the affected executables, report that as partial, not full-preset evidence.

### 2. Driver-manager suite: run the new case everywhere (required)
`it_driver_manager_unicode` at the PR head, on:
- the Windows driver manager (checkpoint `windows-live`, or locally);
- WSL unixODBC;
- WSL iODBC.

The PostgreSQL product and a disposable PostgreSQL server are fine for this step.
- **Report `AnsiLatin1KeywordWarningStateStaysReadable` on the Windows driver manager.** It is the only real answer to whether the F1 ANSI-echo route can hide a SQLSTATE from Windows applications. The Windows driver manager converts ANSI calls to W calls, so the result is not predictable from the other managers.
- **Also report whether the Windows driver manager keeps driver-connect warnings.** For example: does 01S00 for an unknown keyword reach the application? iODBC drops them.
- If the Windows driver manager fails the case, keep the failure. Classify it as a driver-manager defect or a driver defect, with an ODBC trace. Don't weaken the assertion. Add a Windows-guarded skip only for a proven driver-manager defect, with the evidence in the comment, the same way iODBC is handled.

### 3. Live Redshift through installed driver managers (required if AWS access is available)
This is release blocker 2 for Windows and Linux. macOS has done it.
- Register the **REDSHIFT** driver with the Windows driver manager and with WSL unixODBC (private registration on WSL).
- Set `ODBCPP_DM_UNICODE_CONNECTION` to `DRIVER={<registered name>};SERVER=<host>;PORT=5439;DATABASE=odbcpp_pilot;UID=<dbUser>;PWD=<dbPassword>;SSL=1;SSLCAFILE=<CA bundle>;`.
  - The connection string will be about 2000 characters, because the IAM password alone is about 1745. The test now sizes its buffer for that.
- Endpoint, credentials and TLS: the same as the earlier live runs (Serverless `odbcpp-redshift-pilot`, eu-north-1, verified TLS).
  - Use temporary credentials from `aws redshift-serverless get-credentials`. Keep them in private 0600 files only, and never print, commit or publish them.
  - Scan everything you publish for the password.
- **Network:** security group `sg-06227a706295e3f05` currently allows only `79.234.95.139/32` on 5439. If your public IP differs, ask the human before adding a rule, and remove it afterwards.
- Report per manager: passed, failed and skipped, with skip reasons.

### 4. The two policy decisions are made and implemented (`31e5410`)
Don't re-litigate them. If a Windows run contradicts them (for example, the Windows driver manager handles U+FFFD text or ColAttribute truncation differently), report it with an ODBC trace and leave the product code alone.

### 5. Report and remove this handoff
- **PR body:** fetch it fresh, keep every existing section, and add your results under "Test results".
- **Gaps:** update gap 2 (installed driver-manager paths) honestly.
- **Handoff doc:** update `docs/handoffs/unicode-followup014.md` ("Verified so far", gap 2).
- **Delete and push:**
  ```sh
  git rm WINDOWS_HANDOFF.md
  git commit -m "Record Windows follow-up results and remove the Windows handoff"
  git push origin codex/unicode-contract-followup014
  ```
  Use a normal push: no force-push, no rebase of pushed commits, no push to `main`. Confirm the PR head equals your commit.

## Limits
- No merge.
- No cloud changes beyond an approved, temporary `/32` rule.
- No product code changes unless a failure proves a product defect. In that case, report it first, then fix it with failing-before and passing-after GoogleTest evidence.
