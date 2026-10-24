# Driver-manager test inventory (Phase 0)

Phase 0 of [driver-manager-test-layer.md](driver-manager-test-layer.md): this phase classifies tests only. It changes no product code, test code or CTest registration.

- **Per-case table:** [driver-manager-test-inventory.csv](driver-manager-test-inventory.csv), one row per GoogleTest case.
- **Generated counts:** [driver-manager-test-inventory-counts.md](driver-manager-test-inventory-counts.md), by directory, file and suite.
- **Generator:** `tools/development/dm_test_inventory.py`. Its offline tests are in `tools/development/test_dm_test_inventory.py`.

Regenerate, or check that the checked-in files are current:

```sh
python3 -B tools/development/dm_test_inventory.py \
  --csv docs/plans/driver-manager-test-inventory.csv \
  --summary docs/plans/driver-manager-test-inventory-counts.md [--check]
(cd tools/development && python3 -B -m unittest test_dm_test_inventory)
```

## Method and limits

The generator works on source text; it does not parse C++. For each `TEST`/`TEST_F`/`TEST_P` it collects:

- the case body;
- the fixture class and its bases;
- every file-local helper, type and variable the body reaches (transitively), including definitions from the included `tests/` support headers.

From that text it decides:

- **Does it call ODBC?** Calls to any `SQL*` entry point count.
- **Does it name internals?** Internals are:
  - types and aliases declared in `core/`, `odbc/`, `sdk/` and `product/` headers;
  - namespace-scope functions from those headers, counted only when called;
  - any `rs::…`/`odbcpp::…` qualified name outside `odbcpp::test`;
  - any `*TestAccess`.

  A name the test file or its support headers declare locally hides a project name with the same spelling, unless the code qualifies it with `rs::` or `odbcpp::`.
- **Is it a fake?** A file-local type is a fake if it derives from a backend or transport interface (`IBackendProvider`, `IDatabaseConnection`, `ITransport`, `GenericDatabaseConnection`, …) or has a fake-style name (`FakeProvider`, `ScriptedRedshiftProvider`, …). Local types that a fake names, such as its `Observations` or `WireScript`, count as fake state.
- **What counts as fake glue?** Only constructing an `ODBCConnection` over a fake provider and calling `HandleRegistry::instance().register_handle`. Any other `HandleRegistry` member, such as `get_handle_as`, or any other use of `ODBCConnection` counts as an internal.

| class | meaning |
|---|---|
| `public-only` | Calls ODBC and names no internals. It could run through a manager once its connection setup is a manager connection (see the live/integration table below). |
| `fake-backend` | Calls ODBC. Its only internals are a fake backend or transport and the glue that registers a connection over it. It becomes public-only when Phase 2 makes the fake reachable from a connection-string keyword. |
| `needs-internals` | Calls ODBC and also names internals. The `internals_in` column says where:<br>• `body`: the case itself calls or asserts on internals;<br>• `helpers`: only fixtures or helpers it reaches do. |
| `no-odbc-entry` | Never calls an `SQL*` entry point: SDK, backend, wire, transport, auth, crypto and setup-UI component tests. These also need internals, but no manager can reach them, so they stay on the direct layer. |

Other CSV columns:

- `fake_state`: the case body configures or reads fake-backend state, for example `seen->…` or `script->responses[…]`. A Phase 2 fake reached through a manager must provide that state through a named scenario.
- `connect`: the connection API the case reaches. `+registered-handle` means the connection was built over a fake provider and registered directly, not allocated through `SQLAllocHandle`.
- `internals`: the evidence, meaning the internal names found.

### Known limits

- This is lexical: a member call on an object obtained through `auto` is caught only through the internal call that produced the object.
- `TEST_P` rows count definitions. `instantiate_macros` counts `INSTANTIATE_TEST_SUITE_P` uses, not runtime parameters. Exact runtime counts need `--gtest_list_tests` on built binaries, which this phase did not run.
- Nothing was built or executed. The classes describe source, not test results.

### Spot checks

I compared samples from every class with the source:

