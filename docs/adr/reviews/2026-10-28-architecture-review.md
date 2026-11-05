<!-- Checked in for #216 from the coordinator's local copy of the 2026-10-28 review (00-CONSOLIDATED.md), unchanged below this note. The six area reports it cites (01-06) are not in the repository. AGENTS.md treats this file as binding until ARCHITECTURE.md exists. -->

# ODBCPP architecture review — consolidated (2026-10-28)

Scope: main `6a630a7` plus uncommitted IAM slice 1/2a. Six parallel read-only
reviews (01–06 in this folder); claims marked ✔ were re-verified by the
coordinator in code. Nothing was built or run for the review itself.

## Executive summary

The core is better than its surroundings. The PostgreSQL/Redshift wire parser,
TLS client, SCRAM, deadlines and resource budgets are defensively written and
should be kept. The problems are structural:

1. **The data path cannot serve an analytics driver.** Every result is fully
   buffered as one heap string per cell; default caps (100k messages / 1M rows /
   64 MiB) fail and retire the connection on large SELECTs ✔.
2. **Redshift authentication is not in the product.** ~6k lines of AWS auth code
   run only in macOS test binaries (main-thread only, mutates the process
   environment, per-owner SDK init) ✔. Much of the auth core (receipts,
   provenance, trusted-clock inputs, raw body capture) is used only by tests ✔
   and now *blocks* features (provisioned IAM "gated by #100").
3. **Correctness/security bugs with small fixes:** catalog SQL injection on
   Redshift ✔, manual-commit mode can silently autocommit ✔ (code-level; needs a
   proof test), connection string controls the log-file path ✔.
4. **Structure fights change.** 9,155-line `odbc_handles.cpp` ✔ (921-line
   function), a nominal "SDK" that is compiled into core and carries Redshift
   fields, a configure-time include/partition machinery that blocks legitimate
   composition (hit during IAM slice 2a) ✔, ~14k lines of dead code (MySQL,
   prototype pool, `odbc/redshift`).
5. **Verification is thinner than the evidence volume suggests.** PR CI on
   Linux/Windows runs only driver-manager offline tests ✔; Redshift product tests
   are not gated per PR; no fuzzing, no TSan, no hardening flags.
6. **Process outweighs product.** Last 200 commits: 36 touched product source
   vs 79 tooling/CI-only and 32 docs-only; `tools/` ≈ 35k lines and root docs
   ≈ 17k lines vs ≈ 52k lines of product code. Docs understate some features and
   overstate others (auth "implemented").

## Verified critical / high findings

