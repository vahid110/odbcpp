# Shared ODBC and database backend boundary

Planning baseline: 2026-09-23, implementation inspected at `4f6de2a`.
Status: A1–A4 and G9a are complete. The 2026-09-30 S1 review accepts the
[SDK architecture](SDK_ARCHITECTURE.md) and [security model](SECURITY_MODEL.md)
for a bounded MySQL 8 proof across an unrelated protocol.
Scope: PostgreSQL production reference, MySQL SDK proof, Redshift family
specialization. See [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md).

## Requirement and ownership

The shared framework must become reusable during PostgreSQL consolidation.
MySQL must now prove that boundary against an unrelated protocol. Redshift must
exercise PostgreSQL-family specialization rather than trigger a wholesale
extraction later. Public SDK packaging and stability guarantees remain G13.

| Shared framework owns | Database backend owns |
|---|---|
| ODBC entry points, handles, lifetime and call serialization | Connection creation, protocol session, authentication exchange |
| Descriptor storage, binding buffers and alignment rules | Native type IDs, backend type metadata and value representations |
| ODBC return codes, diagnostic record lifecycle and validation | Server error interpretation and backend-specific capability values |
| Common conversion rules, NULL and truncation handling | SQL dialect, parameter-marker syntax and native parameter encoding |
| Common catalog argument validation and ODBC output shape | Catalog SQL, type discovery, visibility and backend-specific metadata |
| Deadline propagation, transport primitives and logging infrastructure | Backend session/deadline recovery decisions and connection retirement |
| Optional shared pool policy, cache policy/limits and logical-connection lifecycle | Physical-session health/reset outcome, credential expiry, native prepared handles and backend-specific invalidation signals |
| Contract-test harness and shared regression suites | Live backend fixtures and backend-specific expected results |

Common text conversions may be shared where their semantics agree. PostgreSQL
wire encodings (for example, bytea representations) are not universal ODBC rules.
Transport reuse does not require every future backend to use PostgreSQL framing.

## Observed coupling and bounded extraction work

| ID | Current evidence | Required PostgreSQL-stage outcome |
|---|---|---|
| A1 | `ODBCConnection::connect` in [odbc_handles.cpp](odbc/odbc_handles.cpp) directly constructs `PgProtocolParser`; [DatabaseFactory](core/database/database_factory.cpp) separately constructs connections | One selected backend creation route; preserve transport options, ownership, TLS settings and deadline behavior. No direct PostgreSQL parser construction in shared ODBC orchestration |
| A2 | Statement preparation calls `PgProtocolParser::parameter_marker_count` | Route SQL/marker processing through the backend boundary; preserve existing PostgreSQL quoting, escaping and marker tests |
| A3 | `postgres_type_info`, OID/domain lookups and catalog queries are embedded in `odbc_handles.cpp` | Backend owns native type interpretation and catalog construction. Shared ODBC code consumes the required type/capability/catalog contract without interpreting PostgreSQL OIDs or issuing PostgreSQL catalog SQL itself |
| A4 | [QueryResult](core/database/query_result.h) exposes native IDs/type modifiers; [IProtocolParser](core/database/i_protocol_parser.h) exposes PostgreSQL-shaped messages/authentication | Document native versus common fields; keep protocol details below the backend boundary. Reuse/evolve [IDatabaseConnection](core/database/i_database_connection.h) and [QueryParameter](core/database/query_parameter.h) where sufficient instead of adding parallel abstractions |

These are the initial four architecture work items, not four automatic large
commits. Inventory exact call sites and size them in M0. Split implementation
into reviewable changes. Moving files alone does not close a work item; callers
must use the boundary and tests must exercise it. A live Redshift endpoint is
not required to implement and verify the PostgreSQL boundary.

## G9a — PostgreSQL architecture acceptance (required before PG-BETA)

- A1–A4 are mapped to concrete code and their PostgreSQL-stage outcomes are met.
- The documented contract specifies ownership/lifetimes, native versus normalized
  metadata, NULL representation, error/SQLSTATE handling, capabilities, deadlines,
  and whether an error permits reuse or requires retiring the connection.
- Shared ODBC orchestration no longer selects a PostgreSQL parser directly,
  interprets PostgreSQL type IDs, or constructs PostgreSQL catalog queries.
  Those operations reside behind the selected backend. Protocol libraries may
  remain PostgreSQL-specific internally and be shared with Redshift later.
- A small fake backend exercises backend selection, successful result/NULL
  delivery, an error, and an unsupported capability through the shared layer.
  It must not duplicate a real protocol or become a new product backend.
- Existing PostgreSQL behavior is preserved by the full PostgreSQL, Driver
  Manager width and sanitizer gates; relevant Windows checks remain green.
- Review the remaining dependencies explicitly. No known leak of database
  semantics across the required boundary is silently deferred to SDK packaging.

## G9b — Redshift reuse acceptance (required before Redshift beta)

- Select PostgreSQL and Redshift backends through the same contract; keep shared
  ODBC wrappers, lifetime rules, descriptors and diagnostic machinery in one place.
- Add observed Redshift differences to its backend or to demonstrated shared
  PostgreSQL-family protocol components, rather than scattering backend branches
  through shared ODBC workflows.
- Run common contract tests against both backends plus real Redshift acceptance.
  Refine only contract gaps demonstrated by this work; preserve PostgreSQL gates.
- Record which components are shared and which differ. This proves reuse within
  the PostgreSQL family, not arbitrary-protocol portability.

## G12 — unrelated-protocol SDK proof

- PostgreSQL and MySQL are sibling implementations selected through the same
  backend contract; neither backend wraps or depends on the other.
- Shared client orchestration contains no PostgreSQL or MySQL packet formats,
  native type identifiers, authentication messages or catalog SQL.
- MySQL passes the bounded live and failure workflow in
  [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md), while all PostgreSQL regression,
  Driver Manager width, Windows and sanitizer gates remain green.
- Backend author contract tests and an out-of-tree sample prove extension
  mechanics without editing shared ODBC wrappers.
- The S1 architecture-quality review accepts cohesive responsibilities,
  dependency direction, ownership/concurrency/error semantics and the future
  columnar attachment point before interface extraction proceeds.
- The S4 clean-room exercise uses only staged SDK materials. It adds a minimal
  backend and passes contract tests with zero shared ODBC workflow edits and no
  undocumented knowledge; all friction and test-message gaps are recorded.
