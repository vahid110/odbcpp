# CodeQL extraction and publication

The security workflow builds the real REDSHIFT product before trusting C++
results. The 2026-10-25 no-build audit indexed nearly every file while recording
parser errors in 210 of 245 inferred compilations. It also omitted
`tests/unit/test_backend_contract.cpp` from both extraction and its baseline
denominator. A successful scanner process or a zero finding count was insufficient.

## Analysis profiles

| Profile | Actual scope | Evidence and limits |
|---|---|---|
| `cpp-redshift` | Fresh Ubuntu 24.04/GCC Debug REDSHIFT, C++20, OpenSSL system shared, unixODBC/SQLWCHAR2; core/driver, existing ordinary test sources and generated SDK header-isolation units | Actual CMake compiler records and source extraction must agree. Errors fail except the single pinned compile-time assertion limitation below. `test_backend_contract.cpp` remains independently required. |
| `cpp-source` | Supplemental no-build analysis of all tracked C++ sources and headers, with a C++20 frontend | Prevents dormant/platform/backend/optional code from disappearing when adding a real build. Requires observed C++20 platform telemetry and reports extraction quality without presenting inferred code as actual-build proof. Only the two exact existing omissions in `coverage-exceptions.json` are allowed; new omissions fail. |
| `python` | All tracked `.py` files, including workflow controls | Successful-extraction diagnostics must cover the Git inventory. This is not execution, secret-history or cloud admission proof. |
| `csharp` | All standalone tracked `.cs` files compiled as a library on Linux | Requires each source, actual-build and positive-code telemetry, a compiled library with retained compiler evidence, and no extractor/compiler-error or low-database-quality diagnostic. Does not execute Windows APIs or scan C# embedded in PowerShell strings. |
| `actions` | All tracked workflow YAML | Successful-extraction diagnostics must cover the Git inventory. Standalone PowerShell, shell, CMake and installer sources need their own review. |

All profiles run the default security queries plus `security-extended`. The audit's
`security-and-quality` experiment added thousands of maintainability results;
this recurring workflow keeps the security expansion without that unrelated
quality backlog. No source paths or security rules are filtered from SARIF.

The C++ build uses two workers and compiles, but never executes, tests. Two
existing REDSHIFT `EXCLUDE_FROM_ALL` test targets are built explicitly so the
actual compile-command inventory has no silent unbuilt targets. Their live-test
registration remains OFF. There are no services, PostgreSQL startup, DSNs, CTest,
native authentication, credentials, cloud calls, caches or restored binaries in
this workflow. It does not repeat the primary test matrix.

CMake configuration runs before CodeQL initialization for the manual profile.
Its architecture checks intentionally compile invalid private-header probes;
those expected failures are configuration evidence, not product translation
units. Initialization then precedes the unchanged actual build, so all product,
ordinary test and generated SDK header units remain traced and checked against
`compile_commands.json`. The configure log is retained. This ordering does not
turn an extractor error in an actual build unit into an accepted exception.

