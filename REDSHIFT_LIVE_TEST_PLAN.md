# Initial Redshift live-test handoff

Recorded 2026-10-02 from the user's account-preparation chat. The user subsequently
paused other implementation and assigned environment completion to this chat.
Provisioning and admission now share this integration owner and the existing
canonical state; do not create duplicate infrastructure in another chat.

## Approved allowance amendment and work priority — 2026-10-02

The user increased the shared initial Redshift allowance from **USD 15 to USD
150 total**, and prioritized Redshift over MySQL for the coming week. This is
one cumulative allowance across all chats, including prior costs/reservations;
it is not a fresh USD 150 balance, monthly budget or per-chat allowance. Plan a
USD 100 compute envelope with USD 50 retained for other charges/tax. Keep the
existing 4/4-RPU capacity and single workgroup. No additional infrastructure or
capacity increase is authorized by the allowance change.

The protected `allowance-amendment-150.json` records the user's authorization
under the canonical lock. A tested, independently reviewed v3 amendment now
preserves the entire v2 history and original USD 15 anchors verbatim in the same
ledger. Unknown actual spend/remaining allowance remain null; all compute bounds
are retained and the full USD 50 other/tax reserve is added. AWS readback verifies
a USD 150 budget, USD 100/125/150 alerts with subscribers preserved, and a 267
RPU-hour monthly deactivate limit (USD 99.858 at the verified rate). One workgroup
and base/max capacity 4/4 are unchanged. These delayed cloud controls are not a
hard USD cap and monthly resets cannot reset cumulative pilot accounting.

The migration and cloud controls grant no SQL admission. Old launchers fail
closed on v3. The next bounded package must add reviewed v3 window admission,
then repair/test the catalog baseline; do not reuse old expired anchors, hashes
or paid one-shot launchers. Historical status below describes original setup
evidence, not the newly authorized ceiling. All 124 offline pilot tests pass;
8 new amendment tests cover history preservation, wrong authority/caps, uncertain
prior attempts, stale/wrong controls, duplicate migration and crash uncertainty.

The allowance amendment CI (37027761397 for 7cf8bc7) is green. The next
package has an independently reviewed offline v4 ledger: explicit v3-to-v4
migration preserves the complete allowance and original history; it permits at
most eight additional reservations, retains every metering/cleanup allowance,
and requires fresh cleanup after reservation before activation. Ten synthetic
window tests cover migration, retained costs, full launcher headroom, finite
attempts, timestamp ordering, restart and interrupted persistence; all 134
pilot tests pass locally. This ledger grants no live SQL permission. The bounded
launcher has also passed independent offline review and 12 composition/control
tests (146 total pilot tests). It admits one private sequence-bound request at a
time, pins identity and a finite scalar/catalog/baseline inventory, obtains fresh
AWS controls plus official pricing and the test machine IPv4 address, reserves
before any cleanup SQL, and verifies independent final cleanup. It performs no
fixture DDL or automatic retry. Native logs/XML stay private; reports expose only
case counts and fixed reasons. The combined package CI and paid qualification
remain pending; canonical protected state stays v3 until those checks complete.

The bootstrap accounting horizon remains 24 hours from original resource
creation. Continuing beyond it requires a separate reviewed cumulative
billing/usage reconciliation; the weekly absence and larger allowance do not
silently extend that horizon or reset unknown liabilities. A launcher must add
1,200 seconds of conservative minimum-metering headroom and 60 seconds of
cleanup headroom per finite window; successful cleanup releases neither.

CI run 37024167587 for 47b2c13 is green across required platform gates. Prioritize
bounded Redshift baseline/catalog work. MySQL remains deferred. The user asked
for no questions during the coming week; record work needing further authority
and continue useful work within the approved scope instead of prompting them.

## Environment preparation status — 2026-10-02

A dedicated named AWS CLI profile has passed a fresh-process identity check with
ambient AWS environment variables removed. Its protected local credentials do
not depend on the root browser session. One Stockholm namespace/workgroup exists;
AWS readback confirms base and maximum capacity both 4 RPUs, required TLS, a
single-host IPv4 ingress rule on port 5439, and a 20 RPU-hour monthly usage limit
whose breach action deactivates queries. The USD 15 pilot budget and USD 10/12/15
alert thresholds have been read back. Resource IDs, credentials, policy drafts
and observations remain in the protected canonical directory.

