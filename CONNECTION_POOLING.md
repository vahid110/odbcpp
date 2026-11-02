# Connection pooling and safe reuse

This is the pooling ownership and qualification contract, clarified on 2026-10-24.
It refines the existing [SDK plan](SDK_PRODUCT_PLAN.md#pooling-and-caching-boundary)
and [architecture](SDK_ARCHITECTURE.md#reuse-pooling-and-caches). It records future
work without starting implementation or interrupting the current Windows/WSL live
checkpoint. Existing full-suite results do not establish pooling qualification
unless their exact Driver Manager configuration and reuse evidence show it.

## Ownership

Standard ODBC pooling is normally provided by the Driver Manager (DM), or by an
application/framework. An application can also retain connections itself. Standard
DM pools operate within a process; they are not a cross-process connection broker.

| Component | Responsibility and physical owner | Boundary |
| --- | --- | --- |
| Driver Manager/application | Retain idle connections, select matches, manage pool lifetime and any application waiting/capacity policy | External to ODBCPP; no duplicate pool is required inside the driver |
| ODBC adapter | Handle attributes, diagnostics, passive connection-dead reporting and supported DM reset interfaces in `odbc/odbc_handles.cpp` | Translate actual ODBC calls into SDK operations; DM-specific APIs stay out of the SDK |
| Shared SDK | Exclusive session ownership, serialized operations, typed health/reset/retirement outcomes, credential/cache isolation; existing `sdk/internal/odbcpp/session/session_owner.h` and `sdk/include/odbcpp/database/session_health.h` / `sdk/include/odbcpp/database/session_reset.h` | Reuse these primitives across clients/backends; they do not by themselves maintain a multi-session pool or prove DM compatibility |
| Backend | Execute protocol cleanup, rollback and the declared reset/health operations; PostgreSQL examples are `core/database/postgres/pg_session_reset.cpp` and `core/database/postgres/pg_database_connection.cpp` | Native protocol and SQL remain backend-owned; unsupported required cleanup prevents reuse |

An SDK-managed pool remains optional and initially internal. Its absence does not
block standard DM pooling qualification. Any future pool must use the shared safe
reuse contract; independent backend pools, public pool APIs and unmeasured sizing
or eviction policies are outside the current commitment. The legacy prototype
pool remains quarantined in its private test/example target.

## Observable reuse boundaries

Application-level connect/disconnect calls may be satisfied inside the DM without
a corresponding physical driver connect/disconnect. Do not assume every logical
checkout or return notifies the driver. For each advertised DM mode, identify the
actual driver calls, attribute changes and reset callbacks available to ODBCPP.

The adapter must implement those interfaces correctly. A reset can only be
promised at an explicit, observable lifecycle boundary. In ordinary pooling, SQL
such as SET, role changes and temporary-object creation may leave server state
that the DM does not track. Document application cleanup obligations and the
supported session-state profile. If that profile requires cleanup which cannot
be guaranteed, decline the pooling claim or use a narrower documented profile;
where a reset/reuse decision is exposed, retire instead of returning unsafe state.

Within a supported reset profile, cleanup covers applicable transaction state,
autocommit/isolation/catalog/role/settings, pending results, temporary/prepared
resources, diagnostics and cache invalidation. Reset failure, timeout or uncertain
protocol synchronization prevents reuse. A successful backend reset alone does
not authorize a new borrower or prove credential/security-context isolation.

`SQL_ATTR_CONNECTION_DEAD` reports the last known connection state efficiently;
its getter must not perform a hidden network round trip. It is not a guarantee
that an idle network connection is currently reachable. Any separately supported
active health probe has its own bounded contract. Qualify sequential connection
handoff between threads, including connect/use/disconnect on different threads;
this does not promise unrestricted concurrent operations on the same handle.

Windows driver-aware pooling is a separate optional capability. Its pool-ID,
rating, reset interfaces and information-token handles require explicit adapter
implementation and evidence. They are not prerequisites for ordinary DM pooling
and must not be advertised from generic pooling tests. Do not assume Windows SPI
or configuration semantics exist in unixODBC or iODBC.

## Authentication and session lifetime

Pool matching/reuse must respect principal, authentication/security context,
credential generation and TLS/trust policy. Incompatible contexts must not share
a physical session. Apply the configured reuse-admission policy and invalidate
related cache state at the corresponding security/session boundary.

Distinguish expiry of a credential used to log in from expiry or invalidation of
an established server session. Credential expiry alone does not prove the server
session ended. Fresh authentication/reconnect must use admissible credentials;
reuse of an established session follows backend semantics and the declared policy.
The existing SDK may deliberately impose stricter admission, including refusing
new borrows after credential expiry/revocation without aborting an active borrower.
Document and test that policy separately; do not silently relax it or infer it
from a generic credential timestamp. No reconnect, reauthentication or replay is
implicit in a pooling operation.

## Qualification and remaining gaps

The following matrix is required evidence to collect, not a declaration of
current support. Pin the DM/OS/backend versions, architecture, SQLWCHAR width,
crypto profile, pooling/matching configuration and actual adapter call trace.

| Profile | Required evidence | Current acceptance boundary |
| --- | --- | --- |
| Native Windows DM | Ordinary pooling off/on, supported connect A/W APIs, matching, thread handoff, actual physical reuse and cleanup/isolation | Production pooling qualification remains open; optional driver-aware SPI is separate |
| WSL/Linux unixODBC | Same behavior under its own pooling settings and version; preserve WCHAR2/native ABI identity | Neither successful reconnects nor existing generic live passes prove a pool was used |
| WSL/Linux or macOS iODBC | Same behavior under its own supported settings/version and selected WCHAR width | Record genuinely unsupported modes; do not substitute unixODBC or Windows evidence |

Checked-in GoogleTests must prove physical-session reuse with an observable server
session identity and a pooling-off control, not just successful SQLConnect calls.
They must cover failure/retirement and state isolation as well as the happy path:
transactions/settings, pending/cancelled operations, dead/idle-disconnected
connections, failed reset, principal/trust mismatch and credential/session lifetime.
Capture actual DM callbacks to establish which guarantees belong to the driver
and which cleanup belongs to the application. CTest schedules the tests and
retains process outcomes; native XML and driver/DM traces establish assertion and
reuse results. Preserve PASS/FAIL/SKIP, original failures and source/configuration/
binary/inventory binding. A focused pooling run does not replace the complete
applicable checkpoint when it is initiated.

Prototype `test_connection_pool`/`it_connection_pool`, SDK lease tests and backend
reset tests retain their own regression value. They cannot close production DM
pooling acceptance. Existing SDK primitives must be audited and reused before
implementing any missing adapter integration; this document does not claim they
are absent or already sufficient.

- [Implementation/integration follow-up #101](https://github.com/vahid110/odbcpp/issues/101): audit and complete the required SDK/backend/adapter reuse behavior, observable reset boundaries and supported-mode declarations.
- [Platform qualification follow-up #102](https://github.com/vahid110/odbcpp/issues/102): collect real pooling/reuse/isolation evidence on Windows DM, unixODBC and iODBC after the current live-test work.

These are deferred follow-ups. They do not request a candidate switch, new build,
new live partition, resource admission or interruption of an executing suite.

## Reset and reuse operations audit (#101 part 1)

This audit documents which reset and reuse operations currently exist across the
ODBCPP driver stack and which remain missing, satisfying the initial bounded
scope of #101:

| Operation / Capability | Layer | Current status | Implemented behavior and remaining gaps |
| --- | --- | --- | --- |
| `SQL_ATTR_CONNECTION_DEAD` | ODBC adapter / SDK session | **Implemented** | Passive inspection (`backend_lease_->inspect()`) without network I/O. Reports `SQL_CD_FALSE` when connected and healthy; reports `SQL_CD_TRUE` when connected but broken/dead (`SessionState::Broken`); returns `SQL_ERROR` with `08003` when disconnected (unconnected, post-`SQLDisconnect`, or post-timeout retirement) leaving output buffer untouched. Zero network reads/sends guaranteed across ANSI (`SQLGetConnectAttr`) and Wide (`SQLGetConnectAttrW`). Setting is rejected as read-only (`HY092`). Length-only queries report `sizeof(SQLUINTEGER)`. |
| `ISessionHealth` active probe | SDK / Backend | **Implemented** (SDK & PostgreSQL) | Serialized active probe under an absolute deadline. On network drop or timeout, retires session to `SessionState::Disconnected`. Internal SDK facet; standard DMs rely on passive `SQL_ATTR_CONNECTION_DEAD`. |
| `ISessionReset` (`SameAuthenticatedServerSession`) | SDK / Backend | **Partial** (PostgreSQL only) | PostgreSQL backend rolls back active/failed transactions and executes `DISCARD ALL` under an absolute deadline, retiring on any failure. SDK `SessionLease::reset_session` enforces idle outcome, invalidates metadata cache, and retires on failure. |
| Redshift session reset | Backend | **Missing** | Redshift does not support PostgreSQL `DISCARD ALL`. Lacks a dedicated dialect-appropriate reset sequence (explicit `ROLLBACK` + supported variable resets, or connection retirement). |
| MySQL / SQL Server session reset | Backend | **Missing** | Backend connections return `nullptr` for `session_reset()`. |
| Multi-tenant re-authentication | SDK | **Missing** | The reset profile only covers `SameAuthenticatedServerSession`. It does not support re-authenticating as a distinct database user or re-binding credentials over an existing transport. |
| `SQL_ATTR_RESET_CONNECTION` | ODBC adapter | **Missing** | Recognized as unsupported; returns `SQL_ERROR` (`HYC00`). Windows DM sets this attribute during connection recycling; adapter does not yet dispatch it to `ODBCConnection::backend_reset()`. |
| DM logical checkout/return visibility | ODBC adapter | **Missing** | unixODBC/iODBC do not notify the driver when a connection is logically checked out or returned to the pool. `SQLDisconnect` currently executes `close_connection()`, tearing down the physical transport. Application cleanup obligations remain required. |
| Driver-aware pooling SPI | ODBC adapter | **Declared unsupported** | Reports `SQL_DRIVER_AWARE_POOLING_NOT_CAPABLE` in `SQLGetInfo`. Separate optional Windows capability; not a prerequisite for standard DM pooling. |

## Primary references

- [Microsoft: Driver Manager connection pooling](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-app/driver-manager-connection-pooling?view=sql-server-ver17)
- [Microsoft: developing driver-aware pooling](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-driver/developing-connection-pool-awareness-in-an-odbc-driver?view=sql-server-ver17)
- [unixODBC: connection pooling](https://www.unixodbc.org/doc/conn_pool.html)
- [iODBC change notes: pooling implementation history](https://www.iodbc.org/wiki/iodbcWiki/ChangeNotes)

Upstream support for pooling is not qualification of ODBCPP under that manager.
