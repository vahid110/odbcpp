# Redshift fetch-mode review — RP-INV-02

Recorded 2026-10-02. Bounded source review of fetch-mode configuration and
lifecycle, extending F-RS3-01/02 and B-RS3-01/02 in
[REDSHIFT_PARITY_INVENTORY.md](REDSHIFT_PARITY_INVENTORY.md). No runtime change,
credential access, driver build, test/CI execution, live Redshift evidence or
qualification occurred. All rows remain **unaudited for our implementation**.
No exception or implementation design is approved by this review.

Reference: official AWS source at
[`56d35297f9bee0cc31c0148581c87ca455639a39`](https://github.com/aws/amazon-redshift-odbc-driver/tree/56d35297f9bee0cc31c0148581c87ca455639a39),
source version `2.2.4 0`. The first inventory records the official v2.2.4 tag's
separate SHA/version declaration and the absence of a runtime source delta.
Source/binary correspondence is still unverified. Every source link here uses
the same pin. **S** is a source statement/branch; **D** is documentation/release
notes; **R** is a proposed requirement, test or inference. No source observation
is proof of server behavior. Test IDs denote proposed fixtures, not passing runs.

P0/M2: resolve defaults, supported paths and semantics before freezing scope.
P2/RP1: fetch breadth, promoted to P1/M3 when a required workload needs bounded
fetching. The SDK/MySQL proof stays outside this package. Live-dependent work is
estimated separately; this review closes neither M2 nor G14.

## Source map

| Ref | Inspected locations / purpose |
|---|---|
| C1 | [rsodbc.h keys/defaults](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsodbc.h#L1165), constructor fields at 1383–1410 |
| C2 | [rsconnect.cpp exclusivity](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsconnect.cpp#L590), DriverConnect two-pass resolution at 686–692, property parse at 2415–2479, DSN reads at 3345–3445, integer/Boolean helpers at 3755–3830 |
| C3 | [Windows setup defaults](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsodbc_setup/setup.c#L202), settings at 444–453, alias map at 599–601, UI load/save at 2363–2366 / 3589–3592 |
| E1 | [rslibpq.c direct/prepare gate](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rslibpq.c#L1265), other-portal closure at 928–933 / 980, eligibility helper at 5365 |
| E2 | [rslibpq.c portal execution/refill/close](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rslibpq.c#L5451), execution at 5491–5680, refill at 5692–5809, close at 5821–5892; empty-suspension retry constant at 29 |
| L1 | [rsresult.cpp fetch integration](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsresult.cpp#L1694), SQLRowCount at 947–957, SQLMoreResults at 498–580, streaming error handling at 1736–1775 |
| L2 | [rsprepare.cpp close helper](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsprepare.cpp#L590), [rsutil.c query reset](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsutil.c#L12856), stream drain/open checks at 16177 / 16262 |
| L3 | [rsexecute.cpp cancellation](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsexecute.cpp#L620), [transaction helper](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rslibpq.c#L669), [SQLEndTran mapping](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rstransaction.cpp#L118), disconnect/free paths in rsconnect.cpp at 1410–1505 / 1988–2034 |

## Atomic settings and resolution rows

| ID | S: pinned fact | R: decision or fixture target |
|---|---|---|
| FM-C01 | C1: connection constructor initializes UseDeclareFetch=0, Fetch=0, StreamingCursorRows=0 and CscEnable=0. Batch fallback constant is 100. | T-FM01 omitted settings must record creation path, not assume every installed DSN uses constructor defaults. |
| FM-C02 | C2: case-insensitive UseDeclareFetch/UDF selection; Boolean helper returns true for case-insensitive TRUE or literal 1, false otherwise. | T-FM02 canonical/alias mixed case, 0/1/true/false, yes/2/empty, whitespace and duplicates. Do not silently copy permissive false conversion into a new strict contract. |
| FM-C03 | C2: Fetch is parsed with integer scanning; negative values normalize to 0. No explicit upper bound or checked overflow branch appears in the inspected parse block. | T-FM03 0/negative/1/100/large, leading signs/whitespace, suffixes, nonnumeric and overflow. Source does not establish portable overflow results or a safe memory ceiling. |
| FM-C04 | C2: StreamingCursorRows/SCR use integer scanning; negative values normalize to 0. | T-FM03 repeat invalid/range cases independently; SCR and Fetch are different settings. |
| FM-C05 | C2: CscEnable numeric values other than 0/1 normalize to 0; DSN processing disables streaming when CSC is enabled. | T-FM04 CSC plus streaming through DSN and DSN-less input; trace downstream CSC selection before assuming identical normalization in every path. |
| FM-C06 | C2: enabled UDF clears CSC and streaming; Fetch <=0 becomes 100. If UDF is disabled, this helper does not alter other modes. | T-FM04 all pairs/triple and key orders, UDF=0 after UDF=1, positive Fetch preservation. Apply effective-mode resolution after final settings, not per isolated key. |
| FM-C07 | C2: exclusivity leaves FetchRefCursor unchanged. | T-FM05 refcursor SELECT versus CALL; an unchanged option does not guarantee expansion in the portal path. |
| FM-C08 | C2: DriverConnect first extracts DSN, reads DSN properties, then parses all connection-string options. | T-FM06 supplied string values overriding conflicting DSN seeds. AuthProfile/prompt/BrowseConnect full precedence remains outside this bounded proof. |
| FM-C09 | C2: DSN UDF/Fetch readers preserve existing values when keys are absent/empty; integer helper scans a string seeded from its current value. Direct SQLConnect applies exclusivity after DSN/AuthProfile processing. | T-FM06 absent versus empty values, repeated connects, direct SQLConnect versus DriverConnect, canonical versus alias DSN names. Full INI/registry fallback hierarchy remains unreviewed. |
| FM-C10 | C3: setup defaults are StreamingCursorRows=100, Fetch=100, UDF=0 and CSC=0. Alias map includes SCR/UDF; UI loads/saves stream rows, UDF checkbox and Fetch cache-size text. | T-FM07 setup-created/saved DSN versus explicit minimal DSN versus DSN-less connection; record actual persisted values. Setup seeds differ from constructor/doc streaming default 0. This is a source discrepancy, not evidence every Windows install enables streaming. |
| FM-C11 | E2: first execution and refill independently fallback to batch 100 for nonpositive Fetch. | T-FM03/08 verify resolution and wire-visible batches agree; runtime fallback is not a validated maximum. |

FM-C01–11 extend F-RS5 configuration coverage only for these fetch controls.
All other option families remain in the first inventory's unreviewed ledger.

## Atomic eligibility and lifecycle rows

| ID | S: branch / output observation | R: expected decision and differential fixture |
|---|---|---|
| FM-E01 | E1: portal route requires enabled UDF, forward-only cursor, no bound parameters, unprepared execution and no catalog/function call. | T-FM08 each gate independently: direct SELECT, prepared SELECT, direct parameter marker, catalog API, CALL, scrollable cursor. An enabled UDF flag does not guarantee bounded delivery for excluded paths. |
| FM-E02 | E1: lexical helper skips leading space/tab/newline/CR, accepts SELECT or WITH with stated boundaries; leading comment is not skipped by this entry check. | T-FM09 positive/negative lexical pairs, leading comments, keyword boundaries and malformed SQL. Eligibility is a routing heuristic, not a SQL validity promise. |
| FM-E03 | E1: helper rejects detected SELECT INTO, modifying/INTO CTE and multiple statements; handles quoted/comment/dollar-string constructs in helpers. | T-FM09 finite corpus from upstream leads plus prepared equivalent. Inspect semantic effects, not only route return value; never route writes invisibly. |
| FM-E04 | E1: named-portal path returns before shared refcursor expansion; CALL stays on its ordinary path. | T-FM05 raw refcursor SELECT versus procedure CALL in materialized, streaming and portal modes, with FetchRefCursor 0/1. Server objects/capabilities needed. |
| FM-E05 | E2: idle connection gets BEGIN before Parse/Bind/Describe/Execute; first-result status must be tuples or suspended, then results are drained and portal state stored. | T-FM10 record transaction state before/after execute and failures. Source comment about server portal survival needs Redshift confirmation. No assumption that protocol-ready means SDK reusable. |
| FM-E06 | E2: needs-commit flag is set only when this path started BEGIN and autocommit is ON; autocommit OFF leaves commit to caller. | T-FM10 idle ON/OFF, preexisting transaction ON/OFF, explicit commit/rollback while suspended, switch autocommit. Record transaction effects on unrelated changes. |
| FM-E07 | E2: startup failure drains, closes named portal/statement only after successful Parse, rolls back if it started BEGIN, and clears portal flags. | T-FM11 inject BEGIN/Parse/Bind/Describe/Execute/result-install failures; distinguish caller-owned transaction from driver-started transaction. Rollback completion was not validated in this helper. |
| FM-E08 | E2/L1: refill is triggered at batch boundary only while active+suspended; it resumes Execute without Describe and uses prior batch metadata. Positive refill replaces PGresult; result offset advances and row index resets. | T-FM12 stable schema and monotonic row order at boundaries; NULL/Unicode/binary, chunked SQLGetData, row arrays and MaxRows. Row-batch ceiling is not aggregate-byte qualification. |
| FM-E09 | E2: inactive/not-suspended refill returns 0; terminal empty result returns 0. Suspended-empty refills retry at most five attempts, then HY000/error sentinel and close. | T-FM12 0/1/N-1/N/N+1/2N/2N+1 rows; T-FM11 adversarial empty suspension, missing prior result and missing result. Do not report contradictory peer state as empty success. |
| FM-E10 | E2: failed resume/status/missing-result path adds HY000 in sampled branches, calls common portal close and returns -1; L1 maps negative refill to SQL_ERROR. | T-FM11 exact diagnostic chain/native state, delivered-prefix preservation, synchronization and subsequent operation. Upstream close on refill failure may commit a driver-started autocommit transaction; that is a policy review item. |
| FM-E11 | E2: close sends protocol Close for portal and statement, drains results and may COMMIT; send/commit failures are logged, flags cleared and void returned. Bad/disconnected PGconn clears flags without protocol close. | T-FM13 server/transport/commit failure during close. Our SDK requires observable error/disposition and retirement on uncertain synchronization; logging/flag clearing cannot prove reusable state. No automatic upstream-bug claim. |
| FM-E12 | L2: SQLCloseCursor's internal helper invokes portal close, releases results, marks closed and returns SQL_SUCCESS. Re-execute/prepare reset closes the current portal; SQLFreeStmt CLOSE/DROP shares internal close. | T-FM13 partial fetch then close/free/reprepare/re-execute; check diagnostics, post-state and retained bindings. Success from this helper does not verify server cleanup. |
| FM-E13 | E1: before execution on a connection with UDF enabled, other active statement portals are closed; current statement excluded. | T-FM14 two statement handles: fetch A partially, execute B, then resume A; both autocommit modes and explicit transaction. Required app workflows may rule out this concurrency profile. |
| FM-E14 | L1: SQLRowCount returns -1 while portal is active. Streaming reports in-memory rows after end-of-stream, materialized mode reports in-memory rows. | T-FM15 count before fetch, between batches, after exhaustion, after MoreResults and after close. Do not infer total count becomes known automatically for an exhausted portal. |
| FM-E15 | L1: sampled SQLMoreResults releases current result and uses streaming-specific next-result logic; no portal-specific close appears in this function. | T-FM15 call MoreResults after partial/full portal fetch, then cancel/close/re-execute. Portal flags/resources after result release need differential/live evidence. |
| FM-E16 | L3: cancel treats still-running execution thread, in-flight command with no current result, or need-data state as active. Idle open cursor closes in ODBC2, no-ops in ODBC3. | T-FM16 cancel before first result, between batches and during suspended-portal resume. Existing result head during refill may prevent the synchronous in-flight test from recognizing active work; verify, do not infer successful refill cancellation. |
| FM-E17 | L3: active-query cancel delegates a request to libpq; its success is request dispatch, not observed query termination. Need-data cancellation resets client state. | T-FM16 delayed server cancellation/deadline, repeated cancel, next fetch diagnostics and connection disposition. Trace lower protocol/TLS cancel channel separately before acceptance. |
| FM-E18 | L3: transaction helper drains streaming rows then sends transaction command. Sampled SQLEndTran/SQLTransact path supplies connection or environment transactions. | T-FM10 transaction commands with suspended portal and unread streaming result. No explicit portal-flag reconciliation appeared in the sampled transaction helper; full downstream trace remains open. |
| FM-E19 | L3: SQLDisconnect delegates physical disconnect; connection free drops statement handles using their close path. | T-FM13 disconnect with partial/failed batch, reconnect and server-resource checks. Full libpq disconnect/drain behavior was not audited. |

## Streaming branches to compare

| ID | S: sampled branch | R: fixture / limit |
|---|---|---|
| FM-S01 | [isStreamingCursorMode](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rslibpq.c#L3090) requires positive stream rows and forward-only cursor. | T-FM08/12 materialized versus positive SCR; scrollable path separate. Full CSC/thread/protocol receive implementation remains unreviewed. |
| FM-S02 | L1: refill errors/replaced result detach the obsolete PGresult, select native SQLSTATE when available or 08S01, mark stream ended and return SQL_ERROR. | T-FM11 delayed server error versus socket/protocol failure, no double-free, post-error second statement. Commented integration-test name is a lead, not a test found/executed here. |
| FM-S03 | L2: connection-level stream drain skips unread results of an active stream; another-open-stream predicate identifies other statement/batch state. | T-FM14/10 execute/transaction while unread rows remain; measure drain latency/cancellation. Compare portal closure and stream draining as distinct behaviors. |
| FM-S04 | L1: MoreResults can receive another streamed result from the socket and return SQL_ERROR on receive failure. | T-FM15 ordered multiple results, partial fetch, errors in later result, repeat MoreResults then cancel. No assumption that portal path supports multi-statement batching. |

## Documentation conflict and pending decisions

**D:** AWS's [option page](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html),
read 2026-10-02, documents streaming default 0, forward-only positive batches,
CSC priority and says streaming replaces the old UseDeclareFetch/Fetch mechanism.
It describes streaming without transaction wrapping or repeated server fetch
round trips. The pinned [v2.2.3 changelog](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/CHANGELOG.md#L11)
adds UDF/UseDeclareFetch default off. **S:** the pin contains both controls and
named extended-protocol portal execution; UDF takes priority. Windows setup
streaming seed differs from the documented/constructor default (FM-C10).

**R:** Freeze a version-specific effective-mode table after binary/platform
verification. Keep streaming, portal batching and materialization independently
observable. Discuss any omission/change to streaming or declare/fetch with the
user; no approved exception exists. Avoid describing the portal implementation
as literal SQL DECLARE/FETCH statements: reviewed code uses protocol Parse/Bind/
Execute/Close and transaction SQL, while the product option is named declare/fetch.
Simba comparison requires a separately pinned binary/reference.

Pending decision records: malformed/overflow parsing and hard maximum batches;
setup-created versus omitted default; prepared/parameterized bounded delivery;
multiple-statement/handle behavior; refcursor delivery; close/commit failure
reporting and retirement; cancel during refill; transaction ownership and cleanup.
Potential upstream unsafe behavior or suspected bugs require specification and
security evidence plus a user decision, never silent reproduction or exclusion.

## Upstream test leads and finite differential package

Selected assertions were read in
[default_value_test.cpp](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/unittest/default_value_test.cpp):
constructor/UDF defaults, aliases/case, Fetch positive/0/negative, exclusivity,
key-order independence and unchanged FetchRefCursor. Selected
[portal_management_test.cpp](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/unittest/portal_management_test.cpp)
assertions cover portal API null/bad-connection guards, suspended status and
eligibility for CTE, INTO, quotes/comments, dollar/escape strings, multi-statements
and malformed lexical endings. These are helper tests, not Redshift lifecycle
qualification. [sqlcancel_unit_test.cpp](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/unittest/sqlcancel_unit_test.cpp)
uses fake/null connection states; [rsresult_test.cpp](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/unittest/rsresult_test.cpp)
samples replacement/lifetime handling. No upstream tests were run. The latter
mentions `test_streaming_cursor_mid_result_server_error`; targeted source search
found only that comment, so no runnable fixture/evidence is assumed.

| Proposed fixture | Finite input package / evidence |
|---|---|
| T-FM01–07 / settings | Canonical/alias/default/invalid vectors above; direct parse, SQLConnect/DriverConnect and setup-created DSN; use non-secret synthetic settings. Record effective mode and precise diagnostics, not parser acceptance alone. |
| T-FM08–09 / routing | One test per eligibility gate and a bounded lexical corpus from upstream leads. Equivalent direct/prepared/bound query; compare actual results/side effects and selected delivery mode. |
| T-FM10 / transactions | Idle ON/OFF and existing-transaction ON/OFF, explicit commit/rollback/switch while partially fetched; isolated temporary fixtures. Record caller versus driver transaction ownership and cleanup effects. |
| T-FM11 / hostile/failure | First-batch and refill phase failures, missing/wrong result/status, up to five contradictory empty suspensions, delayed server error, mid-frame EOF, timeout and close/commit failure. Offline fault harness first; real server subset later. |
| T-FM12 / data | Fixed batch N=3 plus default 100; 0,1,N-1,N,N+1,2N,2N+1 rows; scalar/NULL/Unicode/binary/long chunk cells, stable schema/order, row arrays and MaxRows. Compare complete indicators/lengths and peak retained bytes. |
| T-FM13–14 / lifetime | Early/exhausted close, FreeStmt CLOSE/DROP, reprepare/re-execute/disconnect, and A/B statement overlap. Verify resources and recovery rather than relying on successful close return. |
| T-FM15–16 / counts/cancel | RowCount/MoreResults across partial/exhausted/error states; ODBC2/3 idle cancel, first-response and refill cancel, repeated cancel and subsequent operation. Pin DM mappings and widths. |

Run reference and our packaged driver with equivalent verified TLS/security
settings against the same Redshift fixtures. Record version/hash, platform/DM/
SQLWCHAR width, server/deployment/capability/token presence, effective non-secret
settings, complete ordered diagnostics and observed transaction/result lifecycle.
Fail live gates on absent/incorrect endpoints; PostgreSQL simulation cannot close
these rows. Driver-specific messages may differ only with an explained decision;
SQLSTATE/order/lengths must not be normalized away. Proposed IDs are not an
implemented test suite, an estimate or an accepted compatibility profile.

## Coverage limit and handoff

This pass traced only the locations above. Unreviewed: complete lower libpq
portal framing/result-resume/TLS/cancel/timeout internals; full eligibility parser
helpers; CSC/thread/file lifecycle; allocator/heap ceilings; attribute-changing
cursor interactions; complete connection prompt/AuthProfile/INI/registry precedence;
all source test assertions; packaged binary behavior. No full RS1–RS8 expansion.

Live unknowns: server portal survival and transaction behavior; capability and
mode applicability for provisioned/Serverless; memory under large cells/results;
mid-stream errors, cancellation/deadlines and synchronization; actual cleanup
resources/commit semantics; handle overlap and required BI application workflows;
platform/default/DM behavior. Rows stay open until exact evidence exists.

[SDK_ARCHITECTURE.md](SDK_ARCHITECTURE.md) and [SECURITY_MODEL.md](SECURITY_MODEL.md)
remain constraints: session-owned delivery/recovery, normalized owning results,
bounded bytes/time, no shared ODBC backend branches and retirement when
synchronization/reset is uncertain. Source cleanup/flag changes cannot authorize
SDK reuse or remove hard ceilings. Any future shared change retains PostgreSQL,
common contracts and platform/security regression gates.

RP-INV-02 stops at this research checkpoint. Suggested next decision is to review
FM-C10, FM-E01 and FM-E10–18 and freeze the finite fetch profile/test subset at M2;
implementation and live access remain separate work. This document changes no
shared plan, runtime, active MySQL scope, release gate or approved feature promise.
