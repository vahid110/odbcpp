# Plan: driver-manager test layer

**Status:** authorized by the human on 2026-10-24 as test-infrastructure work, overriding the product pause in `AGENTS.md` for this work only. Everything else in `AGENTS.md` and `DEVELOPMENT_WORKFLOW.md` still applies.

**Branch:** `codex/dm-test-layer`, stacked on PR #96 (`codex/unicode-contract-followup014`). Open a **draft** PR with base `codex/unicode-contract-followup014`. Merge `origin/codex/unicode-contract-followup014` into this branch when #96 changes; never rebase pushed commits. After #96 merges, retarget the PR to `main`.

## Problem
- About 3,000 GoogleTest cases (124 files in `tests/unit` and `tests/integration`) link `odbcpp::core` directly and call the driver's `SQL*` entry points in-process. No driver manager is involved.
- Only `it_driver_manager_unicode` (8 cases), the `it_driver_manager` smoke program and a few opt-in live package tests go through a real manager.
- Applications always go through a manager: Windows ODBC, unixODBC or iODBC. A manager:
  - maps ANSI calls to W calls and back, and converts between SQLWCHAR widths;
  - raises its own diagnostics (IM*, HY010 from its state machine);
  - remaps SQLSTATEs for ODBC 2.x applications;
  - answers some calls itself (`SQLGetFunctions`, `SQLDataSources`, `SQLDrivers`);
  - resolves DSNs and pools connections;
  - has its own defects.
- PR #96 found, with only 8 manager-level cases:
  - iODBC 3.52.16 drops every driver-connect warning record (01S00, 01004);
  - iODBC misreports truncated driver-connect output lengths, and with capacity 0 reads past the buffer;
  - iODBC passes non-BMP character counts through unconverted between mixed widths;
  - unixODBC widens ANSI-connected diagnostics through the C locale;
  - a fixture defect: the 1024-unit connect buffer couldn't hold the ~1960-character Redshift IAM connection string.

