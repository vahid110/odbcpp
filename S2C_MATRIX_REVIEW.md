# S2C finite evidence review

Review date: 2026-10-01. Implementation baseline:
`69c7d3dc00764a4d60abc8c3a807b47df64927df`.
Status: finite evidence review complete; bounded architecture evidence accepted;
provider qualification and G12 remain open.

## Decision and evidence

Accept the provider-neutral private security boundary, strict build profiles,
artifact/export and runtime identity checks, mandatory TLS/authentication tests,
pinned non-FIPS AWS-LC Linux experiment, and the documented OpenSSL package and
cohabitation fixtures as bounded internal architecture evidence. An independent
read-only review reached the same conclusion. Implementation baseline CI
[36804370873](https://github.com/vahid110/odbcpp/actions/runs/36804370873)
passed all required jobs, including Windows fresh-runner packaging. Linked-server
application testing was skipped and is not acceptance evidence.

This decision permits S2 noncrypto migration to resume while qualification
blockers stay visible. It does not close S2C as a whole, waive any G12 criterion,
qualify a public provider matrix, enable main-build AWS-LC, or establish FIPS
compliance. The documentation revision must also pass the protected gates before
this review is used as the current plan baseline.

## Finite matrix

| Profile | Accepted internal evidence | Remaining acceptance limit |
|---|---|---|
| Linux OpenSSL `SYSTEM_SHARED` | Artifact form/origin, runtime identity, independent TLS/downgrade, actual-driver TLS/SCRAM and rejection cases, configured-provider host crypto/TLS operations across driver references | Host lifecycle proof uses the configured provider; it does not prove live queries in that same host or arbitrary incompatible-provider coexistence. Final supported-host assumptions and row signoff remain open |
| Linux OpenSSL `BUNDLED_STATIC` | Static artifact/export checks, archive-input/member link trace, runtime identity, independent TLS/downgrade, live driver under system preload, relocated driver-only archive and three dependency licenses | Cohabitation covers instances from one OpenSSL package. Cross-implementation coexistence and embedded-byte ancestry are not established; recorded archive inputs are not a broader source-provenance claim. Final row signoff remains open |
| macOS OpenSSL `SYSTEM_SHARED` | Both iODBC widths, artifact form/origin, runtime identity, independent TLS/downgrade, actual-driver TLS/SCRAM/rejections, configured-provider host lifecycle | Same configured-provider scope as Linux shared; no broader incompatible-host or public distribution qualification. Final row signoff remains open |
| Windows OpenSSL `BUNDLED_SHARED` | PE imports, runtime identity, independent TLS/downgrade, live Driver Manager TLS/SCRAM/rejections, MSI lifecycle, app-local origin/hash checks, same-installed-file preload | A foreign same-basename preload binds into the driver. The harness detects it after load; production does not prevent it. Windows isolation is an explicit qualification blocker |
| Linux AWS-LC `BUNDLED_SHARED` | Accepted pinned internal experiment: production private adapter composition, TLS/auth, async/session and ODBC proof, identity/artifact/export, OpenSSL coexistence and relocated package checks | Separate experiment only; main-build selection is disabled. Public product integration and distribution qualification remain separate |
| Linux AWS-LC `BUNDLED_STATIC` | Same bounded experiment, with static artifact/export and coexistence evidence | Same experiment-only limit; no main-build or FIPS claim |

The retained JSON records intentionally keep qualification flags false. Windows
records `foreignPreloadPreventedByDriver=false` and
`preloadedModuleCoexistenceVerified=false`; Linux static records its provenance
and cross-implementation limitations. A green test run cannot override these
limits. Detailed evidence scopes remain in [CRYPTO_PROVIDER_PLAN.md](CRYPTO_PROVIDER_PLAN.md).

## Fixed follow-up boundaries

1. Integration owner: resolve Windows same-basename provider isolation in a
   separately estimated batch before qualifying that Windows profile or closing
   a gate requiring it. Select one strategy and qualify its live/package behavior;
   do not scatter speculative loader-lock work across ODBC entry points.
2. Integration owner: complete explicit OpenSSL row signoff against the existing
   finite requirements, including the supported host compatibility model and
   provenance scope. Missing mandatory evidence stays a blocker. Any proposed
   narrowing of a gate requires an explicit decision; this review grants none.
3. Main-build AWS-LC integration remains a separate reviewable implementation
   step before product use. The bounded Linux proof stays accepted as previously
   documented. Public SDK packaging, broad platform/provider variants and FIPS
   stay in their existing deferred milestones.

Do not reopen unlimited crypto exploration during each SDK batch. Revisit these
items when their named qualification gate is worked, a supported security defect
appears, or a shared change invalidates existing evidence. Existing crypto and
platform regression gates remain mandatory during S2.

## Next S2 batch and gate order

The next implementation batch is A5 structured error/session disposition:
introduce owning backend errors with safe messages and optional native detail,
explicit passive session state/disposition, and tests for server errors,
transport failure, timeout and ambiguous-state retirement. Migrate PostgreSQL
incrementally, preserving ODBC diagnostics and existing deadline behavior;
remove mutable error side channels only as their callers migrate. Do not combine
this with results, pool replacement or a MySQL protocol implementation.

Then continue normalized owning results, session/facet separation, resource
budgets and safe reuse contracts through the accepted S2 sequence. SEC-3 pool
ownership, SEC-4 aggregate limits and the remaining security evidence are open.
S3 MySQL, S4 conformance/clean-room usability and G12 follow; PostgreSQL G8 remains
host-dependent and Redshift live M2 waits for restored access. Original effort
ranges are estimates, not a fresh remaining-work completion date.


## Linux shared live-report binding — 2026-10-01

The Linux OpenSSL SYSTEM_SHARED job now records a separate bounded live summary
only after its mandatory TLS fixture tests pass. It binds the live XML hash and
exact successful verified TLS/SCRAM query, trust rejection and hostname rejection
case inventory to the existing driver/probe/manifest/unit evidence. Missing,
duplicate, unexpected, incomplete, failed/skipped cases and inconsistent totals
are rejected; failed CLI validation removes stale output. Unit-only recording for
other profiles is preserved. Both live XML and summary are retained in CI.

This does not evaluate live queries within the cohabitation host, packaged-driver
acceptance, different-provider coexistence, Windows loader isolation or row signoff.
Qualification and combined package/coexistence acceptance remain false; T13/T14
and the S2C gate stay open. The new live-summary option is intentionally limited
to this Linux shared row until other row-specific live formats are reviewed.
