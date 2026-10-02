# Initial Redshift live-test handoff

Recorded 2026-10-02 from the user's account-preparation chat. The user subsequently
paused other implementation and assigned environment completion to this chat.
Provisioning and admission now share this integration owner and the existing
canonical state; do not create duplicate infrastructure in another chat.

## Current bounded evidence and next qualification package

At105cc87, CI37047693730 passed all required gates. Catalog window005 passed
identity and the configured fixture metadata case, including legacy SQL_C_LONG
numeric reads; independent cleanup succeeded and the reservation remains
retained. Scalar4/4 was qualified separately atfbbefbb. These are narrow fixture
observations, not full catalog/type/datashare/SHOW parity or G1/M2 completion.

The next package starts with an offline fixed-role temporary database credential
contract. It performs no AWS calls, SQL, refresh, admission or launcher enablement.
It fixes role/account/session, database/workgroup and expected IAM-derived DB
principal; validates expiry against the execution deadline plus cleanup margin;
keeps secret representations redacted and encodes connection-string delimiters.
Before live qualification, integrate fresh role identity/GetCredentials, include
that exact principal in independent cleanup, add exact IAM case inventory and
adversarial offline runner tests, then complete review and CI. One future reviewed
window may qualify externally supplied temporary credentials through ODBC;
it cannot establish native SDK IAM discovery, renewal or server expiry behavior.
GetCredentials minimum900-second validity exceeds a180-second window, so expiry
refusal is an offline gate and actual server-expiry semantics remain open.

The next candidate runner now integrates a fixed `iam` inventory. After durable
reservation and IAM-inclusive pre-activation cleanup, it assumes the existing
role once, verifies fresh STS identity, and obtains one900-second GetCredentials
response. AWS secrets stay in memory/ephemeral child environments; DB credentials
are revalidated and passed only to private driver environments. Identity runs
first, followed by exact principal/scalar/NULL/timeout/disconnect and invalid
password rejection cases. No refresh, fixture grants or permanent-config mutation
is added. Independent cleanup resolves the exact approved IAM-derived principal
alongside admin/test users for termination and both active-session/query counts.
Exchange, expiry, identity and driver failures retain consumed liability and run
independent cleanup. This remains offline-reviewed candidate code until full CI
and a separately reviewed request; no006 has run and no IAM SQL is claimed.

Five of eight finite v4 windows are consumed. No automatic006 or replay001-005.
The original24-hour horizon is unchanged; independently review cumulative
billing/usage reconciliation before subsequent-day admissions. No additional
warehouse, capacity or IAM privilege expansion is part of this package.

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
case counts and fixed reasons. CI37032653550 for fbbefbb passed all required gates. Canonical state is now
v4, and window001 passed identity plus all four scalar cases with independent
cleanup. Actual spending and remaining allowance remain unknown; successful
cleanup retains reservations.

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

## First catalog diagnostic — 2026-10-02

A separately reviewed window002 ran the rebuilt fixed metadata case once,
following successful Redshift identity and fresh controls/reservation. SQLTables
passed; SQLColumns failed with SQLSTATE `42000`, a native syntax error near the
PostgreSQL-only `domain_chain` subquery. Raw diagnostic/XML stays private. Final
remote cleanup passed and the window remains consumed with all liability retained.
This is useful failure evidence, not a qualified catalog baseline or an automatic
paid retry. The next package needs an explicit immutable catalog profile selected
by product composition, a separate Redshift builder, and PostgreSQL regression
protection; shared ODBC/SDK orchestration must not infer the backend from names,
ports or versions. PostgreSQL domain-resolution SQL remains unchanged.

The repair introduces a private immutable PostgreSQL-wire-family catalog
profile selected explicitly by product composition and propagated by the
provider into the session. PostgreSQL column construction retains its existing
domain resolution; Redshift column construction uses a separate SVV_COLUMNS
projection into the same 18 ODBC fields. Quotes and backslashes are escaped
according to Redshift literal rules. Unit tests protect profile propagation,
PostgreSQL isolation, ordering, empty/omitted filters and escaped patterns. The
fixed live case checks field count, integer/varchar types and dimensions,
nullability, ordinals, and empty/escaped-wildcard no-match cases. This is a bounded
local-table repair: other catalog requests still use existing builders; numeric,
temporal, unknown types, external/datashare and SHOW behavior remain unqualified.