The user confirmed the scoped IAM update. Exact-resource operator access,
immutable permissions boundary, fixed test-role trust and unattended STS role
assumption are verified. One-time IAM creation grants were removed. Some regional
usage-limit APIs do not support resource ARN scoping; their caller must pin the
canonical limit ID. Budget/capacity increases still require user authorization.

The environment has completed unattended live setup. Verified evidence includes
`verify-full` certificate/hostname checks, an ODBC driver connection to Redshift,
Redshift identity, the dedicated restricted test user, the two-row fixture,
prepared scalar/NULL results, row fetching and recovery after invalid SQL. All
six admitted test cases executed. Fresh SYS monitoring independently verified
zero active pilot sessions and queries after cleanup. The fixed IAM role also
successfully generated Serverless credentials without browser/MFA or a SQL call.
Temporary credential generation is qualified; IAM-authenticated SQL remains a
later driver test, not a completed claim.

The initial fixture script exceeded an overly short whole-script bound. The
corrected script handles partial setup, preserves existing pilot users, recreates
only the owned fixture table and has a 90-second whole-script bound with the
existing 30-second server ceiling. Driver query limits remain 15 seconds.
STV monitoring was replaced by Serverless SYS views; termination executes on the
leader independently from the system scan, and monitoring verification allows
the existing 30-second server ceiling.

Live driver evidence exposed one catalog gap: `SQLTables` succeeded but
`SQLColumns` failed for the configured fixture. Record this as Redshift driver
work before catalog/parity acceptance; environment readiness does not close that
gate. The error-path test expected native `42P01`, although the existing ODBC
mapping correctly returned `42S02`; its assertion is corrected and needs the
next admitted driver run. Recovery itself completed successfully. Do not report
the full driver baseline as green.

The protected original anchor and v1 reservation are preserved verbatim inside
an explicit v2 cumulative overlay in the SAME `setup.json`. Its single increasing
cost bound includes all elapsed time from original resource creation, retained
minimum-metering and cleanup headroom, and the full USD 5 other/tax reserve.
Actual spend and remaining allowance remain null while billing is unavailable;
no refund or period reset occurs. Latest conservative total bound is USD 8.76,
not an actual-charge or remaining-balance claim. Both recorded attempts retain
cleanup and failure history. Fresh resource/price/network checks, durable
reservation before SQL and fresh cleanup before activation are required. No
automatic failed-SQL retry or additional warehouse is allowed. The finite ledger
supports separately reviewed admissions; the corrected qualification entrypoint
itself permits only one attempt.

Protected JSON rejects duplicate keys and unreviewed admission fields. Processes
receive minimal environments; local process-group cleanup and exact completed
case inventories are independently checked. Offline safety tests pass (116 cases),
and the fresh Redshift build passed 65 unit tests before the live qualification.
The setup code is committed and CI run 37024167587 is green. This includes the
Linux pinned MySQL fixture, PostgreSQL, sanitizer, crypto, iODBC and Windows live
and packaging gates; SQL Server linked-server acceptance was skipped as planned.
The separately added Python allowance amendment needs its own CI qualification.

AWS access no longer requires the user's browser or passkey. The Mac must remain
online and awake. General SDK work remains paused during final setup handoff;
the existing 15-minute timer now prioritizes Redshift under the USD 150
amendment above. It must not restart MySQL work or repeatedly relaunch failed
driver tests without reviewed admission. The original USD 15 evidence remains
historical and immutable.

## Shared allowance and source of truth

The original setup allowance was **USD 15 total for one initial testing period across
all chats**, never per chat, month, scheduler invocation or CI run. Target a
USD 10 compute envelope and reserve USD 5 for other charges and tax. Do not
increase limits, create additional warehouses or exceed the allowance without
the user's authorization. Assume paid usage: zero active credits was reported;
the account plan and trial eligibility remain unverified. No automatic budget
reset or trial assumption is allowed.