- The result contract supports the current row-oriented ODBC path without
  preventing a later Arrow columnar path. No Arrow or ADBC implementation is
  required for G12.
- Interfaces remain explicitly unstable. Public versioning, compatibility and
  distribution are G13.
- Automated forbidden-dependency checks reject backend-to-ODBC dependencies,
  protocol/native-type leakage into shared orchestration and dependency cycles.
  Functional tests cannot waive these architecture failures.
- The current generic connection/parser is kept private to the PostgreSQL
  family during migration. MySQL owns an independent session and protocol state
  machine; no universal parser or protocol conditionals are accepted.
- Verified channel/authentication, configuration provenance, secret lifecycle,
  resource budgets, logging classification and extension trust satisfy
  [SECURITY_MODEL.md](SECURITY_MODEL.md).
- Driver Manager reuse and any internal SDK pool use the same documented health,
  reset and retirement contract. PostgreSQL/MySQL tests cover transaction/session
  cleanup, failure and credential-expiry paths; backends do not own pool policy.
- Each metadata/type/prepared cache declares scope, bounds, concurrency,
  invalidation, sensitive-data handling and observability. G12 proves correctness;
  G10 measurements gate new performance behavior. Query-result caching is out of
  scope.

## What remains later

G13 covers public SDK packaging, external compatibility, complete reference
documentation, licensing/support lifecycle and an explicit API stability policy.
It does not own A1–A4 or the G12 unrelated-protocol proof.

No generic plugin registry, ABI guarantee or universal authentication framework
is required for G9a or G12. Apply the investigation
limits in [RELEASE_PLAN.md](RELEASE_PLAN.md). If a necessary extraction exceeds
its budget, report the impact and re-estimate the milestone; do not silently
waive architecture acceptance or hide the extra work in contingency.

## Implementation progress — 2026-09-25

A1's construction change routes `ODBCConnection::connect` through
`DatabaseFactory`, transferring its configured transport. Existing no-argument
and explicit-type factory entry points remain available. Tests cover selected
startup/query, authentication error/timeout, refused TLS and transport destruction
on unsupported selection. Transport options and deadline logic remain in their
existing paths and are covered by the full regression gates.

The marker-counting part of A2 now uses `IDatabaseConnection` and the selected
parser's lexical rules, removing the PostgreSQL parser include from shared ODBC
handles. A test parser with different quoting semantics proves dispatch is not
hardwired to PostgreSQL. This adds an internal virtual contract; no stable public
C++ ABI is promised, and downstream implementations must provide the new method.
The MySQL placeholder remains explicitly unimplemented.

A2 is not closed: shared SQL escape translation still needs backend ownership.
A3/A4 and fake-backend acceptance through the complete shared ODBC layer remain
open. Removing the parser include does not establish complete SDK reuse.

## Implementation progress — 2026-09-27

A2's remaining escape translator now lives in
[pg_sql_dialect.cpp](core/database/postgres/pg_sql_dialect.cpp). SQLNativeSql A/W,
direct execution and preparation obtain translation through the selected backend.
The backend owns syntax/lexical decisions; the ODBC layer keeps SQLSTATE mapping,
input/output validation, truncation and NOSCAN behavior. The result contract is
[sql_translation.h](core/database/sql_translation.h); backend/parser implementations
must implement pure `translate_sql`, in addition to marker counting. The MySQL
placeholder explicitly returns Unsupported and remains outside release scope.

UTF-8 scalar decoding/counting is shared in [utf8.h](core/util/utf8.h), so the
PostgreSQL dialect does not include ODBC headers. The ODBC Unicode adapter uses
the same validator, preserving UTF-16/UCS-4 and malformed-input behavior.

Tests exercise selected-backend translation, syntax/datetime/unsupported errors,
recovery and a fake parser with different translation rules. Real PostgreSQL
A/W direct/prepare errors preserve a previous prepared statement and permit
reuse. Existing native-SQL output/truncation, Unicode, quoting, nested comments,
NOSCAN and Driver Manager tests remain required. A3/A4 and complete fake-backend
ODBC acceptance remain open; A2 closure is recorded with the batch validation.

## A3 scalar-metadata batch — 2026-09-27

The selected backend now interprets native type IDs, widths and modifiers through
`describe_type`, returning the ODBC-independent families and fields in
[native_type_info.h](core/database/native_type_info.h). PostgreSQL interpretation
lives in [pg_type_info.cpp](core/database/postgres/pg_type_info.cpp). Shared ODBC
column and parameter metadata maps those normalized families to ODBC constants;
it no longer contains the `postgres_type_info` native-ID switch. The connection
contract delegates to its selected parser, and a differing mock mapping is tested.

Known/unknown classification for parameter discovery also comes from the backend.
Existing domain lookup, cache semantics, bound parameter precision overrides,
SQLGetTypeInfo definitions, descriptor type names and catalog SQL remain for
subsequent A3 work. Therefore A3 and overall G9a remain open. Native IDs in
QueryResult are still opaque backend metadata awaiting the remaining extraction.

Tests cover scalar mappings, numeric precision/negative scale, temporal widths,
character limits, unknown-type fallbacks, and metadata agreement before/after
execution including NULL output preservation. Temporal width calculation uses
unsigned wide arithmetic to avoid signed overflow on extreme metadata modifiers;
this does not promise support for out-of-range temporal precision.

## A3 domain-discovery batch — 2026-09-27

`IDatabaseConnection::resolve_types` returns normalized metadata for every
requested opaque native ID, with fallback metadata for unknown or missing types.
`PgDatabaseConnection` owns the recursive PostgreSQL domain query and its row
validation; the generic connection resolves parser metadata without catalog I/O.
The lookup uses the caller's existing deadline and returns no partial map on
failure. Shared ODBC statements cache normalized values and retain diagnostic,
timeout-disconnect and binding responsibilities. They no longer issue domain
lookup SQL or interpret domain catalog rows.

Tests cover deduplication, nested domain typmods, unknown/missing types,
malformed/duplicate/unrequested rows, overflow, lookup errors and recovery.
Reprepare coverage exposed and fixed inferred IPD metadata being reused as an
application binding: only explicit bound types and dimensions now override fresh
backend metadata. Character widths, numeric precision/scale, error output
preservation and explicit binding reuse are checked against PostgreSQL.

Catalog SQL, SQLGetTypeInfo construction, descriptor type names and remaining
capability/contract acceptance are still open. A3 and G9a are not complete; this
batch does not establish Redshift compatibility.