- **public-only:**
  - `test_api_validation` handle-state cases;
  - `test_driver_logging` cases that connect to `127.0.0.1:1` and inspect the log;
  - `it_prepared_statements_real` cases. Its `DatabaseFactory` helper is counted only against the cases that call it.
- **fake-backend:** `RedshiftScriptedSessionTest`. Its fixture only registers a `ScriptedRedshiftProvider` connection and then calls `SQLDriverConnect`, and its cases use public calls plus `script->responses`.
- **needs-internals:**
  - an `ApiHandleLifetimeTest` case that calls `HandleRegistry::get_handle`;
  - `it_redshift_real`, whose fixture calls `rs::odbc::ConnectionString::parse` and `rs::util::Clock`.
- **Driver-manager targets:** all 20 cases in targets that link only a manager (`tests/driver_manager`, `it_driver_manager_unicode`, `it_redshift_powerbi_relationship`) classify as public-only. They cannot link internals, so this cross-checks the classifier.

## Results

Before this pass, a file-level grep suggested about 2,980 of 3,021 cases touched internals. Per case, it is 2,744 of 3,021 in `tests/unit` and `tests/integration`. Of those 2,744, 1,766 never call ODBC at all.

| scope | cases | public-only | fake-backend | needs-internals | no-odbc-entry |
|---|---:|---:|---:|---:|---:|
| `tests/unit` + `tests/integration` | 3021 | 277 | 100 | 878 | 1766 |
| all gtest dirs (adds `driver_manager`, `security`, `windows`) | 3051 | 293 | 100 | 878 | 1780 |

The needs-internals cases split as follows:

- **Body: 419 cases.** The case itself calls or asserts on internals; 287 of these also touch fake state.
- **Helpers only: 459 cases.** Typical helper-only internals:
  - compiled-profile selection through `DatabaseFactory::get_compiled_database_type()`;
  - `ConnectionString::parse` in a fixture;
  - `rs::odbc::utf8_to_wide`;
  - `make_statement`/`make_descriptor`, which register handles without `SQLAllocHandle`;
  - backend-data builders such as `owning_date_contract_rows()` that return a `QueryResult` for a fake.

Of the 100 fake-backend cases, 92 touch fake state in the body.

| file | cases | public | fake | internals (helpers only) | internals (body) | no-odbc |
|---|---:|---:|---:|---:|---:|---:|
| `unit/test_backend_contract.cpp` | 426 | 0 | 78 | 134 | 209 | 5 |
| `unit/test_attribute_apis.cpp` | 64 | 3 | 0 | 60 | 0 | 1 |
| `unit/test_diagnostics_apis.cpp` | 34 | 0 | 0 | 14 | 20 | 0 |
| `unit/test_api_validation.cpp` | 26 | 8 | 0 | 14 | 4 | 0 |
| `unit/test_descriptor_apis.cpp` | 23 | 0 | 0 | 12 | 2 | 9 |
| `unit/test_session_cancellation.cpp` | 26 | 0 | 10 | 0 | 2 | 14 |
| `unit/test_redshift_special_types.cpp` | 13 | 0 | 9 | 0 | 4 | 0 |
| `unit/test_redshift_floating_results.cpp` | 6 | 0 | 3 | 0 | 0 | 3 |

The remaining unit files are almost entirely `no-odbc-entry`. The generated counts file has every file and suite.

## Live and integration suites

"Unchanged through a manager" means the same source, linked to a manager instead of `odbcpp::core`, with only the connection string changed. None of the in-process suites qualify yet, for four reasons:

1. **Linking:** the suites include internal headers (`odbc/odbc_api.h`, `odbc/unicode.h`, `core/database/database_factory.h`, …) and link `odbcpp::core`. A manager build needs `<sql.h>`/`<sqlext.h>` from the manager and its library.
2. **`SQLConnect` with a connection string:** fixtures call `SQLConnect(dbc, configured_connection_string_data(), …)`. The value comes from `ODBCPP_TEST_DSN` (default `DSN=RedshiftProd`), so a whole connection string goes in as the server name.
   - The in-process driver accepts that.
   - A manager treats the argument as a DSN name (at most 32 characters) and would return IM002.
   - Fix: pass a DSN name, or switch the fixture to `SQLDriverConnect`.
   - CI already writes `odbc.ini` and `odbcinst.ini` for `RedshiftProd`: unixODBC in `ci.yml`, iODBC in `primary.yml`.