The job explicitly sets `CODEQL_OVERLAY_DATABASE_MODE: none`, disables TRAP
caching and keeps dependency caching disabled. These are separate controls in
the pinned action: dependency caching alone did not prevent automatic database
overlays and TRAP reuse in PR98 run `40313014470`. Its manual scan combined
cumulative compilation metrics with a current extraction summary containing
252 cache hits and no new partial unit, so the strict limitation check correctly
rejected it. Retain that original failure; do not widen the exception to accept
mixed extraction evidence. The
[pinned overlay configuration](https://github.com/github/codeql-action/blob/9b3254085494b55a0f7c4c2b3d2d52cc712ce370/src/config-utils.ts)
recognizes `none` as an explicit disable, and the pinned init action exposes
`trap-caching` separately from `dependency-caching`.

The supplemental profile also syntax-checks `dm_header_context.cpp` with actual
unixODBC headers. Its two TU-local defines select manager declarations and the
Linux SQLWCHAR2 ABI, then include the unchanged `tests/test_dm_odbc.h`. Without
this context, inferred shared-test compilations take the direct-driver branch
and never reach that guarded header. The unit adds no copied declarations,
global defines, product target or executable behavior. The existing all-source
inventory requires extraction of both this unit and the original header; neither
is an omission exception. A successful compiler smoke is not extraction proof:
the fresh hosted raw SARIF and extractor logs still require review. This follows
[GitHub's no-build context guidance](https://docs.github.com/en/code-security/reference/code-scanning/codeql/build-options-for-compiled-languages#accuracy-of-no-build-analysis-for-cc).

### Pinned compile-time assertion limitation

CodeQL 2.27.3 reports a failed private-member `requires` assertion at line 34 of
`tests/unit/test_session_owner.cpp` although the actual GCC build accepts it.
The supplemental Clang-based extraction reports the same diagnostic; this is
not established as a GCC-only issue. Original assertions and raw SARIF remain
unchanged. The publication gate accepts only that exact diagnostic with pinned
test/header SHA256, extractor version, path, line and complete message. It also
requires the actual GCC unit object, every owned compiled source/header unit,
zero syntax errors, exactly two compiler-error records in one partial compilation, and retained
logs proving the sole nonzero extractor exit is this unit's partial exit 3.
The two grouped `cpp/telemetry/compiler-errors` records must be exactly
`static assertion failed` = 1 and `There was an error during this compilation` = 1;
the latter is the failed-compilation marker. The pinned extraction metric counts
both records, while the clean-compilation count decreases by one. Missing,
duplicate, extra, renamed or differently counted buckets fail; this is not an
allowance for two arbitrary errors.
Missing evidence, drift, another error/partial compilation or any hard abort
fails. Original errors remain in the report beside `accepted_extractor_limitations`.

This is an accepted scanner limitation, not error-free extraction or proof that
every function body in that test TU was extracted. The retained hosted artifact
has no database/TRAP relations with which to establish that stronger claim.
[GitHub's extraction-error guidance](https://docs.github.com/en/code-security/reference/code-scanning/troubleshoot-analysis-errors/extraction-errors-in-the-database)
supports assessing a small number of errors in context; it does not justify a
general error budget. A future tool/source change requires renewed review of
this exact limitation. No runtime assertion or security query is suppressed.

The manual profile does not qualify POSTGRESQL-only composition, Windows setup/
registry/IOCP, Apple trust APIs, alternate ODBC widths, installed packages, runtime
TLS, or the default-off native AWS SDK modules. Those native modules require a
separate reviewed Darwin arm64/static OpenSSL/explicit SDK profile and explicit
targets. The broad source profile retains visibility, not universal coverage.

## Independent extraction check

`coverage.py` reads raw SARIF, successful-extraction diagnostics, the tracked-file
inventory, actual CMake cache and compiler commands. For the manual profile it
requires real C++20 arguments, both REDSHIFT composition compile records, every
owned compiled translation unit, every generated SDK header unit and six named
critical sources. It also requires positive complete compilation metrics and
zero extractor compiler/syntax errors except the exact documented limitation
above. It does not infer extraction from an
archive, baseline denominator, native compiler success or result count.
The supplemental source profile sets the supported `cpp.frontend_options`
extractor option to `--c++20` and independently requires all reported C++ platform
standards to be at least C++20. The first hosted run inferred C++17 and aborted
seven MySQL test-source extractions; these are failures to repair, not new
omission exceptions. The source inventory check remains required.
Its only CLI input is the allowlisted profile; repository and artifact paths are
fixed by the checked-in helper, rather than accepted from environment or caller.
Each profile requires exactly one raw SARIF file containing exactly one CodeQL
run with its expected language's extraction diagnostics. Additional files or
runs fail closed before aggregation, so another run cannot overwrite failed
extraction metrics. Raw input is preserved unchanged.

Publication follows that check. `analyze` writes raw SARIF with `upload: never`;
`upload-sarif` publishes only after coverage passes. Raw SARIF and the coverage
report are retained even when verification fails. This is not finding suppression:
the helper does not edit, delete or relabel any result. A rejected scan fails CI
and retains its evidence instead of publishing an apparent clean replacement.

The action code is pinned to reviewed official commits; `tools: linked` selects
the compatible CodeQL bundle shipped with that pinned action. Actual tool
versions and SARIF/input hashes are recorded. All jobs use standard Ubuntu
runners with a 60-minute limit, two query threads and a 4096 MB evaluator setting.
The first actual manual job completed its build but reached the previous
30-minute limit during query evaluation. The new limit is a finite execution
bound, not a prediction that every analysis will complete. C++ extractor logs and
diagnostics are retained alongside raw SARIF after failures, including timeouts.
That setting is not an OS-enforced process-tree memory cap. No automatic
in-progress cancellation or privileged pull-request trigger is used.

## Transition from default setup

Default setup and this advanced workflow must not be run as competing publishers.
The coordinator must complete independent review of the exact candidate before
changing GitHub settings. This implementation does not change settings itself.

1. Push the reviewed candidate branch when authorized. Its branch push does not
   trigger this workflow's `main` push graph.
2. Once the advanced workflow is ready for its first PR execution, the coordinator
   disables default setup and opens/updates the reviewed PR. Do not infer that
   adding this file disables default setup or makes custom uploads acceptable.
3. Follow one exact-head hosted execution through all four profiles. Inspect
   actual compiler commands, zero manual extraction errors, file inventories,
   raw SARIF, successful coverage reports, upload processing and categories.
   Local offline tests do not establish hosted build/extraction acceptance.
4. After reviewed integration and a successful exact-main advanced scan, inspect
   historical default-setup categories for stale configurations. Any retirement
   is a separate coordinator decision; do not bulk-dismiss findings as migration.

The categories include both language and profile, so manual and supplemental C++
results cannot overwrite each other. This workflow does not claim an OpenSSF
Passing badge, cross-platform acceptance or a clean security assessment merely
because its checks are green.

## Ownership and focused qualification

| Component | Owner | Rationale / allowed dependencies | Proof |
|---|---|---|---|
| Scheduling/build/extraction/publication | `.github/workflows/codeql.yml` | Repository CI policy; pinned official actions, standard CMake, scanner helper | Actionlint and independent workflow review; actual hosted run pending |
| Security suite and exact inferred omissions | `.github/codeql/config.yml`, `coverage-exceptions.json` | Scanner policy, not SDK or product behavior | Pinned compatible suite resolution; explicit per-path boundary review |
| Evidence verifier | `.github/codeql/coverage.py` | CodeQL/CMake/Git evidence only, Python standard library; no product/SDK API | Offline rejection tests and raw audit-SARIF smoke |
| Control regression tests | `.github/codeql/test_coverage.py` | Synthetic scanner records, no feature assertions or live scheduling | `python3 -B -m unittest discover -s .github/codeql -p 'test_*.py' -v` |
| Supplemental manager context | `.github/codeql/dm_header_context.cpp` | Scanner-owned TU; original manager header and installed unixODBC headers only; no runtime or SDK ownership | Actual syntax-only compilation, unchanged header guard/ABI rejection checks, full source-inventory extraction |
| Workflow boundary controls | `.github/codeql/test_workflow.py` | Standard-library offline checks for fresh extraction, compiler context and trace ordering | Explicit cache/overlay rejection cases; no cloud or product execution |

These helpers belong beside the scanner configuration because they interpret CI
artifacts; they are not reusable provider-neutral runtime foundations. SDK,
backend, ODBC, tests/CMake and product contracts are unchanged.

Primary references: [compiled-language build modes](https://docs.github.com/en/code-security/reference/code-scanning/codeql/build-options-for-compiled-languages),
[query suites](https://docs.github.com/en/code-security/concepts/code-scanning/codeql/codeql-query-suites),
[pinned init inputs](https://github.com/github/codeql-action/blob/9b3254085494b55a0f7c4c2b3d2d52cc712ce370/init/action.yml),
[pinned analyze inputs](https://github.com/github/codeql-action/blob/9b3254085494b55a0f7c4c2b3d2d52cc712ce370/analyze/action.yml),
[multiple SARIF categories](https://docs.github.com/en/code-security/how-tos/find-and-fix-code-vulnerabilities/integrate-with-existing-tools/upload-sarif-file),
[default-setup transition](https://docs.github.com/en/code-security/reference/code-scanning/troubleshoot-analysis-errors/results-different-than-expected).

## Standalone C# scope

The C# profile compiles the original `tools/development/windows_local_process.cs`,
the Windows job/process helper, through `standalone-csharp.csproj`. The project
declares `OutputType=Library`, targets .NET 8 and adds no duplicate source or entry
point. The compiler runs after CodeQL initialization, with fresh compilation and
compiler servers disabled. It neither starts the helper nor validates P/Invoke
behavior. The two C# regions
embedded in `test_windows_local_process.ps1` and `test_windows_dsn_gui.ps1` remain
outside this tracked `.cs` inventory and require separate review.

The pinned query source explains why source presence is insufficient:
[`ExtractedFiles.ql`](https://github.com/github/codeql/blob/a1575b98a8cc19bcfa7bb6cb41be688cb3dae65c/csharp/ql/src/Diagnostics/ExtractedFiles.ql)
selects source files even if their extraction had errors. The publication check
also rejects `cs/diagnostics/extraction-errors` and
`csharp/diagnostic/database-quality`, and requires positive code and actual-build
telemetry from `cs/telemetry/extraction-information` (the no-build flag must be
zero). It records the library and compiler-evidence hashes. All tracked `.cs`
sources must still appear in extraction diagnostics; adding another source without
including it in the library project fails the offline inventory control and the
hosted publication gate.

The first C# hosted attempt on `38747ca` retained one `CS5001` compiler diagnostic:
the pinned no-build extractor constructs a console application even for this
Add-Type library helper. The original failed SARIF remains retained. Its
[`Extractor.cs`](https://github.com/github/codeql/blob/a1575b98a8cc19bcfa7bb6cb41be688cb3dae65c/csharp/extractor/Semmle.Extraction.CSharp.Standalone/Extractor.cs)
sets `OutputKind.ConsoleApplication`. The manual library build corrects that
compilation context rather than suppressing the diagnostic. Actual successor
compilation and SARIF qualification remain hosted acceptance requirements.

Primary build references: [CodeQL manual builds](https://docs.github.com/en/code-security/how-tos/find-and-fix-code-vulnerabilities/manage-your-configuration/codeql-for-compiled-languages),
[C# library output](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/compiler-options/output#outputtype).