## A3 table/key catalog batch — 2026-09-27

`CatalogRequest` defines table discovery and primary/foreign-key filters without
ODBC headers or PostgreSQL identifiers. `IDatabaseConnection::catalog_query`
constructs SQL without I/O; unsupported backends return `UnsupportedFeature`.
`PgDatabaseConnection` owns these three queries, including native catalog joins,
name quoting, rule mappings, result shapes and ordering. Shared ODBC code keeps
A/W argument validation, table-type list parsing, enumeration-mode selection,
query execution, deadlines, diagnostics and cursor state. Null versus empty names
and pattern versus literal matching remain explicit in the request contract.

Six backend tests cover unsupported discovery, omitted/empty/quoted filters,
all/none/explicit table types, enumeration modes and foreign-key ordering. Live
PostgreSQL coverage adds quoted Unicode table/key names and empty-result reuse;
existing metadata tests retain enumeration, key rules, composite-key order,
ANSI/wide behavior and open-cursor rejection coverage.

Five catalog operations remain in shared code: columns, statistics, procedures,
procedure columns and special columns. SQLGetTypeInfo construction and descriptor
type names also remain. A3/G9a are still open; no new Redshift support is claimed.

## A3 remaining-catalog batch — 2026-09-27

All eight catalog operations now dispatch through `CatalogRequest` and
`IDatabaseConnection::catalog_query`. The PostgreSQL backend owns columns,
statistics, procedures, procedure columns and special columns as well as the
three previously extracted operations. Shared ODBC code contains no PostgreSQL
catalog query text or catalog type/domain SQL helpers. It converts validated
ODBC special-column options into backend-neutral identifier/scope enums and
retains argument validation, A/W conversion, execution and cursor diagnostics.

The move preserves the existing SQL expressions, filters, domain typmods,
nullability rules, ordering and conservative special-column scope behavior.
Eleven backend tests cover all eight request alternatives, including unsupported
backends, literal versus pattern filters, omitted/empty names, quoting, unique
indexes, nullable candidates and typed empty results. Added PostgreSQL tests
exercise quoted Unicode identifiers and domain dimensions across the five moved
operations; all existing catalog regression cases remain required.

Catalog construction extraction is complete. SQLGetTypeInfo construction and
descriptor type names remain in A3, followed by A4 and shared-layer contract
acceptance. This is an ownership change, not a claim that all catalog semantics
or PostgreSQL beta acceptance are complete; no new Redshift support is claimed.

## A3 type-catalog completion — batch 7

Backend `TypeDefinition` catalogs now supply advertised names, sizes, literals,
radix and scale limits. PostgreSQL owns its server-version scale rule. Shared
ODBC code maps normalized families, adds legacy temporal aliases and formats the
19-column SQLGetTypeInfo result. Descriptors use the same catalog, including the
configured unconnected backend before login. Synthetic result columns can carry
normalized metadata without native IDs. A3 extraction is implemented; A4 and
full shared-layer contract acceptance still block G9a. See PG_BETA_CHECKLIST.md
for batch evidence and DEVELOPMENT_HANDOFF.md for the next entry points.

## A4 transaction boundary — batch 8

The transaction contract uses backend-neutral action/isolation enums and a
capability record. Backend commands preserve the caller's deadline and return
errors unchanged; ODBC state changes occur only after success. PostgreSQL owns
its transaction SQL, while generic unsupported backends return UnsupportedFeature.
Transaction-related SQLGetInfo values use the same capability record. Cursor
preservation remains shared-driver behavior for buffered results. This implements
only the transaction portion of A4; broader capabilities, normalized/native
contracts and fake-backend orchestration acceptance remain open. Batch 8's
full local validation evidence is recorded in PG_BETA_CHECKLIST.md.

## A4 advertised backend profile — batch 9

`BackendCapabilities` is a no-I/O snapshot with backend-neutral enums, feature
flags and names. Its string views remain valid for the connection lifetime.
PostgreSQL owns its database identity, identifier limits/casing/quoting, catalog
terms, NULL/concatenation rules, GROUP BY/correlation rules, SQL conformance,
index/insert/set-operation flags and schema usage. Shared SQLGetInfo maps that
snapshot to ODBC values and retains ANSI/wide output, truncation and diagnostics.
Generic backends return a conservative empty profile rather than PostgreSQL
claims. The legacy Redshift build name is preserved in the PostgreSQL-family
backend without adding any unverified Redshift capability differences.

The shared layer still owns Driver Manager/driver identity, interface conformance,
forward-only/read-only buffered cursors, multiple result delivery, scalar binding,
get-data order and lack of async/array/bookmark support. Zero SQL feature masks
remain conservative unadvertised features of the exposed driver profile; they do
not claim the server lacks those features. Empty keyword/collation strings and
zero unspecified limits remain unchanged. Accessible-object flags do not promise
all returned objects are authorized. New backend support must review this exposed
profile alongside its backend-specific values before advertising additional SQL.

This completes this batch's capability routing, not A4/G9a. Next: explicitly
review/document native versus normalized result/parameter representations,
error/SQLSTATE and connection reuse/retirement contracts; exercise a differing
fake backend through shared ODBC orchestration (including these capabilities),
then close architecture acceptance only after the dependency review and gates.

## A4 diagnostic and ownership contract — batch 10

`normalize_error_sqlstate` now interprets native server states in the selected
backend. It accepts a neutral statement context and returns an owned normalized
SQLSTATE or no mapping. PostgreSQL owns its vendor codes and context-sensitive
DDL mappings; the generic backend makes no PostgreSQL assumptions. Shared ODBC
validates the returned state's shape, chooses operation-specific fallbacks and
maps transport/timeout/unsupported errors. Immediate execution, preparation and
SQLMoreResults use this boundary. Deferred results lack reliable per-statement
DDL context, so their existing conservative fallback remains intentional.

The internal synchronous contract is:

- Input strings/views/spans are borrowed only until the call returns. ODBC handle
  serialization protects mutable session state; the interface does not promise
  independent concurrent calls on one connection.
- QueryResult owns rows, column names, normalized completions and nested
  results/errors in a flat ordered sequence. They survive subsequent calls,
  disconnect and backend destruction.
  A nullopt cell is NULL; an engaged empty string is a distinct non-NULL value.
  Type/catalog/capability views use their documented connection-lifetime storage.