## Principles
1. **Two layers, different jobs.**
   - The **direct layer** (today's tests) proves driver contracts precisely. A failure is unambiguously the driver's.
   - The **manager layer** proves what applications see.
   - Neither replaces the other. A manager can hide a driver bug (iODBC hides our 01004), and some inputs (malformed server bytes, oversized names, posting paths) only exist on the direct layer.
2. **One scenario source, compiled twice.** Black-box scenarios that use only public ODBC calls are written once:
   - built linked to `odbcpp::core` (direct);
   - built linked to each manager with the driver registered (manager).
   - Don't fork test bodies per manager.
3. **Expectations as data.** Expected return code, SQLSTATE and lengths come from a small per-profile table.
   - The default is "same as direct".
   - A profile is the manager, its version, the application and driver SQLWCHAR widths, an ANSI or Unicode application, and ODBC 2.x or 3.x.
4. **Assert known manager deviations; don't skip them.** For example: "iODBC 3.52.16: SQL_SUCCESS_WITH_INFO, no record, length 7", with an evidence link.
   - If a manager fixes or changes the behaviour, the test fails and the table is updated.
   - Skips are only for genuinely unreachable cases, with the reason in the XML.
5. **Three assertion tiers per scenario:**
   - **Invariant on every profile:** data content, SQLSTATE class, no crash or overrun, handle state.
   - **Profile-specific:** exact codes and lengths, from the table.
   - **Driver-side attribution:** when a manager result deviates, an ODBC trace (Windows trace, unixODBC `Trace=Yes`, iODBC trace) must show what the driver returned. Keep the trace as evidence.
6. **Record SQLSTATE origin.** Each expectation says whether the driver or the manager produces the state. The same scenario yielding different states on different managers is then expected and explained.

## Phases (stop and report after each one; wait for the human before the next)

### Phase 0: classify (no product or test behaviour changes)
- **Count:** for every GoogleTest case, record whether it uses only the public ODBC API, or needs driver internals:
  - `HandleRegistry`, `get_handle_as`, `FakeProvider`, `*TestAccess`, direct `add_diagnostic`;
  - internal classes, injected backends.
- **Output:** a checked-in table (`docs/plans/driver-manager-test-inventory.md` or a generated CSV with its generator) with per-file and per-suite counts:
  - public-only;
  - needs the fake backend only;
  - needs internals.
- A rough grep on 2026-10-24 suggested about 2,980 of 3,021 touch internals at file level. The real per-case number is the point of this phase.
- **Also list:** the live and integration suites, and whether each could run unchanged through a manager if given a `DRIVER=` connection string.
- **Report:** which suites are worth migrating first, with an estimate.

### Phase 1: live and integration suites through a manager
- **Run the existing live/integration suites through a manager:** `it_redshift_real`, the PostgreSQL `it_*` suites and the like.
  - Change only the connection setup: allocate through the manager, connect with a `DRIVER=<path>` string. Keep the path bare: iODBC 3.52.16 passes a braced value to `dlopen` with its braces.
  - Keep the direct run as the diagnostic baseline.
  - Live Redshift results on macOS: [driver-manager-live-macos.md](driver-manager-live-macos.md).
- **Registration:**
  - private ini files on Unix (iODBC `ODBCINSTINI`/`ODBCINI`; unixODBC `ODBCSYSINI`);
  - on Windows, a per-user DSN naming the PR-built DLL without admin rights, or HKLM in CI.
- **Connection buffers:** size every buffer that echoes a connection string from the actual string. Redshift IAM strings are ~2000 characters.
- **Deliverable:** CTest registration (labels like `dm-live`, `dm-integration`) that runs on each platform's manager.

### Phase 2: fake backend reachable through a manager
- **Add a test-only, compile-time-gated build option for the driver:** for example `ODBCPP_TEST_BACKEND=ON`, never in release packages.
  - With it, a connection-string keyword selects the in-process fake provider and a named scenario.
  - The fake must be deterministic and must not open sockets.
- **Ownership:** the test backend belongs to tests or the test build. It must not leak into product code paths or the SDK public surface.
- **Proof needed:**
  - a release build doesn't contain the keyword or the fake;
  - a test build registered with each manager can run one fake scenario.

### Phase 3: shared black-box suite plus the expectation table
- **Migrate:** move public-only scenarios from Phase 0 into a shared suite, compiled twice (direct and manager).
  - Start with connect, diagnostics, legacy `SQLError`, metadata, fetch, Unicode and catalog functions, then attributes, cancel and async.
- **Expectation table:**
  - keyed by profile, with defaults equal to direct;
  - known deviations asserted, with evidence links;
  - SQLSTATE origin recorded.
- **Convert the existing `it_driver_manager_unicode` iODBC skips** into asserted deviations.
- **Don't delete direct tests.** Remove a direct duplicate only after the shared version provably covers the same assertions on the direct build.

### Phase 4: CI matrix
- **Profiles:**
  - Windows ODBC (2-byte);
  - unixODBC (2-byte);
  - iODBC 4/4;
  - iODBC with a 2-byte driver and a 4-byte application.
- **Variants:** ANSI and Unicode applications, ODBC 3.x and 2.x applications, and pooling on and off where the manager supports it.
- **Servers:** disposable PostgreSQL in CI. Live Redshift through a manager as an opt-in job.
- **Constraints:** no new cloud resources, and no unattended credentials in CI unless the human sets that up.

## Known facts to reuse
- **Registration on each platform** is documented in the PR #96 description: macOS private ini files, the Windows per-user DSN, the WSL ini files.
- **`it_driver_manager_unicode`** reads `ODBCPP_DM_UNICODE_CONNECTION`. It is built whenever `ODBC_DRIVER_MANAGER_LIBRARY` is set, including REDSHIFT builds, and is labelled `integration`.
- **The iODBC 3.52.16 defects above** were confirmed on macOS and WSL. A probe program and its outputs are in private macOS evidence (`~/odbcpp-audits/macos-pr96-unicode014/dm-live/`). The Windows manager keeps driver-connect warnings and full truncation lengths.
- **Live Redshift:** Serverless `odbcpp-redshift-pilot`, eu-north-1, verified TLS, temporary credentials from `aws redshift-serverless get-credentials`.
  - Keep credentials in 0600 files and never print or commit them.
  - Security group `sg-06227a706295e3f05` allows only listed `/32`s; a new IP needs the human's approval.

## Rules
- **Run the check before expensive builds** when C++ tests change: `tools/development/check_changed_gtest_control_flow.py` against this branch's base.
- **Preserve valid assertions and original failures.** Classify every failure as a driver, manager, fixture or environment defect, with traces.
- **Before a product change:** failing-before and passing-after GoogleTest evidence, and report it first.
- **Builds:** reuse one build per configuration, and keep at least 5 GiB free.
- **No merge, no force-push, no push to `main`.** Push only `codex/dm-test-layer`.
- **No cloud changes** beyond a human-approved temporary `/32` rule, removed afterwards.
