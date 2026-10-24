# Supported local commands

The Windows coordinator may explicitly issue a finite local pilot AWS session
from WSL using `python3 -m tools.development.redshift_operator_session
--output-directory /home/vahid/.local/state/odbcpp/operator-sessions/NEW_RUN`.
The parent must already be owner-only `0700`; `NEW_RUN` must not exist. The fixed
bootstrap `operator-bootstrap024/credentials.ini` must contain only the existing
pilot's long-term profile, with owner-only `0600` file / `0700` parent custody.
The issuer verifies the pinned CLI and pilot identity, requests 43200 seconds
(12 hours),
then verifies the temporary identity. It makes at most three STS CLI invocations,
without retries or ambient providers. Success leaves private `credentials.ini`
and secret-free `metadata.json`; failure leaves metadata and reports any
unverified process cleanup rather than erasing potentially active inputs.
Per-call exit, timeout and reap facts remain in the metadata after raw output
retirement. Late/failed metadata publication revokes success, retires exact owned
credentials, and records public failure facts in `failure.json` when writable.
Only credential validity is extended. Issuance remains bounded to 90 seconds;
operational runs retain their own finite admissions, deadlines and caps. There
is no duration option or automatic renewal.
Keep the bootstrap protected for future explicit issuance; retire each issued
credential file only after its consumers are reaped. This grants no resource or
live-test admission. Offline controls: `python3 -m unittest
tools.development.test_redshift_operator_session tools.redshift.test_pilot_runtime`.

Run from the chosen source checkout. CMake 3.21+, CTest, Python 3.9+, Git and the
platform compiler are required; Unix profiles use Ninja. `cmake --list-presets`
and `ctest --list-presets` discover the checked-in commands. Each configure preset
uses its own `build-<preset>` directory. `pg` means POSTGRESQL; `redshift` means
REDSHIFT. Host-default compatibility presets remain, but the explicit profiles
below bind driver-manager and SQLWCHAR ABI. MySQL product presets are unsupported
and removed; the old preset file is archived.

| Host/profile | Configure / build / test preset | Required local dependency selection |
| --- | --- | --- |
| Windows x64, native ODBC 2/2 | `windows-pg`, `windows-redshift` | VS 2022 x64 tools; pass `-DODBCPP_CRYPTO_ROOT=C:/absolute/OpenSSL3` |
| WSL/Linux or macOS, unixODBC 2/2 | `unixodbc-pg`, `unixodbc-redshift` | unixODBC development headers/library and system-shared OpenSSL 3 |
| WSL/Linux, iODBC 4/4 | `linux-iodbc-pg`, `linux-iodbc-redshift` | Set `ODBCPP_IODBC_ROOT` to an installed UCS4 iODBC prefix (`include`, `lib/libiodbc.so`) |
| macOS, iODBC 4/4 | `macos-iodbc-pg`, `macos-iodbc-redshift` | Set `ODBCPP_IODBC_ROOT` to its UCS4 prefix (`include`, `lib/libiodbc.dylib`); supply dependency prefixes via `CMAKE_PREFIX_PATH` as needed |

An iODBC build must use iODBC headers for both driver and application. Do not
silently pair UCS2 headers with UCS4 libraries. Other width/provider/sanitizer
profiles remain checkpoint obligations as applicable; these defaults do not
claim those profiles were executed. Use a new build for a changed crypto profile.

For example (replace the preset name for the owning host/profile):

```sh
cmake --preset unixodbc-pg
cmake --build --preset unixodbc-pg
ctest --preset unixodbc-pg -R '^test_connection_options$' --output-junit focused.xml
ctest --preset unixodbc-pg --output-junit full.xml --output-log full.log
ctest --preset unixodbc-redshift-offline --output-junit offline.xml
```

The unfiltered test preset is the full configured inventory. Its `-offline`
variant excludes the `integration` label and is **not a complete product/live
suite**. Configure the required opt-in TLS/live targets for the applicable
checkpoint before discovering its final inventory; defaults keep cloud tests off.
Inspect actual names with `ctest --preset <name> --show-only=json-v1` for local
nonsecret graphs. Never expose live registration metadata containing environments.
No stop-on-failure, failed-only rerun or feature filter belongs in a full collection.

Full PostgreSQL integration needs the approved local server, credentials, DSN,
TLS CA/hostname and fixtures supplied by its existing lifecycle owner. Presets
do not provision them. Native System DSN tests also need the approved local
elevated token and exact owned registry fixture setup/cleanup; a suite with those
missing is a setup failure, not a pass. Preserve existing PostgreSQL data and roles.