- Native completion tags and table provenance IDs never cross the result boundary.
  Type IDs, sizes/modifiers, parameter type IDs and format codes exist only in
  private ParsedQueryResult storage. Shared metadata requires normalized_type
  and normalized_parameter_types; native interpretation/resolution hooks are absent
  from IDatabaseConnection. PostgreSQL binary wire results are rejected. Known
  binary/Boolean/text cells are normalized before return; richer scalar forms
  remain a documented migration step.
- A top-level Result error prevents exposing partial results. Successfully buffered
  earlier results can instead carry a deferred error in additional_results. Its
  native SQLSTATE travels with that result, independent of mutable last-error state.
  get_last_server_sqlstate supplies the immediately failed server operation, not
  a persistent diagnostic history. Callers use it only for QueryFailed.
- One absolute steady-clock deadline covers an operation and its nested I/O;
  metadata resolution and transaction commands preserve the supplied deadline.
  The shared caller owns timeout selection. Timeouts retire the ODBC connection;
  the PostgreSQL-family session also closes on failed/framing-invalid transport.
- A drained server error with a complete ReadyForQuery can leave the session
  reusable, although an explicit transaction may still require rollback. Protocol
  corruption and network failure retire the backend. COPY streaming is unsupported
  and retires the session; unsupported binary results rejected after draining do
  not by themselves retire it. SQLSTATE mapping never decides session reuse.

Tests verify normalized errors including malformed/unknown/ambiguous states,
owned diagnostic storage, direct/prepared/deferred mapping and recovery. A scripted
real PostgreSQL parser/session test retains NULL, empty and nonempty rows, metadata
and a deferred error after destruction. Existing liveness/deadline tests remain
required, rather than replacing them with contract-only assertions.

### Explicit remaining dependency review

A4/G9a remains open. Shared binary conversion still understands bytea text and
binary parameters still carry its encoding; boolean conversion also accepts native
PostgreSQL representations. These must be normalized or moved behind the backend
boundary, with existing binding/get-data/parameter tests preserved. Shared apply_query_result still classifies native command_tag text into ODBC
dynamic-function diagnostics; move that interpretation to normalized backend
metadata as part of the same remaining value/result work. GenericDatabaseConnection and
IProtocolParser are PostgreSQL-family protocol/session internals despite their
names; they are not a universal protocol SDK. A differing fake IDatabaseConnection
must exercise shared ODBC selection, results/NULL, diagnostics, unsupported features
and capability reporting after those leaks are addressed. No work is waived or
moved to public SDK packaging by this documentation.

## A4 binary parameters and completion metadata — batch 11

Binary QueryParameter values now contain raw bytes, including embedded NUL, and
preserve NULL versus engaged empty values. Shared binding converts ODBC character
hexadecimal input with a plain-hex utility and passes binary C buffers unchanged.
The binary_input flag preserves binary C input when the logical SQL type is not
Binary. PostgreSQL alone encodes those bytes as bytea hex for its text Bind format;
text parameters remain untouched. Wire-length limits account for expansion before
allocating encoded data. Existing indicator/length, Unicode, odd-character handling,
truncation and recovery behavior is preserved.

QueryResult now carries an optional normalized StatementKind. PostgreSQL interprets
CommandComplete tags while extracting each result, including additional results.
Shared ODBC maps the kind to dynamic-function names/codes and no longer reads the
native command_tag. Absent completion metadata preserves the caller's diagnostic
context; explicit Unknown clears it. The native tag remains available to backend
consumers and parser tests. Shared classification of application SQL for pre-execution
and failure diagnostics remains separate from native result interpretation.

Tests cover raw binary framing, NUL/high-bit/escape-like bytes, NULL/empty distinctions,
all-octet plain-hex round trips, malformed hex, and server-observed parameter bytes.
Completion tests cover recognized tags, prefix lookalikes, absent metadata and
per-result diagnostics for a CTE SELECT followed by an unclassified command and
another SELECT. Existing parameter and diagnostic integration suites remain gates.

This closes binary parameter and command-tag ownership extraction. The batch 10
remaining-work inventory is superseded for those two items. A4/G9a remains open
for result-side bytea/boolean representations, remaining conversion dependencies,
and differing fake-backend acceptance through shared ODBC orchestration. These
remain PostgreSQL-stage work; no Redshift compatibility claim is added.

## A4 final acceptance — batch 12

A1–A4 implementation and the required acceptance cases are complete. G9a closes
when this revision's full regression gates and CI pass; record the exact CI run
in the completion report before starting W1. Earlier open-item inventories are
historical and are superseded by this review.

`normalize_result_value` is the final native-value boundary. It is a pure,
no-I/O conversion of one non-NULL cell to an owned common value: raw bytes for
Binary, "0"/"1" for Boolean, unchanged text for other scalar families. nullopt
means invalid encoding, never SQL NULL. PostgreSQL owns hexadecimal/legacy bytea
and boolean text interpretation; the generic normalized profile has no PostgreSQL
escape rules. Shared bound-column and SQLGetData paths invoke that contract after
metadata resolution, retain diagnostics/truncation/chunking/NULL/output ownership,
and never interpret native encodings themselves. The text converter's binary
path now copies normalized bytes. Character-to-bit accepted literals remain
common application conversion rules, not native SQL_BIT result decoding.

The per-ODBCConnection factory is an internal selection seam. Its default still
uses DatabaseFactory and the configured transport. The same selected factory
supplies prelogin metadata and login/reconnect sessions, without a global testing
switch. It must transfer or release the transport and return a backend; input
views cannot outlive their synchronous call. Metadata views live as long as their
IDatabaseConnection object, not across replacement of that object on reconnect.

`test_backend_contract` implements IDatabaseConnection directly without any
PostgreSQL parser/session. Its deliberately different native IDs (23 is Binary,
17 is Boolean), byte/boolean encodings, error states, SQL translation and capability
profile exercise the real ODBC entry points and registry. Seven cases cover:

- Selection before/after login, configured transport ownership, metadata and ANSI/
  wide capabilities, empty capability strings, and unsupported catalogs/isolation/SQL.
- Bound fetch, normalized metadata, NULL versus empty, binary and boolean output,
  hexadecimal text output and chunked binary SQLGetData.
- Native/deferred errors, invalid normalized SQLSTATE fallback, malformed values,
  unchanged outputs on conversion errors, row status and recovery.
- Raw prepared parameters and the caller's absolute deadline.
- Timeout retirement/reconnect and network-failure liveness reporting.