3. **Fixture internals:** listed per suite below. Most are profile selection and string-conversion helpers, which have public or test-local replacements.
4. **Exact echo expectations:** for example, `it_handle_lifecycle` expects the `SQLDriverConnect` output to equal the input exactly, in a 256-byte buffer. A manager may rewrite the completed string; that becomes a profile expectation (plan principle 3). Buffers must also be sized from the actual string for long Redshift IAM strings.

| target (file) | registration | link | connects | cases | public | internals: helpers / body | no-odbc | runs unchanged through a manager? what blocks it |
|---|---|---|---|---:|---:|---|---:|---|
| `it_prepared_statements_real` | `integration` (glob) | core | `SQLConnect`(string) | 141 | 126 | 11 / 4 | 0 | No. Blockers 1 and 2. Helpers use `DatabaseFactory` for the Redshift profile; bodies use `utf8_to_wide` and `refuse`. **Best first candidate.** |
| `it_bind_col_real` | `integration` | core | `SQLConnect`(string) | 110 | 101 | 7 / 1 | 1 | No. Blockers 1 and 2. Helpers use `DatabaseFactory`; one case uses `ConnectionString` and `configured_backend_provider`. **First wave.** |
| `it_native_sql` | `integration` | core | `SQLConnect`(string) | 18 | 12 | 0 / 6 | 0 | No. Blockers 1 and 2. Six cases use `utf8_to_wide`/`wide_to_utf8` (needs a test-local converter). **First wave.** |
| `it_handle_lifecycle` | `integration` | core | `SQLDriverConnect` | 14 | 12 | 0 / 2 | 0 | No. Blocker 1, plus the exact echo expectation (blocker 4). Two cases use `ODBCBackendTestAccess`. **First wave.** |
| `it_diagnostics` | `integration` | core | `SQLConnect` | 10 | 1 | 9 / 0 | 0 | No. The fixture uses `make_statement` to create a statement on a connection that was never opened. Correction found in Phase 1: no public call can create that state (a manager returns 08003 first), so those 9 cases stay direct-only. |
| `it_get_info` | `integration` | core | `SQLConnect`(string) | 11 | 0 | 10 / 0 | 1 | No. Only the fixture's `DatabaseFactory` profile is internal; a compile definition replaces it. Cheap. |
| `it_metadata_real` | `integration` | core | `SQLConnect`(string) | 75 | 0 | 54 / 21 | 0 | No. Helpers use `DatabaseFactory`; bodies use `utf8_to_wide`, `ConnectionString` and `ODBCBackendTestAccess`. Second wave. |
| `it_redshift_real` | `integration`, Redshift builds only, live | core | `SQLDriverConnect` from `ODBCPP_REDSHIFT_TEST_CONNECTION` | 157 | 0 | 110 / 47 | 0 | No. The fixture uses `ConnectionString::parse` for its SSL check and `rs::util::Clock`; replacing those makes 110 cases public. Needs live admission. Second wave, opt-in. |
| `it_redshift_columns_omitted_table` | opt-in, Redshift live | core | `SQLConnect` | 4 | 0 | 4 / 0 | 0 | No. The fixture uses `ConnectionString`, `configured_backend_provider` and `utf8_to_wide`. Live only. |
| `it_redshift_routine_schema_patterns` | opt-in, Redshift live | core | `SQLConnect` | 1 | 0 | 1 / 0 | 0 | As above. |
| `it_session_reset` | `integration`, PostgreSQL only | core | `SQLConnect` | 2 | 0 | 0 / 2 | 0 | No. It asserts session state through `ODBCBackendTestAccess`. Stays direct. |
| `it_session_baseline`, `it_session_owner`, `it_connection_pool`, `it_auth_pg_material_tls`, `it_redshift_native_auth`, `it_mysql_*` (5) | various, opt-in | core / SDK | none | 17 | 0 | – | 17 | Not applicable: they test SDK or backend sessions directly, without ODBC. Stay direct. |
| `it_driver_manager_unicode` | `integration`, manager | manager | `SQLDriverConnect` from `ODBCPP_DM_UNICODE_CONNECTION` | 8 | 8 | – | 0 | Yes, already. The iODBC skips become asserted deviations in Phase 3. |
| `it_redshift_powerbi_relationship` | opt-in, manager, Redshift | manager | `SQLDriverConnectA` | 2 | 2 | – | 0 | Yes, already. |
| `it_redshift_package_live`, `it_redshift_package_import_live` (and `it_redshift_official_import_live`, built from the same source) | opt-in, manager, installed package | manager | `SQLDriverConnect` | 6 | 6 | – | 0 | Yes, already. |
| `it_crypto_profile_live` | opt-in, manager | manager | `SQLDriverConnect` | 3 | 3 | – | 0 | Yes, already. |
| `it_redshift_max_length_reference` | no CMake/CI registration found | manager headers | `SQLDriverConnect` | 1 | 1 | – | 0 | Public-only source with no registration. |
| `it_driver_manager` (smoke program, not GoogleTest) | `integration`, manager | manager | – | – | – | – | – | Yes, already. |