The local canonical coordination state is
`/Users/vahidsbr/.local/state/odbcpp/redshift-pilot/setup.json`.
It records account identity, reported versus verified status, setup details,
usage windows, reservations and remaining allowance. Unknown actual spend and
remaining allowance are null, not zero spend or USD 15 available. Protect this
directory and any credential configuration; never commit credentials or copy
them into chat, command-line arguments, test output or logs. Preserve an existing
state file rather than initializing a second ledger in another worktree.

## Provisioning handoff and activation gate

1. Verify account plan and applicable pricing/credits. Record the verified
   Stockholm RPU-hour rate and the check date; do not invent a dollar-to-RPU
   conversion. One Serverless workgroup in `eu-north-1`, base 4 RPUs, and the
   lowest supported maximum capacity are proposed. Read back the actual values;
   do not assume the maximum equals the base or accept service defaults.
2. Create and read back RPU-hour usage limits with **turn off user queries**,
   plus budget alerts. Translate the shared compute envelope using the verified
   rate and reserve headroom. Recurring AWS usage-limit periods must not reset
   this pilot's total allowance. Alerts and delayed billing are not a hard
   USD cap. Include setup queries and monitoring queries in usage accounting.
3. Verify TLS, host identity and access restricted to the test machine. Use a
   dedicated database test user and a tiny schema/table. Defer Serverless
   `GetCredentials` IAM authentication until database-credential baseline passes.
4. Supply the protected local connection configuration containing
   `ODBCPP_REDSHIFT_TEST_CONNECTION` with explicit `SSL=1`, plus
   `ODBCPP_REDSHIFT_TEST_SCHEMA` and `ODBCPP_REDSHIFT_TEST_TABLE`.
   Record only the configuration path in the coordination state. The current
   test fixture rejects a missing endpoint and requires Redshift server identity;
   PostgreSQL simulation is not live evidence.
5. Verify server-side statement/runtime limits appropriate to the small test
   window. A client timeout or killed process does not prove server query
   cancellation or zero subsequent compute. Record query-cleanup verification
   and unresolved sessions before allowing the next window.

Do not enable live testing until endpoint, controls, fixture, protected
configuration and the shared runner below are verified. Backup MFA is deferred
as reported by the user; no authentication changes are authorized here.

## Required shared runner

Implemented and exercised through the two recorded bounded setup windows.
The admission is finite: this does not enable general paid test execution,
scheduler retries or a cloud integration job. Historical requirements follow;
the cumulative overlay above governs the reviewed unknown-billing exception.

- Use one canonical cross-chat/worktree lock and ledger. Acquire the lock before
  usage checking or reserving a window; fail closed when another runner owns it.
  Do not auto-delete a stale lock while a client or server query may still run.
- Before every batch, validate workgroup identity, current base/max capacity,
  usage-limit action and available shared allowance. Record fresh usage and
  billing observations with timestamps and their reporting delay. Unknown or
  failed checks block execution. Account-preparation activity uses the same
  ledger and lock; a local lock cannot constrain unrelated cloud-console use.
- Reserve a conservative window charge from verified rate, maximum capacity,
  execution bound and billing/cleanup headroom before connecting. Retain uncertain
  reservations until reconciliation; do not refund elapsed client time as proof
  of unused compute. Keep total and compute envelopes separately enforced.
- Run only the reviewed `it_redshift_real` executable from a Redshift build,
  with a bounded baseline filter and explicit timeout. Build and debug locally
  first. No parallel live batches, load tests, automatic retries, provisioning,
  capacity increases or unrelated arbitrary commands.
- Handle signals/timeouts, close connections and verify outstanding server work
  is cancelled/terminated. Record unresolved cleanup and block subsequent runs.
  Bound process-tree execution rather than only its parent process.
- Capture test output privately. Publish sanitized case names, return codes,
  timings and result counts; omit connection strings, identities, native server
  text and credentials from shared reports. Redact/disable driver logs too.
- Record UTC start/end, verified controls, usage observations, reservations,
  reconciled spend, remaining allowance and cleanup status in the canonical
  ledger. Offline happy-path and failure tests must cover lock contention,
  exhausted/unknown budget, stale checks, malformed controls, timeout, signal,
  secret-bearing output and failed cleanup before the runner is enabled.

## Teardown and release sequence