The empty capability profile also exercises the narrow-output zero-byte copy;
that helper now avoids passing a null empty-view source to memcpy. Existing
PostgreSQL/native-parser lifetime, malformed-wire, recovery, metadata, parameter,
Driver Manager and sanitizer tests remain required. No existing regression gate
is replaced by the fake backend. Legacy disconnected-statement diagnostics and
other conformance gaps retain their existing audit/release scope; G9a is an
architecture gate, not full ODBC conformance certification.

### Final dependency review and boundary decision

| Area | Reviewed ownership / evidence |
|---|---|
| Creation and dialect (A1/A2) | DatabaseFactory/per-connection selection; no PostgreSQL parser include in shared ODBC; marker/translation calls use the backend. Fake SQL translation dispatch is observed. |
| Native IDs, domains, catalogs (A3) | Shared ODBC consumes normalized column/parameter metadata only. Native interpretation/resolution hooks and ID-keyed map storage belong to the private PostgreSQL-family implementation. TypeDefinition/CatalogRequest remain normalized SDK services. |
| Capabilities, transactions and diagnostics (A4) | Neutral records/context and backend normalization; shared ODBC constants, fallbacks, handles and output validation remain shared. |
| Result/parameter encodings and completions (A4) | Native cell decoding, binary Bind encoding and command-tag interpretation are backend-owned. Shared byte/bit conversions and normalized StatementKind mapping are database-independent. |
| Lifetimes, deadlines, reuse (A4) | Owning QueryResult/QueryParameter and documented borrowed views; caller deadline propagation, backend liveness and timeout retirement are covered by existing and fake tests. |
| Protocol internals | GenericDatabaseConnection/IProtocolParser still implement PostgreSQL-family framing/authentication below IDatabaseConnection. The direct fake proves shared ODBC does not require them; a universal protocol abstraction is not a G9a requirement. |
| Product configuration | Compiled PostgreSQL identity, default endpoint/database values and PostgreSQL build adapters remain product defaults. They are not native query/type interpretation; explicit settings and a different backend are exercised. The bounded compiled MySQL target is G12; runtime plugins and public packaging remain G13. |

No known required A1–A4 extraction is deferred. G12 now owns the independent
MySQL protocol proof, backend test kit and unstable out-of-tree example. G9b
still requires real Redshift reuse/compatibility evidence; G13 owns public SDK
packaging and stability. W1–W4 are complete. G8 real-application acceptance
remains necessary before PG-BETA, although its host-dependent execution is
explicitly deferred while MS1 proceeds.

## S2 owning query failures — 2026-10-01

`execute_query`, `execute_prepared` and `describe_statement` now return
`BackendResult<QueryResult>`. Each immediate failure owns a `BackendError` with
its error class, message, optional native state/code, operation, passive session
state and disposition. Retry evidence is absent by default. Existing error-code
and message accessors preserve diagnostic compatibility; ODBC maps the returned
native-state snapshot instead of asking the connection for mutable SQLSTATE.
Errors survive subsequent calls, disconnect and backend destruction.

PostgreSQL derives passive state from validated ReadyForQuery: idle,
transaction, or failed transaction. An idle completed server failure is
`Reusable`; transaction/failed-transaction or unknown ready state requires
`ResetRequired`. Transport, timeout, malformed-protocol and ambiguous partial
write failures physically retire the session and return `Retire`. Disposition is
not inferred from SQLSTATE and does not imply that a pool/reset facet exists.
`Reusable` means protocol-ready for the current owner, not clean for another
borrower; pool isolation still requires its separate reset contract.
The transitional default passive state for other implementations is `Unknown`
when connected, never an implicit idle/reuse claim.

This batch migrates immediate query errors only. Type resolution now uses owning
map results; setup/authentication and transactions use owning void
results as described below;
ordered/deferred result errors remain in owning QueryResult until the normalized
results migration. Mutable SQLSTATE and error-message getters/storage have been
removed from the backend connection interface.
Primary messages preserve existing server diagnostic behavior; this does not
complete the safe-message/logging security contract. Internal C++ interfaces are
unstable; the exported ODBC surface and diagnostics remain protected.

## S2 owning transaction and isolation failures — 2026-10-01

Transaction control and isolation changes now return `BackendResult<void>`.
PostgreSQL preserves the complete query BackendError snapshot, including native
state/code, classification and disposition, and labels begin, commit, rollback
or isolation context explicitly. The shared ODBC begin-transaction helper also
preserves that complete error; existing ODBC diagnostic codes and timeout
handling are unchanged. There is no automatic retry or replay.

Local invalid-enum and unsupported-operation failures snapshot passive state
without I/O: disconnected sessions report Retire, idle sessions Reusable, and
transaction/failed-transaction/unknown sessions ResetRequired. The local helper
accepts only validation/unsupported failure kinds, not transport/protocol errors.
Copy/move and later-success tests verify error ownership; actual protocol
fixtures verify state and ambiguous-failure retirement through both adapters.
Safe-message/logging migration remains open. This does not
implement a reset/pool facet or complete A5.

## S2 owning setup and authentication failures — 2026-10-01

Connection setup now returns `BackendResult<void>` with Connect, Authenticate
or Startup context. Server failures retain native SQLSTATE in the returned
snapshot; existing error codes and ODBC diagnostic mapping are preserved.
Authentication-phase errors and later SQLSTATE class 28 login rejections retain
authentication classification. Other failures after AuthenticationOk carry
Startup context. Cleanup completes before recording passive state/disposition,
so failed transport/authentication attempts report Disconnected/Retire.

Local validation occurs before transport mutation. Invalid reconnect settings
leave a previously idle session available to its current owner and report
Idle/Reusable without I/O; this is not a reset or pool-isolation guarantee.
Tests cover successful setup, server rejection, authentication timeout,
malformed exchanges, cleanup, copy/move/destruction ownership and invalid
reconnect snapshots. No automatic retry or replay is introduced.

The last mutable error-message side channel is removed from connection and
prototype wrapper interfaces. Primary messages still preserve existing server
diagnostics; safe-message/logging and deferred-result normalization remain
separate work. Internal C++ interfaces remain unstable.

## S2 owning type-resolution failures — 2026-10-01

`resolve_types` now returns `BackendResult<ResolvedTypeMap>`. PostgreSQL
preserves the complete underlying query error and labels ResolveTypes context,
including native state/code, classification, passive state and disposition.
Timeout, partial-write and malformed-wire failures retain physical retirement;
there is no automatic retry. Empty/known types and generic parser fallbacks
still require no catalog query. Caller deadlines, domain resolution and missing
native-type fallback behavior are unchanged.