## Recommendation for what to migrate first

1. **Phase 1, wave 1: PostgreSQL integration suites through unixODBC in CI, and through iODBC locally.** Suites: `it_prepared_statements_real`, `it_bind_col_real`, `it_native_sql`, `it_handle_lifecycle`, `it_diagnostics`, `it_get_info`.
   - These provide 252 public cases. Replacing their fixture helpers adds up to 37 more, for up to 289 cases:
     - the `DatabaseFactory` profile helpers in `it_prepared_statements_real`, `it_bind_col_real` and `it_get_info`;
     - the `make_statement` helper in `it_diagnostics`. (Corrected in Phase 1: those 9 cases can't migrate. Results are in [driver-manager-phase1-wave1.md](driver-manager-phase1-wave1.md).)
   - The 13 cases with internals in the body stay direct-only, compiled out of the manager build by a test define.
   - Changes needed:
     - manager include and link variants of these targets;
     - `SQLDriverConnect` (or a DSN name) in the fixtures;
     - compiled-profile selection from a compile definition (`TARGET_DATABASE`), not `DatabaseFactory`;
     - a test-local UTF-8/SQLWCHAR converter in place of `rs::odbc::utf8_to_wide`;
     - echo buffers sized from the input.
   - CI already registers `RedshiftProd` for unixODBC and iODBC.
   - **Estimate:** about 2 working days for the source and CMake changes plus the first local iODBC and unixODBC runs. Triage of new manager deviations comes on top and can't be predicted: PR #96 found four manager defects with 8 cases.
2. **Phase 1, wave 2: `it_metadata_real` and `it_redshift_real`.**
   - Replacing two fixture helpers makes about 54 + 110 more cases public.
   - `it_redshift_real` runs only under live admission.
   - **Estimate:** about 1.5 days of source work, plus a live session for `it_redshift_real` that the human must schedule.
3. **Phase 2/3: fake-backend cases.**
   - These are the 100 fake-backend cases, plus the `test_backend_contract` helper-only cases whose helpers mostly build fake result data (about 134). That is roughly 230 cases.
   - Almost all of them configure or read fake state, so the Phase 2 test backend needs named scenarios that carry results, errors and what the fake observed. That is the main design cost.
   - `RedshiftScriptedSessionTest` is the easiest start: one fixture, wire-level scripts, and 12 fake-backend cases in `test_redshift_special_types` and `test_redshift_floating_results`.
   - **Estimate:** 3–5 days for Phase 2 alone. Defer the estimate for migrating cases until Phase 2 shows how scenarios are expressed.
4. **Unit API suites with fixture-only internals:** `test_attribute_apis` (60), `test_diagnostics_apis` (14), `test_api_validation` (14 + 8 public) and `test_descriptor_apis` (12).
   - Most of them need only `make_statement` replaced by `SQLAllocHandle`, plus a fake or real connection.
   - They are natural Phase 3 shared-suite candidates once Phase 2 exists.
5. **Not candidates:** the 1,780 `no-odbc-entry` cases and the 419 cases with internals in the body. They remain the direct layer (plan principle 1).