Coordinate teardown with the provisioning chat under the same lock: verify no
active test/query, retire the fixture/user as appropriate, remove the workgroup
and namespace according to the agreed snapshot policy, and inspect residual
snapshots/storage/network/logging charges. Record actual resource IDs and
teardown commands after provisioning, not guessed names or destructive generic
commands now. Stop future test windows and reconcile delayed charges.

Infrastructure and bounded runner readiness are verified. **Redshift M2 remains
open on the driver catalog failure and baseline acceptance**, not AWS sign-in.
General implementation is paused during setup handoff. After handoff, review M2
scope before expanding parity, IAM or fetch-mode implementation. S2C, PostgreSQL
application acceptance and existing protected gates are unchanged.

References checked for planning:
[Stockholm 4-RPU support and capacity controls](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-capacity.html),
[usage-limit actions](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-workgroup-max-rpu.html),
[Serverless billing and metering](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-billing.html).

## Authorized parallel activation packages — 2026-10-02

The user initially authorized a bounded parallel Redshift activation track, then
prioritized setup completion and paused MySQL S3. Authorization does not bypass
the activation gate or
increase the shared USD 15 allowance. One integration owner reviews isolated
packages before accepting them. Infrastructure completion is now owned by this integration chat; do not
duplicate its resources or ledger.

| Package | Deliverable | Stopping condition |
| --- | --- | --- |
| R0: offline admission foundation | Strict verified-observation schema, conservative reservation arithmetic and canonical nonblocking lock helpers, with temporary-directory safety tests | Reviewed tests pass; no live launcher, AWS calls or live-enable claim |
| R1: qualified shared runner | Fresh AWS control/usage readback, protected configuration loading, fixed executable/filter admission, durable reservations, bounded process-tree execution, private output and verified server cleanup | Offline happy-path and adversarial tests pass; unresolved cleanup and unknown costs block further windows |
| R2: environment admission | Independently verified account/pricing, one workgroup, capacity and usage limits, alerts, restricted access, dedicated user, tiny fixture and private configuration | Readback evidence satisfies all activation gates; no credentials in Git/chat/logs |
| R3: first live baseline | One reserved short window running connection, Redshift identity and scalar/row baseline checks; reconcile usage and cleanup | Record sanitized evidence and M2 scope review before IAM, metadata expansion, parity or fetch work |

R0 is necessary but insufficient for paid testing. Caller-supplied observations
are not AWS verification, an offline lock test is not a qualified live runner,
and a client exit does not verify server cleanup. R1 and R2 may be prepared in
parallel, but both must pass before R3. No unattended paid retry is permitted.

The fixture now suppresses user identity/native diagnostic output and checks
explicit connection/query timeout settings. Private capture remains required
and must not be mistaken for safe shared evidence.

R1 review also requires an explicit final `SQL_NO_DATA` assertion in the
five-row fetch test, exact fixture metadata assertions, and checked handle
cleanup results. Server identity must pass in a separate first phase before
admitting the bounded baseline phase, since GoogleTest otherwise continues
running other cases after an identity failure. Pin executable identity, exact
case inventory and filters; reject skipped, missing or repeated cases. Persist
the reservation before the first connection, keep one bound across both phases,
and retain uncertain charges until independently reconciled. Client-side
cleanup checks complement, rather than replace, server-side cancellation proof.

### R0 offline foundation candidate

`tools/redshift/pilot_preflight.py` provides sanitized strict observation and
accounting validation, conservative max-capacity reservation arithmetic and a
stable nonblocking POSIX lock at the canonical pilot path. Its CLI only validates
supplied records and always returns `live_enabled=false`; it neither verifies
AWS evidence nor mutates the canonical ledger. Nineteen focused tests exercise
budget boundaries, unknown/delayed liabilities, freshness, unsafe paths and
permissions, cross-process contention, secret-bearing invalid inputs and nested
duplicate JSON fields. The existing Redshift build job runs these offline tests.

Reservations contain conservative upper bounds, not actual billing. R1 must
enforce the outstanding-to-reconciled transition through its locked atomic
writer and matched billing evidence; a caller changing a status field does not
prove reconciliation. Admission must bind the original pilot period and account
to protected canonical state, rather than accepting a new caller-defined period
as a budget reset. No live runner or environment qualification is claimed here.