Invalid catalog rows return no partial map. They retain the existing QueryFailed
code/message used by ODBC, with InvalidMetadata classification and a passive
snapshot. This is distinct from an ambiguous wire-protocol failure.
Because the query has drained, this validation failure does not itself retire
an idle session. Transaction or unknown state still requires ResetRequired;
disconnected state reports Retire. Native details and retry evidence are absent
for locally detected invalid metadata. Tests cover all passive states, owning
snapshots across later success/destruction, real protocol server errors,
rollback recovery and no additional sends after ambiguous-failure retirement.

ODBC parameter-cache updates remain atomic after complete-map validation, and
its existing diagnostics are preserved. Safe-message/logging and normalized
ordered/deferred results remain open; this does not complete A5 or S2.

## S2 failure-log summaries and component bounds — 2026-10-01

ODBC connection/direct/prepared failure logs now use fixed public summaries,
with SQLSTATE and operation/timing context. Raw server, transport and exception
messages remain available through the existing ODBC diagnostic path, not those
failure-log messages. Debug/Trace does not enable raw diagnostic logging.
Tests verify an attacker-controlled DSN marker remains in its diagnostic and is
absent from the log, alongside existing diagnostic-retrieval compatibility tests.

Logger message/field values are bounded to 1024 input bytes before escaping;
event/field keys are bounded to 64 input bytes. Text output escapes control bytes,
including NUL/ESC/DEL, and keys/events as well as values. JSON retains its control
escaping and receives the same input bounds. Long values carry a truncation
marker; truncation preserves valid UTF-8 character boundaries. Tests verify
hostile records stay on one line, exclude oversized tails, and retain multibyte
characters that fit exactly while dropping split tails.

This is a partial security-contract migration. Typed Public/Sensitive/Secret
fields, default sensitive-field redaction, total-record budgets and safe BackendError
summaries remain open. Existing opt-in query logging can expose SQL and is not a
sensitive-diagnostic mode. No logging security gate or A5 closure is claimed.

## S2 typed log fields and encoded payload budgets — 2026-10-01

LogField now carries Public, Sensitive, Secret or QueryText classification.
Unclassified fields default to Sensitive and emit only [redacted]. Secret fields
are omitted with their keys; Debug/Trace and query opt-in do not override either
rule. Public ODBC operation/state/timing and numeric counters are explicitly
annotated. Host/database identifiers remain sensitive. QueryText is emitted only
when the existing LogQueries opt-in and Debug-level availability both hold.
No sensitive-diagnostic debug mode is introduced.

Encoded record payloads are capped at 8192 bytes with at most 32 emitted fields,
including expansion from escaping. Space is reserved for complete closing syntax
and a fixed fields_truncated indicator. These bounds apply to serialization,
not prior caller allocation or the sink's timestamp/process/thread envelope.
Default logging-off behavior and ODBC diagnostics remain unchanged. Tests cover
Trace/query-opt-in isolation, implicit sensitivity, secret key removal, escaped
expansion, excessive field count, later usable records and complete JSON output.

Safe BackendError summaries, adversarial backend diagnostic fixtures, invalid
source-encoding handling and aggregate result budgets remain planned work.
This does not close the complete logging security contract, A5 or S2.

## S2 public BackendError summaries — 2026-10-01

BackendError now exposes safe_summary(), a noexcept view of fixed public text
selected only by its error class. It never formats detailed message, native
state/code, SQL, identifiers or credential material. Unknown classes use a fixed
fallback. The owned message remains explicitly diagnostic detail for trusted
consumers; error_message() compatibility and ODBC diagnostic behavior remain.
Summary storage is static and survives error mutation/copy/move/destruction.

Typed setup/isolation/begin/direct/prepared failure log paths consume this summary.
Exception paths retain fixed summaries because no BackendError exists there.
Tests inject a secret-bearing backend message through direct and prepared ODBC
execution: diagnostics keep exact text and normalization while Trace logs contain
only the fixed summary. Class coverage verifies summaries ignore sensitive fields.

This strengthens the immediate-error boundary, not deferred-result normalization.
Raw diagnostics may contain sensitive data and must not be passed to log messages.
Invalid-source encoding, deferred errors, aggregate budgets and safe reuse remain
planned work; A5/S2 and crypto qualification gates remain open.

## S2 owning deferred errors — 2026-10-01

QueryResult now carries optional BackendError rather than parallel raw error
message/native-state strings. PostgreSQL parser errors own the existing diagnostic
prefix and native state. An engaged error is detected independently of message
emptiness. After the whole response drains, the session annotates deferred errors
with originating operation and final passive state/disposition; it never infers
reuse from SQLSTATE. These are exchange-end snapshots, not the state at each
individual statement inside a batch. Additional results are an ordered flat list.

SQLMoreResults consumes the owned error, preserves diagnostic normalization/text,
and clears remaining pending results on error as before. Tests cover parser first
and deferred errors, direct ODBC diagnostic compatibility, null/empty/value rows,
idle/transaction/failed-transaction snapshots, copy/move, later connection
retirement and backend destruction. Retry/native-code evidence remains absent
unless supplied explicitly; safe summaries are available on deferred errors.

This removes the legacy result error-string side channel. Native metadata fields,
materialized result buffering, aggregate budgets and session/reset facets remain
separate migration work; A5/S2 and provider qualification gates stay open.

## S2 buffered response wire budgets — 2026-10-01

ConnectionSettings carries ResponseLimits for one query/description exchange:
64 MiB cumulative wire bytes and 100000 messages by default, including control
frames. SDK callers may choose finite ceilings; byte limits must allow a five-byte
header and message limits must be positive. Invalid limits are rejected before
connection mutation. ODBC profile/connection-string exposure remains separate.

The session checks message count before reading the next frame and passes the
remaining byte allowance to the frame reader. A declared frame exceeding that
allowance is rejected after its header and before allocating/reading its payload.
Subtraction-based remaining capacity avoids cumulative overflow. Limit failures
return typed ResourceLimit with fixed detail/public summary, no partial results,
and mandatory physical retirement because the exchange is not drained. ODBC maps
this new error to HY000 and connection liveness reports dead; no retry is implied.

