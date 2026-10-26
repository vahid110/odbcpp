# Driver-manager test layer: Windows verification handoff

This is a handoff from the macOS coordinator to the Windows coordinator for PR #98 (`codex/dm-test-layer`). The macOS work is done: three local builds, live Redshift through iODBC 3.52.16 and unixODBC 2.3.14, and results in [driver-manager-live-macos.md](driver-manager-live-macos.md). Windows needs to verify the same source with MSVC and the Windows ODBC manager, then re-run its tests.

The rules in AGENTS.md and DEVELOPMENT_WORKFLOW.md apply unchanged:
- Push only `codex/dm-test-layer`: no merge, no force-push, no rebase of pushed commits.
- Keep the cluster paused during gaps and at the end.
- Credentials stay in 0600 files and stay out of reports.

## Source

- **Head:** the current head of `codex/dm-test-layer`. Merge `origin/main` in first if `main` has moved.
  - **Code:** the test code was last qualified on the Mac at `4010fe8`. Later commits change documentation only.
  - **macOS result at `4010fe8`:** the fifth live run (`dm-live-macos-4010fe8-a`) passed **1,495 of 1,495** across direct, iODBC and unixODBC, with no skips and complete cleanup. See [driver-manager-live-macos.md](driver-manager-live-macos.md#fifth-run-dm-live-macos-4010fe8-a-all-pass).
- **What changed for Windows:** C++ test code, one test header, and the POSIX live-profile tooling. No product (driver) code changed on this branch, apart from merges from `main`. The changed test files:
  - `tests/test_dm_expectations.h`: the expectation table for manager deviations. It has entries for iODBC 3.52.16 (`03.52.1623.0502`) and unixODBC 2.3.14 (`03.52.0002.0003`) only. On a manager with no entry, every case expects exactly the direct driver's behaviour.
  - `tests/integration/it_redshift_real.cpp`:
    - fixture forwards for `SQLGetDiagRec`, `SQLConnect`, `SQLDriverConnect` and `SQLDriverConnectW` (`diag_rec`, `note_connect`);
    - `connection_text()`;
    - `exec_utf8`, `refetch_after_no_data` and other fixture helpers;
    - the expectations for the live buckets R1–R17.
  - `tests/integration/it_metadata_real.cpp`, `tests/test_connection_config.h`, and the other `tests/integration/it_*` suites built as `dm_*`.
- **Not compiled by the PR's Windows CI:** that job builds `TARGET_DATABASE=POSTGRESQL` only. `it_redshift_real` and `dm_it_redshift_real` exist only in Redshift builds, so **they have not been compiled with MSVC yet**.

## What Windows should run

1. **Build both products with MSVC x64:** `TARGET_DATABASE=POSTGRESQL` and `TARGET_DATABASE=REDSHIFT`. Use the Windows CI options (`.github/workflows/ci.yml`, `pr-windows-build`), plus `-DODBCPP_DM_INTEGRATION_TESTS=ON` so the `dm_*` suites register as CTest tests.
   - **Expected:** every `dm_*` target links against `odbc32` only, not `odbcpp_core`.
   - **New since the last Windows check:** `dm_it_redshift_real` in the Redshift build. It uses `std::strtoul`, `std::atoi`, `SQL_FUNC_EXISTS` and `SQL_API_ODBC3_ALL_FUNCTIONS_SIZE` from the manager's `sqlext.h`.
   - **If something doesn't compile:** fix it on this branch with the same structure. Don't compile the expectation out on Windows.
2. **Offline tests:** unit tests and `python -m unittest discover tools/development`.
   - On macOS, `test_live_checkpoint` has one error that comes from `main`'s own test: macOS temp paths resolve through `/private`. It isn't expected on Windows.
3. **PostgreSQL suites through the Windows manager:**
   - Run the seven `dm_it_*` suites against the CI-style PostgreSQL fixture. Register the PR-built DLL as a per-user DSN, with the same DSN name and settings as the direct suites.
   - The table has no Windows entry, so a case that fails through `odbc32` but passes directly is a candidate Windows deviation. For each one, record the `SQL_DM_VER` string, an ODBC trace showing what the driver returned, and the SQLSTATE. Then either add a table entry with that evidence or classify it as a driver defect.
   - Per PR #96, the Windows manager keeps driver-connect warnings and full truncation lengths, so buckets A, I, J and R6 shouldn't apply. Don't add entries without evidence.
4. **Redshift live, direct build:**
   - Run the Windows native checkpoint (`tools/development/live_checkpoint.py` with `native_windows`; see `tools/development/CHECKPOINT_INPUTS.md`) on the Redshift build of this head.
   - This is a regression check of the shared test changes. The direct build must behave exactly as before: all R* paths are compiled out of direct builds, or inactive there.
   - **Expected:** the macOS direct results. Pass everything except the open items below.
5. **Not supported yet:** Redshift `dm_*` live through the Windows manager. `windows_live_backend.py` refuses any profile with `driver_manager` (`posix_driver_manager_profile`). Supporting it needs a Windows DSN profile that names the PR-built DLL through the manager. Treat it as a follow-up, not part of this verification.

## Open items carried over

| item | where | state |
|---|---|---|
| P1 | static-cursor `SQL_ATTR_ROW_NUMBER` off a row | Driver unchanged (returns 0); unixODBC refuses with 24000 (R11). Resolved by the issue #120 product change: the driver now raises 24000/HY109 itself. The 0-vs-24000 clause of R11 is retired; unixODBC's interception of the HY109 case remains as `RowNumberUnfetchableIntercepted`. |
| HYT00 | `RedshiftMaterializedEnvelopeRealTest` ~4 MB materialization in a 5 s statement budget | Timed out in the second and third macOS runs on a slow link and passed in the fourth and fifth. Depends on throughput, not code. Watch for it on Windows. |
| R12 | `SQLFetch` after `SQL_NO_DATA` on Redshift catalog results through iODBC | iODBC raises HY010 itself. Recorded and asserted; macOS/iODBC only. |
| B1 | shared timeout fixture through a manager | Unrun boundary: its verified-TLS check needs the driver's own DSN parser. |

## Evidence on the Mac (private)

- Live runs: `~/.local/state/odbcpp/redshift-pilot/dm-live-macos-*/` (CTest XML, logs, `classified.json`).
- Inventories and receipts: `~/.local/state/odbcpp/continuation/dm-live-macos-2026102*/`.
- Probe programs and their outputs: `~/odbcpp-audits/dm-test-layer/probe-*` (loopback PostgreSQL fixture, both managers, and direct).