| # | Finding | Evidence | Fix size |
|---|---|---|---|
| S1 | Catalog literal quoting doubles `'` but not `\`; Redshift SQLTables/legacy PK-FK use it; `standard_conforming_strings`/`client_encoding` never checked | `pg_catalog_query.cpp:7-16,162-179,211-295`; correct helper exists `redshift_catalog_query.cpp:33-41` | days (then parameterize) |
| S2 | Manual-commit: `transaction_active_` not refreshed from server state after `execute_direct`/`dispatch_owning_parameters`; SQL `COMMIT` → next statement autocommits, rollback cannot undo | `odbc_handles.cpp:3834-3966` (only `begin_transaction_if_needed`), `7612-7704`; other paths do refresh (`6744`…) | days + proof test |
| S3 | `LOGFILE`/`LOGQUERIES` honoured from the application connection string; spdlog rotating sink, no confinement/0600/O_NOFOLLOW | `driver_logging.cpp:121-129,268-272` | days |
| D1 | Full materialization; caps retire the session; `SQL_ATTR_MAX_ROWS` truncates after download | `generic_database_connection.cpp:648-858`, `i_database_connection.h:29-37`, `odbc_handles.cpp:7857` | 5–7 pw |
| A1 | AWS owners unusable in a driver (main thread, setenv, InitAPI per owner, /tmp staging, env-activated fake HTTP and fixtures in production units) | `serverless_credentials_owner.cpp:23,105,150-162`; owner header calls itself "observational prototype … standalone macOS main entry" | rebuild |
| A2 | Provenance machinery unused by product, blocks provisioned IAM | Receipt/TrustedIssuer/Authority: no product references | delete/freeze |
| A3 | Auth is not one pipeline step: session owner and pool call `connect` directly | `session_owner.cpp:1078`, `connection_pool.cpp:13` | part of auth rebuild |
| Q1 | 32 `std::terminate` sites in driver-linked auth core (not reached today) | `sdk/src/auth/checked_*`, `response_operation.cpp` | with auth rebuild |
| C1 | PR CI: Linux/Windows run only `dm-offline`; Redshift product untested per PR; no sanitizers per PR | `.github/workflows/ci.yml:164,237` | 1–2 pw |

Other notable (unverified by coordinator, see area reports): process-wide
registry lock taken ~8–12× per fetch (03 C3); query timeout disconnects instead
of cancelling (03 M1); unmapped server errors fall back to 42000 (03 M5);
per-row futures in the epoll/IOCP transport (04 M1); no statement reuse or
pipelining (04 M2); interval/hllsketch unmapped, timezone offsets dropped (04
M5); MD5/trust accepted on plaintext, no `require_auth` (05 M1); secrets copied
unwiped (05 M2); no compiler hardening (05 M3); raw-address handles (ABA, 05
M5); `SQL_DRIVER_PROMPT` unsupported (03 M4); Redshift defaults port 5432/db
`postgres` (`product/compiled_backend.cpp:24`) ✔.

## Recommended program

Effort in person-weeks (pw), ranges overlap where proposals share work.

**Phase 0 — Fix now (1–2 pw).** S1 (dialect-aware quoter + session check on the
two settings, red test first), S2 (proof test, then refresh from ReadyForQuery
state on every path), S3 (logging keys admin/driver-section only, 0600,
O_NOFOLLOW), Redshift defaults 5439/`dev`, connection-string allow-list for IdP
keys, PR CI runs unit tests for both products on all three OSes plus ASan/UBSan.

**Phase 1 — Foundation (5–7 pw).** Replace partition/allow-list machinery with
conventional per-layer libraries (sdk-util/security/transport/contracts/session,
auth, backend, odbc, product) and keep a small architecture check; delete dead
code (MySQL, pool, `odbc/redshift`, stray files); clang-format + clang-tidy +
`-Wconversion` + hardening flags; fuzz harnesses for wire, connection-string and
AWS response parsers; vcpkg manifest pinning OpenSSL/AWS SDK/spdlog/gtest.
Rationale: every later rewrite needs fast, trustworthy gates.

**Phase 2a — Authentication rebuild (9–11 pw + 2 packaging).** One
`SessionEstablishment::open` used by ODBC, session owner and pool →
`CredentialResolver` (provider registry, keyed cache, single-flight, deadlines)
→ backend-owned opaque `AuthPlan` (removes AWS fields from public SDK structs) →
one process-wide `AwsRuntime` (init once, any thread, no env mutation) and one
`HttpsPolicy` (proxy, CA, timeouts, endpoint resolution incl. FIPS/VPC/ISO) →
`IPgAuthMechanism` registry (SCRAM, SCRAM-PLUS, MD5, GSS/SSPI, IdP token, RDS
IAM). Providers: Static, Profile (+role chains), explicit default chain /
InstanceProfile, JWT IAM, IdpToken, BrowserIdc, then SAML/OAuth plugins.
Serverless **and** provisioned from day one. Delete the standalone owners and
the unused provenance/clock/body-capture machinery after replacement.

**Phase 2b — Data path (9–13 pw).** One synchronous buffered PG wire engine on
all OSes (drop epoll/IOCP/async-TLS/thread-pool transports); streaming
`ResultStream` over portals/DECLARE-FETCH with batch budgets; statement cache and
pipelined array execution; set-based parameterized Redshift catalogs.

**Phase 3 — ODBC adapter (9–12 pw).** Split `odbc_handles.cpp` into units with a
data-driven state table and typed diagnostics; lock-light generation-tagged
handles; thin exclusive Session with post-operation state; cancel instead of
disconnect on timeout; DSN/registry via driver manager APIs; prompting.

**Process (1–1.5 pw, in parallel).** ≤5 root docs; status in GitHub issues and
milestones; archive journals; prune `tools/` to the load-bearing set (version,
packaging, TLS fixtures, fixture factory, export/SQLWCHAR checks); target a
majority of commits on product.

Total ≈ 35–50 pw. Suggested order: 0 → 1 → 2a and 2b in parallel → 3.

## Decisions recorded (2026-10-28)

- Plan and phase order approved. Auth uses the AWS SDK through one shared,
  process-wide runtime. Provenance/trusted-clock deletion: remind before 2a.
- AWS auth is a shared component linked into all drivers by default (Redshift,
  RDS/Aurora PostgreSQL and MySQL IAM tokens, Secrets Manager); RDS IAM for
  PostgreSQL joins Phase 2a.
- Products: one build, separately named drivers (PostgreSQL, Redshift, MySQL)
  from shared code. MySQL is planned: archive current stubs, design the backend
  interface for it now, implement after PostgreSQL/Redshift reach production.
- Reusable libraries (goal: ADBC drivers for AWS products). Phase 1 layout:
  `base` (std only: Result, Deadline/cancellation, SecretBytes/wipe, logging
  sink, LoginMaterial and HTTPS/trust config value types) ← `auth` (providers,
  resolver, cache, shared config/keyword parser) ← `auth-aws` (AWS SDK static,
  hidden symbols, no env mutation, coexists with other SDK users); `base` ←
  `db` SDK ← backends. `auth` and `db` never depend on each other; drivers
  compose them. Each is its own CMake package and namespace; an out-of-tree
  sample consumer built against installed packages proves the boundary in CI.
  C++ API, static linking; C ABI deferred. Phase 1 becomes 6–9 pw.
- IAM slice 1 option resolution feeds the redesign; slice 2a is not merged.
- Process: replace with GitHub issues/milestones, ADRs, ≤5 root docs, an agent
  reviewer per PR with human-approval labels, CI artifacts as evidence, and an
  OIDC live-test workflow with enforced pause and budget. Draft ADR + new
  AGENTS.md for approval before switching; no safety removed before its
  replacement works.

## Decisions requested (original list, now resolved above)

1. Approve the overall direction and phase order (or reorder, e.g. 2b first if
   large result sets matter more than IAM right now).
2. Auth: AWS SDK via a pinned process-wide runtime (recommended) vs our own
   SigV4 signer without the SDK (+3 pw, smaller binary, fewer dependencies).
3. Delete the provenance/trusted-clock machinery. This makes #100 and the
   pending PR #130 moot; consider asking the coordinator to hold that merge.
4. Ship PostgreSQL and Redshift from one binary with a runtime profile (halves
   the build/CI/package matrix) vs keep separate builds.
5. Process reduction — changes the coordinator workflow (AGENTS.md /
   DEVELOPMENT_WORKFLOW.md), so it needs your explicit approval.
6. In-flight IAM work: keep slice 1's strict option resolution as input to the
   new design; do not merge slice 2a.
