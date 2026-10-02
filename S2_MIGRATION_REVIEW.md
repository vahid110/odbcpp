# S2 noncrypto migration stopping review

Review date: 2026-10-02. Prior green baseline: `62cb065`,
[CI 36965441496](https://github.com/vahid110/odbcpp/actions/runs/36965441496).
Disposition: the bounded noncrypto S2 migration is implementation-complete;
acceptance requires all protected gates to pass on the commit containing this
review: complete PostgreSQL unit/live integration, iODBC UTF-16/UCS-4,
Windows live/package, sanitizers, Redshift build/absent-endpoint, and all existing
security/crypto regression jobs. Passing these does not qualify S2C/T13/T14.
A failure keeps S2 open and must be repaired. This is not G12 closure.

## Evidence and decision

The integration-owner assessment and an independent read-only audit found the
following migration evidence in the current source. The remaining identified PostgreSQL-default
leak, PostgreSQL's implicit port in SDK settings, is removed by this batch.

| S2 requirement | Evidence |
|---|---|
| Security prerequisites | Verified TLS/cleartext-auth policy, trusted DSN discovery and protected security tests |
| Provider/composition | Provider-owned identity, options/defaults, static capabilities and product registration; SDK settings use unresolved port 0, with PostgreSQL-family rejection before I/O |
| Errors and lifecycle | Owning backend errors, truthful state/disposition, safe summaries and conservative retirement on ambiguous failure |
| Resource boundaries | Startup/query wire, messages, metadata, result and request limits; bounded ODBC SQL, connection and parameter capture |
| Normalized results | Owning normalized schema/cells, flat ordered additional results, deferred errors and structural validation; native IDs/format tags remain private |
| Sessions and facets | Optional live transaction/description/catalog/reset/health facets; immutable provider dialect/type/capability policy |
| Dependency direction | Isolated SDK headers, partitioned runtime/backend/ODBC/composition targets, forbidden-dependency and export checks |
| Ownership/reuse primitive | Exclusive move-only leases, cleanup/health, credential binding/expiry, bounded opt-in lifetime/idle, cache-generation affinity, terminal failures and ODBC one-shot ownership |
| Pool quarantine | Unsafe prototype excluded from production/installed surfaces; no production pool claim |
| Reusable harness | Same isolated session baseline against independent synthetic and mandatory live PostgreSQL; fault rejection, recovery and retained ownership |

The private PostgreSQL-family session/parser machinery may remain PostgreSQL-
shaped. It is excluded from the staged SDK contracts and is not a base class for
MySQL. Existing historical progress notes saying “S2 remains open” describe their
individual checkpoints; this review supersedes that status after its gates pass.

## Fixed handoffs and limits

- **S3, integration owner:** implement the pinned MySQL 8 TLS/auth vertical slice
  against these contracts. Revisit catalog-query composition, binary-input hints,
  scalar representation and richer execution/parameter metadata only when real
  MySQL evidence demonstrates a necessary change. No speculative abstraction.
- **S4, integration owner:** staged author surface, full conformance kit, extension
  guide, independent clean-room sample and friction review. The internal session
  baseline is not that kit and no stable public SDK is claimed.
- **S2C/G12, integration owner:** RELEASE_PLAN T13/T14 and main-build AWS-LC
  integration retain their existing triggers and blockers. This review does not
  waive profile qualification or enable AWS-LC selection in the main product.
- **G12/security, integration owner:** cross-backend adversarial and reset evidence,
  supply-chain/static-analysis/fuzzing obligations remain required by
  SECURITY_MODEL.md. Existing bounded inputs are not an exact heap quota or proof
  of all translator/configuration/intermediate allocations. Address demonstrated
  defects immediately; do not reopen unlimited S2 work to claim universal bounds.
- **Optional/G10:** production pooling/capacity defaults, cross-session or prepared
  payload caches, external-schema freshness and performance tuning remain
  unclaimed. Add them only through their named product/measurement checkpoints.
- **Existing product gates:** PG-BETA/G8 application evidence and Redshift live
  access are unchanged. G12 needs S3/S4 and required security/crypto evidence;
  ADBC implementation, FIPS and public SDK distribution remain deferred.

Next planned package after acceptance is S3 preparation and the smallest live
MySQL handshake batch, preserving every PostgreSQL/platform regression gate.
S2's original 4–6 engineering-day estimate is not a measured elapsed-time report
or a new remaining-work estimate.

## Batch validation

Focused isolated/provider/liveness tests and complete local PostgreSQL,
iODBC UTF-16/UCS-4, ASan/UBSan, Redshift build and absent-endpoint checks passed
on the final source. Windows live/package and existing hosted security/crypto
jobs must still pass for this review's exact commit. The accepted record is the
successful run whose `headSha` equals that commit; an earlier green run is not a
substitute. No linked-server/application result is claimed.