Window003 was consumed during attempted repaired baseline qualification, but
its pre-activation cleanup-count query was cancelled at the 30-second server
limit. No driver cases ran. Independent final cleanup then verified zero active
sessions/queries and retained the reservation. The repaired catalog remains
unqualified live; do not label this as an SQLColumns failure or automatically
retry the consumed request. Complete the offline/platform batch and review a
separate bounded catalog request before further paid work.

Window004 passed pre-activation, identity and execution of the repaired catalog
SQL. Numeric metadata reads then exposed shared ODBC rejection of the legacy
SQL_C_LONG target; the live metadata case remains unqualified. Independent
cleanup succeeded and the request is consumed. The next bounded code package
supports legacy signed C targets through the existing checked shared conversions,
preserving descriptor identifiers and the original live assertions. After its
platform gates pass, review a new catalog request with the current binary hash;
never automatically retry004 or reopen earlier windows.

AWS documents the fields available in
[SVV_COLUMNS](https://docs.aws.amazon.com/redshift/latest/dg/r_SVV_COLUMNS.html),
and recommends SHOW COLUMNS for discovery across local, datashare and external
contexts. A bounded local-fixture repair may use an explicit view-backed profile;
SHOW adaptation, datashare/external behavior and full feature/behavior parity
remain separate planned work and must not be claimed from two fixture columns.

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
open on compatibility assessment and scope acceptance**, not AWS sign-in. The
configured fixture catalog failure is repaired and narrowly qualified by005;
chosen IAM SQL evidence and full parity are not established by that result.
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


## Cleanup-only recovery after IAM window006 — 2026-10-02

CI37056942333 at1bede4e passed all required gates. Window006 was reserved
once, but preactivation cleanup verification and final termination timed out.
No IAM credential exchange or driver cases ran. Its phase remains uncertain,
with all liability retained; further paid qualification is stopped. Successful
termination before the first timeout does not prove zero active sessions or
queries. Windows001–006 must not be replayed.

The offline query repair separates session and query absence into two
`SELECT EXISTS` checks with unchanged exact database/admin/test/IAM principal
predicates and own-session exclusion. Both must explicitly return false.
Termination failure, timeout, empty or malformed output stops cleanup. No
historical cutoff is allowed. This permits early exit when activity exists;
absence can still require a historical scan, so performance and live recovery
remain unqualified. Independent review accepted the query repair and all162
offline safety tests passed.

Before any further SQL, implement and review one cleanup-only accounting path:
preserve the complete v4 state and006 result verbatim with digests, append a
separate recovery reservation under the canonical lock, and durably add1200s
metering plus60s cleanup before dispatch. Recompute the continuous conservative
bound from original resource creation, retaining every prior reservation and
unknown cost, under unchanged100compute/150total caps and original24h horizon.
Use fresh fixed-resource, control, official price and exact-host network evidence.
Allow one cleanup-only operation under a single60s monotonic deadline; allow no
credential exchange, driver cases, provisioning, grants or retries. Record
success separately only after termination and both absence checks succeed.
Failure or interruption retains the additional liability and blocks admission.
Do not rewrite006 to imply its original cleanup succeeded. Recovery neither
qualifies IAM SQL nor authorizes window007. Later-day admissions still require
independent cumulative billing/usage reconciliation.


The next offline accounting candidate, `tools/redshift/pilot_recovery.py`, uses
one explicit v5 overlay in the same canonical setup file. Reservation snapshots
the exact original v4 and006 result UTF-8 bytes (including line endings), with
SHA-256 digests, then appends one cleanup-only record targeting uncertain006.
The original result file remains unchanged. Start requires a durable reservation;
completion requires fresh explicit absence evidence within its60s deadline.
Unknown billing stays null, successful cleanup retains all additional headroom,
and interruption or failure prevents replay. Old v4 qualification launchers
reject v5 even after recovery success. Independent offline review accepted this
accounting package. It supplies no launcher, controls verification, SQL admission
or permission to run007; those remain a separate finite review package.


The offline cleanup-only launcher candidate, `pilot_recovery_live.py`, binds its
request to the exact source ledger/result byte digests and a digest of the fixed
runner source inventory. Fresh read-only AWS/control/price/network verification
precedes reservation; request/deadline/code are checked again after these reads.
The reservation itself checks both expected source digests against the bytes it
freezes, preventing changes during control reads from migrating unreviewed
history. One durable consumed start precedes one cleanup call. All SQL shares
one remaining deadline of at most60s, with no deadline reset, exchange, driver
cases, second cleanup or retries. Failed reservation/start persistence prevents
SQL; failure after dispatch retains liability. Report failure cannot permit
re-entry. Tests include mutation of both source files during controls, failed
persistence, missing report, scope/hash/controls rejection and interruption.
All177 offline safety tests passed and independent review accepted the corrected
binding and persistence paths. This is an offline candidate: no recovery request,
canonical migration, AWS call or SQL ran while developing it. Live recovery needs
required green CI and a separate explicit admission of this exact cleanup scope.


## Remaining original windows after cleanup recovery

The cleanup-only runner at a6d8b7e passed all required CI gates. A separately
reviewed live recovery terminated owned sessions and both independent session
and query EXISTS checks returned false. V5 records successful recovery separately;
its exact frozen v4 and006 result retain006 as uncertain. Conservative retained
bounds are USD18.10 compute/USD68.10 total, not actual billing or remaining
balance. No IAM exchange, driver cases or window007 ran in recovery.

The offline `pilot_continuation.py` candidate explicitly freezes complete v5
raw history and its digest into a v6 overlay in the same canonical state file.
It retains recovery and prior headroom and admits only the original remaining
sequences007/008, each adding1200s metering and60s cleanup under unchanged
100compute/150total caps and original24h horizon. Successful recovery is the
migration prerequisite; unresolved continuation attempts block new reservations.
Fresh matching cleanup after durable reservation still precedes activation.
Recovery cannot be restarted through this interface. Six additional tests and
independent review accepted the accounting candidate; all183 offline tests pass.
This adds no continuation launcher, verified admission or canonical migration.
Keep paid qualification stopped until a separately reviewed runner and required
green CI, followed by root's explicit admission of a new finite scope. Later-day
admission still requires cumulative billing/usage reconciliation; do not extend
the accounting horizon by editing an anchor or creating a new pilot period.


The explicit continuation runner candidate `pilot_continuation_live.py` accepts
only007/008 against separately initialized v6 state. The original v4 entry point
still rejects later schemas. A reviewed request binds current runner sources,
executable hash and exact current v6 state bytes; reservation verifies the state
binding inside its durable operation after fresh control reads. Shared execution
keeps identity before fixed profile cases, IAM provenance/expiry/private child
environments and independent final cleanup. No automatic migration or retry is
introduced. Independent review accepted the integration. All189 offline tests
pass, including both007 and008 composition, stale bindings, source mutation
during controls, entry isolation, identity failure and cleanup failure before
exchange. No canonical initialization or paid qualification occurred in this
batch. Full required CI and root's separate fresh admission still precede007.


## IAM window007 credential-format failure

Continuation CI37071688856/5e2eaae passed all required gates. Root explicitly
initialized v6 under the canonical lock, preserving exact v5 bytes, then admitted
one fresh bounded IAM007 request. Its reservation and preactivation cleanup
passed, but temporary-credential validation rejected the expiration format with
`iam_expiration_invalid`. No driver cases ran. Independent final cleanup
verified absence;007 remains consumed with retained liability and phase
cleaned_pending_billing. Conservative bounds USD20.17 compute/USD70.17 total
remain reservations, not bills or balance. Do not replay007 or automatically
admit008. IAM SQL remains unqualified.

The timestamp parser required a zero UTC offset, while the protected historical
AWS CLI GetCredentials proof contains a valid aware ISO timestamp with +02:00.
The offline repair normalizes any valid aware ISO timestamp to the same UTC
instant. Naive and malformed timestamps remain rejected; the execution-plus60s
cleanup margin and960s upper bound remain unchanged. Regression tests cover
positive/negative/fractional-hour offsets for role and database expirations and
expired, too-short and overlong values. All190 offline tests pass. A new required
full green CI and separately reviewed finite008 request precede further paid
qualification. The AWS field is a timestamp, not a timezone-free local time:
[GetCredentials response](https://docs.aws.amazon.com/redshift-serverless/latest/APIReference/API_GetCredentials.html).


## Local runner is a temporary stage; full CI coverage is planned

The user explicitly confirmed medium-term migration to full Redshift live CI
coverage across supported Linux, Windows and macOS profiles. Track this as RS9
in [REDSHIFT_FEATURE_PLAN.md](REDSHIFT_FEATURE_PLAN.md), including the complete
supported feature/behavior matrix and additional fixtures as features are added.
The present local GoogleTest runner remains the temporary approach; current CI
build/offline results and local live results stay distinct until live CI admission
and execution are qualified. Migration must retain shared cumulative cost controls,
coordination across machines, scoped credential/network access, hard execution
bounds, independently verified cleanup and sanitized per-platform test evidence.
No immediate change to the active IAM/catalog path or AWS authorization is made.


## IAM window008 opaque-token validation failure

Expiry repair CI37074374065/a0d090f passed all required gates. Root separately
reviewed/admitted008 with fresh controls, exact state/source/binary hashes and
durable retained headroom. Credential processing then failed
`iam_credential_invalid` before driver results. Independent final cleanup
verified absence;008 is consumed and cleaned_pending_billing. All eight original
windows are now consumed. Retained bounds USD21.37 compute/USD71.37 total are
conservative reservations, not billing or remaining balance. Do not create009,
replay008 or remove the finite limit to continue paid testing.

The historical protected AWS GetCredentials proof contains a printable opaque
DB password of1761 characters; only length/character-class metadata was inspected.
Our external-credential runner capped it at256 in both its factory and connection
encoder. The offline repair gives temporary database tokens a finite16KiB bound
at both paths, below the driver's64KiB connection-field limit. It does not alter
ordinary configuration passwords or role secret-key limits. Full-length tokens
are preserved and brace escaped, never truncated or exposed. Tests cover1761
and maximum-size values, escaping, redacted representations, and oversized/control
character rejection at both factory and dispatch. Independent review accepted the
repair; all191 offline tests passed. Live IAM SQL remains unqualified.

Before further paid qualification, independently review cumulative billing/usage
and a new finite accounting admission that preserves every original window,
recovery, period, resource and retained liability. Unknown actual spend cannot
justify a refund or a fresh balance. The original24h horizon must not be silently
extended. Continue the M2 inventory/profile assessment and offline repairs while
that separate accounting package is prepared; no further warehouse, capacity
increase or unrelated IAM expansion is authorized by this checkpoint.

## Read-only accounting assessment checkpoint

Opaque-token repair CI37076687940/980199d is green across required gates.
The next offline helper, `tools/redshift/pilot_accounting_review.py`, assesses
the exact locked, exhausted v6 state without writing, migrating, reserving or
launching anything. It validates nested history, both consumed continuation
windows' cleanup, the source-byte digest and the unchanged resource, horizon,
capacity and cumulative-bound rules. Another1200s metering allowance exceeds
the existing12000s ceiling: this is a blocked assessment, not authorization to
widen the ceiling or create009. All prior liabilities and unknown actual/remaining
values remain intact. A later versioned amendment requires separate review.

Read-only Cost Explorer returned `DataUnavailableException`; no permission
denial was observed. A CloudWatch `ChargedSeconds` observation for the fixed
workgroup returned639 datapoints with sum3600 and unit `Count`. Private raw
evidence is retained. These values are not reconciled costs: metric aggregation,
coverage/completeness, reporting delay and separate charges still need review.
The helper checks normalized observation syntax and resource/time binding only;
it cannot authenticate provenance or promote a metric sum, including zero, into
actual spend, remaining allowance or admission. AWS recommends the distinct
[`SYS_SERVERLESS_USAGE.charged_seconds`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_SERVERLESS_USAGE.html)
for compute-cost calculation; obtaining that SQL evidence would itself require
an explicitly reserved, bounded scope. No such query is authorized or run here.
