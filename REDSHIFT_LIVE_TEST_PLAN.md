# Initial Redshift live-test handoff

Recorded 2026-10-02 from the user's account-preparation chat. This is planning
and local coordination only. No AWS resource, live test, charge, pricing or
account verification was performed by this chat. The provisioning chat owns
infrastructure setup; this chat owns driver tests and integration evidence.

## Shared allowance and source of truth

The approved allowance is **USD 15 total for one initial testing period across
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

**Not implemented or qualified yet.** This handoff does not enable paid test
execution, scheduler retries or a cloud integration job.

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

Account access is reported restored; **Redshift M2 remains blocked on verified
infrastructure and runner readiness**. The bounded MySQL SDK proof continues
until that gate is ready. Then run the small Redshift baseline and its M2 scope
review before expanding parity, IAM or fetch-mode implementation. S2C, PostgreSQL
application acceptance and existing protected gates are unchanged.

References checked for planning:
[Stockholm 4-RPU support and capacity controls](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-capacity.html),
[usage-limit actions](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-workgroup-max-rpu.html),
[Serverless billing and metering](https://docs.aws.amazon.com/redshift/latest/mgmt/serverless-billing.html).