Tests cover exact/one-less byte and frame limits, direct/prepared/description
oversize rejection after only five read bytes, cleanup/no later sends, and invalid
reconnect preserving a valid session. Wire budgets bound input and frame-count
buffer growth, not exact decoded heap consumption or caller allocations.
Authentication/startup, rows/cells/columns/results, metadata/diagnostic lengths,
configuration exposure and allocation-failure contracts remain open SEC-4 work.
This batch does not close SEC-4, A5/S2 or a reuse/crypto qualification gate.

## S2 authentication/startup response budgets — 2026-10-01

ConnectionSettings has independent startup_response_limits, defaulting to 1 MiB
wire bytes and 10000 messages for the authentication/startup response exchange.
Counts include authentication messages, notices, parameter status, backend key
and final ready state; they do not include TLS-provider handshake traffic or
outgoing credentials/startup packets. The same absolute login deadline applies.
SDK overrides remain finite and validate before connection mutation; product
configuration exposure remains separate.

Remaining byte allowance reaches the header reader, rejecting declared oversized
payloads before body allocation/read. Message overflow stops before the next
frame. Errors retain Authenticate or Startup context and owning ResourceLimit
classification; existing failed-connect cleanup closes once and snapshots
Disconnected/Retire. Exact limits succeed without weakening TLS/auth policy.
Tests cover exact/one-less bytes/messages, notice accounting, default huge-frame
header-only rejection, cleanup and invalid reconnect without transport mutation.
The incremental-read regression test uses an explicit larger ceiling so it still
verifies no allocation based solely on a declared frame length.

These are input budgets, not exact heap quotas. Rows/cells/columns/results,
metadata/diagnostic limits, product exposure and allocation-failure boundaries
remain SEC-4 work. A5/S2, G12 and provider qualification gates remain open.

## S2 decoded result count budgets — 2026-10-01

ResultLimits adds SDK ceilings per query/description exchange: 1000000 rows,
4000000 cells, 4096 columns per RowDescription or parameters per
ParameterDescription, and 1024 completed results. Rows/cells and completions are
cumulative across ordered results, including SQL NULL/empty cells and error or
empty-query completions. Zero row/cell/description ceilings permit no-data work;
max_results must be positive and validates before connection mutation.

The PostgreSQL-family session reads only bounded count headers to account frames
before retaining them for decoded extraction. Exceeding a ceiling returns owning
ResourceLimit/HY000, retires the undrained exchange, and exposes no partial result.
Protocol ordering validation remains in force. These checks are backend framing
work, not PostgreSQL tags leaking into shared ODBC workflows.

Tests cover exact/one-less limits in all four dimensions, parameter description,
aggregate counts across two row-bearing results, null/empty cells, zero-data
metadata and invalid reconnect without I/O. Defaults/configuration are SDK-only;
no public profile support claim or exact heap quota is made. Aggregate metadata
entries/names, diagnostics, outbound input, product exposure and allocation
boundaries remain SEC-4 work. Native normalization and safe reuse remain S2 work.

## S2 native metadata hooks removed from SDK — 2026-10-01

The earlier A3 native-ID service adapter is superseded: IDatabaseConnection no
longer requires describe_type or resolve_types. Backend implementations supply
normalized_type and normalized_parameter_types directly. PostgreSQL-family
parser interpretation and atomic domain resolution retain their private hooks,
original deadlines, owned errors and same-session recovery. ResolvedTypeMap now
lives in a separate private header, outside the normalized scalar metadata
header. The independent backend compiles without any native-ID service, and
compile-time checks reject their reintroduction on the SDK interface.

This removes unused SDK service requirements without changing ODBC behavior.
QueryResult native migration fields still exist until parser/result storage is
split; ordered execution, richer canonical values and session facets remain open.

## S2 private parser-result split — 2026-10-01

SDK `QueryResult` now exposes only normalized column and ordered parameter
metadata. Native type IDs, sizes/modifiers and wire format codes reside in private
`ParsedQueryResult`; PostgreSQL-family parsing and decoding stay below the session
boundary. Each drained response is validated and converted to an owning SDK
snapshot before publication. Additional parser results must be flat and ordered;
nested private results fail atomically as invalid metadata.

Prepared/description parameter resolution uses the original absolute deadline.
Resolver errors preserve their owning session/disposition snapshot, including local
input-limit rejection without I/O; incomplete resolution returns no partial result
and records the drained session state. Direct execution never triggers parameter
resolution, so catalog lookups cannot recurse through this conversion.

Compile-time SDK tests forbid native metadata fields. Parser tests retain native
wire-offset/format coverage; session/ODBC tests cover normalized ownership, primary
and additional results, malformed metadata/cells, binary-format rejection, lookup
failures and reusable-session recovery. Ordered execution items, richer scalar
forms, session/reuse facets and provider qualification remain open S2 work.

## S2 execution-sequence validation — 2026-10-01

The materialized SDK execution contract now explicitly requires a flat ordered
sequence. A primary operation failure uses BackendResult's error alternative;
additional deferred errors are error-only items and cannot also carry schema,
rows, cell errors, parameter descriptions or completion metadata. This prevents
silent item loss or publication of contradictory success/error results.

PostgreSQL validates the normalized sequence after response drain and before
return. Shared ODBC validates the entire sequence before queuing additional items
or publishing result state. Contract violations return fixed metadata diagnostics;
a protocol-ready session stays available to its current owner. This does not add
replay or pooling guarantees.

Tests cover direct and prepared rejection/recovery for nested sequences, root
errors and every contradictory deferred-item payload, plus native-parser
contradictions with post-drain recovery. Happy-path ordering distinguishes a
nonzero update count, an empty rowset with schema, a zero update count and a
deferred server error. Existing native multi-result tests protect PostgreSQL
ordering. A dedicated ExecutionItem/ExecutionResult representation, warning items,
final execution disposition and separate parameter-description delivery remain
open; this checkpoint enforces the current materialized contract rather than
closing the whole ordered-execution milestone.

Statement description has a separate metadata-only gate: primary errors, additional
items, rows and cell-error ledgers are rejected before parameter/column metadata
is published or cached. SQLDescribeParam/SQLNumResultCols rejection leaves caller
outputs unchanged and a later valid description can retry on the same session.

## S2 owning operation session snapshots — 2026-10-01

BackendResult now provides a passive, owning SessionSnapshot for both success and
failure. Successful PostgreSQL query, prepared execution and statement description
outcomes record their final state/disposition after response drain and parameter
resolution. The snapshot belongs to the operation envelope, not individual
rowsets; callers retaining only QueryResult deliberately retain data rather than
the operation status. Deferred errors retain the same final session context.