For Windows GUI checks, run locally in the actual interactive desktop:

```powershell
./tools/development/test_windows_dsn_gui.ps1 -BuildDirectory build-windows-pg -Configuration Release
```

This checked-in runner builds/tests its three GUI suites without registering a
system driver or requiring a database. It does not replace privileged full-suite
coverage. It owns bounded child cleanup and records native GUI evidence.

## Complete live collection

The existing [live registrar](../../tests/development/live_suite/README.md) is the
portable command surface for already-built owning binaries and complete applicable
case/profile inventories. It does not admit cloud use or prepare credentials.
Use the same registrar on Windows, WSL unixODBC/iODBC and macOS supported profiles.
Installed vendor/narrow-package tests may require macOS-specific targets; preserve
those unavailable boundaries rather than registering them against other binaries.

Before any resource admission, the standalone offline registration regression
runs real synthetic CTest processes (PASS, FAIL, crash, SKIP, later PASS and
zero-test) and verifies continuation and inventory rejection. On WSL/macOS:

```sh
python3 -B tests/development/live_suite/test_live_suite.py
```

The checked-in preflight command works on every host and validates the real
ordered inventory and image hashes through the registrar, using empty synthetic
environments. It never opens profile input files, executes images or contacts cloud:

```sh
python3 -B tools/development/live_preflight.py --index /absolute/public/index.json --expected /absolute/public/expected.json --output /absolute/new/preflight
```

Expected-inventory provenance and genuine unavailable boundaries still need review.

For a real admitted session, its lifecycle owner first validates exact source,
images, finite one-use admission, identity/TLS/network, complete applicability,
principal/fixture/input/process cleanup and pause. It then stages the private
per-profile inputs and invokes the **documented registrar CMake/CTest commands**.
CTest schedules all cases; the owner always reaps readers, restores fixture
permissions, verifies exact-principal/query zero, closes inputs and pauses even
after failure. No credential values belong in Git or public command output.

The normal checked-in [checkpoint entry and input schema](CHECKPOINT_INPUTS.md)
provide `prepare`, cloud-free `preflight`, and separately admitted `run` modes.
They compose fixed checked-in fixture/resource/input modules; no historical
private Python source is loaded. Windows uses its local WSL resource owner and
the checked-in native held-input/HKCU/job/CTest backend. macOS/WSL native owning
binaries use POSIX custody. Old optional composition APIs/receipts remain for
historical evidence and regression compatibility, not as the command surface.
No resource resume or live execution is authorized during workflow replacement.

## Compact evidence

Retain CTest JUnit, `Testing/Temporary/LastTest.log`, native GoogleTest XML,
driver diagnostics and the actual terminal exit. The evidence helper below
records source/config/image/inventory hashes and summarizes standard results;
it never executes tests, loads credentials or controls resources. Run it before
and after collection to record both image bindings. Hash only public inputs.

```sh
python3 -B tools/development/collect_test_evidence.py --source . --build build-unixodbc-pg --output before.json --inventory public-inventory.json --image build-unixodbc-pg/libodbcpp.so
# Run CTest through the owning lifecycle; record its actual exit, including nonzero.
python3 -B tools/development/collect_test_evidence.py --source . --build build-unixodbc-pg --output after.json --inventory public-inventory.json --image build-unixodbc-pg/libodbcpp.so --ctest-junit full.xml --ctest-exit 0 --gtest-dir /absolute/private/gtest --cleanup-receipt /absolute/private/cleanup.json
```

`--ctest-exit 0` above is an example, not a value to assume. Report creation is
not acceptance. Missing/incomplete XML, absent cleanup, image changes or unrun
inventory remain explicit debt. Review private output before publication.

## CI disposition

`primary.yml` now runs for PRs and explicit dispatch, removing the duplicate
branch-push and post-merge automatic macOS graph. Its existing macOS job/build/
tests and the `ci.yml` Ubuntu/Windows PR builds retain their names and behavior.
Checkpoint/focused job bodies are unchanged. Existing jobs reuse the build they
just produced for their tests. No hosted workflow is dispatched in this batch.

Cross-run artifact restoration/deduplication is not implemented: native build
graphs embed source/toolchain/dependency paths, and required branch checks and
tested-tree equality have not been proven for a replacement producer/consumer.
Do not skip a required checkpoint/branch check on the strength of this trigger
change. Use explicit dispatch for a new required exact tree; future restoration
needs real compatibility/applicability proof and branch-protection compatibility.