Failures derive the snapshot from the existing owning BackendError, so later
internal error annotation has one source of truth. Successes that do not report a
snapshot default to Unknown/Retire. Void results support explicit snapshots, but
connect/transaction/health/reset success annotation remains later session-facet
work; no blanket success-status or production pooling claim is made.

Reusable means protocol-ready for the current owner at operation completion. It
is not a health probe, reset completion, permission to pool or a promise that a
later call cannot fail. Transaction/failed-transaction outcomes require reset.
Copy/move, later ambiguous retirement, disconnect and backend destruction cannot
rewrite retained success snapshots. Tests cover all three PostgreSQL states,
prepared/description completion, deferred-error agreement, conservative defaults
and the error record's single source of truth. Dedicated ordered ExecutionItems,
warning delivery and separate parameter descriptions remain open S2 work.

## S2 connection and transaction success snapshots — 2026-10-01

Successful PostgreSQL connection outcomes now own the final passive startup
state/disposition. Successful transaction and isolation adapters preserve the
underlying command outcome's SessionSnapshot rather than discarding it or
recomputing it from a later mutable session reading. Existing failure records,
operation context and absolute deadlines remain unchanged.

Tests cover BEGIN/COMMIT/ROLLBACK/isolation wire completions, all adapter actions
and isolation levels, conservative Unknown/Retire propagation and reported states
differing from the probe's current state. Connection tests retain snapshots across
copy/move, invalid reconnect rejected before I/O, later ambiguous loss, disconnect
and backend destruction. Invalid reconnect preserves the current owner's state;
it neither changes the earlier connection outcome nor grants a pooling lease.

This supersedes the earlier deferral of connection/transaction success annotation.
Health/reset/lease facets, optional service separation, ordered execution items
and warning delivery remain open S2 work. A completed BEGIN requires reset even
though it succeeded; no automatic replay, health check or production pooling
support is added.

## S2 provider-owned SQL dialect service — 2026-10-01

ISqlDialect is a pure provider-owned service for parameter-marker counting and
SQL escape translation. IDatabaseConnection no longer requires either method.
PostgreSQL-family providers expose an immutable service using the existing lexer
and translator; native parser composition helpers remain private implementation
code. SQL dialect rules do not require credentials, a transport or a live session.

ODBC direct/prepared execution and SQLNativeSql A/W now use the selected provider
service. Existing connection, buffer/Unicode, SQL byte-budget, marker-budget,
NoScan and diagnostic guards retain their order and behavior. Translation strings
are owning snapshots; service references last for the provider lifetime.

The independent synthetic backend session compiles without dialect methods; its
provider owns the dialect. Tests cover use before session creation, borrowed
input overwritten after translation, retained results after provider destruction,
error/recovery, PostgreSQL quotes/comments/dollar quotes and stable service
identity across session creation/destruction. Compile-time checks prevent SQL
services returning to the SDK session interface.

This closes the pure SQL-dialect extraction within S2 session/service separation.
Type/catalog and optional transaction/description/reuse facets, internal target
separation, ordered execution representation and warning delivery remain open.
No MySQL protocol or speculative Redshift specialization is introduced.

## S2 optional transaction-session facet — 2026-10-01

Transaction control and isolation now live on the optional ITransactionSession
facet. IDatabaseConnection exposes stable borrowed facet discovery, defaulting to
absence, and no longer requires transaction/capability/isolation methods.
PostgreSQL implements the facet; the generic PostgreSQL-family machinery and
independent synthetic backend no longer need unsupported transaction stubs.

Shared ODBC obtains transaction capabilities from a live facet when connected.
Before connection, or after an unsuccessful open has disconnected its session,
the immutable provider declaration supports attribute planning. A provider
advertisement cannot grant missing live behavior: guarded adapter dispatch
returns an owning unsupported error without I/O. A failed deferred-isolation
open can correct its settings and retry. Existing PostgreSQL command/deadline,
error annotation and success snapshots are preserved.

Tests cover absent facet discovery, stable PostgreSQL facet lifetime across
open/disconnect, closed-session failures, live capability suppression despite
provider overadvertisement, no-I/O autocommit/isolation rejection, safe command
recovery and failed-open setting correction/retry. Compile-time checks keep
transaction operations off the required SDK session interface. Existing native
transaction and ODBC suites protect happy-path and failure behavior.

This extracts transaction behavior only. Statement-description/catalog/reuse
facets, static type services, internal build targets and the ordered execution
representation remain S2 work. No reset/health/pooling guarantee is added.

Statement description is optional through IStatementDescription. Facet discovery
is passive and borrowed for the session lifetime, including disconnected state;
returned metadata and operation snapshots remain owning. Missing pre-execution
description rejects metadata discovery without I/O and does not prevent execution
or access to metadata from a completed execution. Live capability reporting must
not overadvertise absent description behavior.

Catalog discovery is optional through ICatalogQueries. Its stable session-owned
facet builds owning SQL without I/O; generated queries execute through the ordinary
execution path. Missing discovery rejects catalog calls before cursor mutation or
I/O. SQLGetFunctions must suppress absent catalog functions in every support format;
SQL namespace features and static advertised types are independent contracts.

Advertised normalized type definitions are provider-owned immutable policy selected
using an explicit server-version snapshot. Borrowed input is not retained, and
returned spans/strings remain valid for the provider lifetime across selections.
An empty version selects a conservative profile; a retained advertisement does not
imply connectivity or reuse safety. Live native type resolution remains backend-private.

Static capabilities and native-state diagnostic policy belong to the immutable
provider. Capability string views last for the provider lifetime; the ODBC adapter
masks copies for missing live facets. Sessions do not borrow product identity or
policy from the provider. Native mapping borrows inputs only until return and
returns owning SQLSTATE text or no mapping; it never grants retry/reuse safety or
overrides class-first and operation-specific diagnostic fallbacks.


The internal `odbcpp::sdk_contracts` build target exposes only the explicit
`sdk/contract_headers.txt` manifest in a generated include tree. Contract headers
must form a closed dependency graph using only allowlisted standard C++ headers;
ODBC, concrete backend, parser and crypto-provider dependencies belong outside
this surface. Individual-header compilation and negative boundary fixtures enforce
this rule. The target is independently configurable from `sdk/`; runtime target
separation, public SDK packaging and external-author qualification remain open.
