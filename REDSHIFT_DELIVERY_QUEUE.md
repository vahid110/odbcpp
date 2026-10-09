# Redshift delivery queue

Updated 2026-10-09. This executes the existing order in
[REDSHIFT_FEATURE_PLAN.md](REDSHIFT_FEATURE_PLAN.md); it does not expand cloud
authority, change acceptance gates or resume AUTH/MySQL.

## Current and next deliveries

| Order | Product delivery | Owner / state | Acceptance and remaining work |
|---|---|---|---|
| A1 | Exact-decimal prepared-parameter metadata aliases | API worker delivered; locally and CI accepted at b62c7f3 | DECIMAL and NUMERIC retain caller-bound precision/scale across the closed alias pair. Three checked-in scenarios cover equivalent bindings, unrelated/unknown dimensions and NULL/refusal/rebind recovery. Native and application qualification remain separate. |
| A2 | BIT input respects declared exact-decimal precision | API worker delivered; implementation/local/CI accepted at bb16fe0 | Canonical BIT inputs use the existing whole-digit guard; zero and NULL retain their semantics, overflow refuses before dispatch and explicit rebinding recovers. Exact-head CI37891943740 is accepted; native/application qualification remains separate. |
| A3 | Failed transaction completion reconciles ownership | API worker delivered; implementation/local/CI accepted at bb16fe0 | SQLEndTran follows the authoritative Idle/Transaction/FailedTransaction snapshot, retains explicit rollback duty and terminal retirement, and avoids stale ownership or implicit replay. Exact-head CI37891943740 is accepted; native/application qualification remains separate. |
| A4 | Failed END diagnostics use approved provider policy safely | API worker delivered; repaired implementation/local/CI accepted at 5c13df4 | Reuse existing validated SQLSTATE mapping without widening its allowlist. Reconcile ownership and terminal closure before a diagnostic callback can throw; preserve owning diagnostics after closure. Original rejected candidate and baseline failures remain immutable. Shared regression, final integration review and exact-head CI37895188227 are accepted; native/application qualification remains separate. |
| A5 | BEGIN and connected isolation diagnostics follow that policy | API worker delivered; implementation/local/CI accepted at 5c13df4 | Direct/prepared BEGIN and connected isolation errors retain approved states, local-error precedence, state ownership and failure publication. The reproduced discrepancy, retained assertions and throwing-callback boundaries passed source/focused review and the coherent shared graph; final integration review and exact-head CI37895188227 are accepted. Native/application qualification remains separate. |
| A6 | Bounded Sync-first SQLCancel(STMT) | Implementation/local/CI accepted at bd0691a | Genuine backend-owned cancellation with protected keys, verified TLS sibling, bounded wait/drain, honest carrier support, HY008, precedence, cursor races and recovery. Independent final acceptance and exact-head CI37924834128 are complete. SQLCancelHandle/async and native Redshift/vendor/application qualification remain separate. |
| A7 | Wide-text chunked retrieval | Implementation/local/CI accepted at 65d1f98 | SDK UTF-8 decoding preserves byte indicators, UTF16 pairs/UCS4 units, NULL and continuation/reset behavior. Joined regression, independent final review and exact-head CI37958757662 are accepted; native/application qualification remains separate. |
| A8 | Forward column-wise rowsets | Implementation/local/CI accepted at 65d1f98 | Checked strides, per-row status/diagnostics, partial NOROW and recovery are accepted. Multirow GetData remains HY109; scrolling remains separate. |
| A9 | Release invalidated result storage | Implementation/local/CI accepted at 65d1f98 | Close/final MoreResults release unused outer row allocation while retaining bindings and owning siblings. No streaming or process RSS release is promised. |
| A10 | Compact the accessible MAX_ROWS prefix | Implementation/local/CI accepted at 65d1f98 | Full input validation and staged capped rows preserve unlimited/exact-size zero-copy and queued/prepared/sibling recovery. No server or peak-memory bound is claimed. |
| A11 | Indicator-only column binding | Integrated A11–A15; source/focused and six-profile regression accepted | Bound length/NULL indicators work without a data buffer. Current six-profile, offline, ordinary PostgreSQL and verified-TLS checks pass; independent shutdown/final integration review and exact-head CI remain pending. |
| A12 | Row-wise result binding | Integrated A11–A15; source/focused and six-profile regression accepted | Positive byte strides support padded typed rows, split indicator/length fields, neighbors and descriptor aliases; ordinary/TLS checks pass; independent shutdown/final review and exact-head CI pending. |
| A13 | Row-binding offsets | Integrated A11–A15; source/focused and six-profile regression accepted | Capture one nonnegative offset per fetch, retain original bases, preflight arithmetic before progress and leave status/count pointers unshifted; ordinary/TLS checks pass; independent shutdown/final review and exact-head CI pending. |
| A14 | Retrieve-data suppression | Integrated A11–A15; source/focused and six-profile regression accepted | SQL_ATTR_RETRIEVE_DATA OFF skips deferred bound output access while preserving rowset positioning/status; ON and explicit scalar GetData recover; ordinary/TLS checks pass; independent shutdown/final review and exact-head CI pending. |
| A15 | Single-set parameter-binding offsets | Integrated A11–A15; source/focused and six-profile regression accepted | Capture adjusted owning input values before dispatch, preserve NULL/descriptor/span/refusal/rebind semantics and scalar cancellation scope; ordinary/TLS checks pass; independent shutdown/final review and exact-head CI pending. |
| A16 | Single-set parameter operation masks | Separate sealed source; independent source review accepted, uncompiled | PROCEED/IGNORE selection remains outside A11–A15; joined compilation, runtime, integration and CI are still required. |
| A17 | Opt-in automatic parameter descriptions | Separate sealed source; independent source review accepted, uncompiled | SQL_ATTR_ENABLE_AUTO_IPD remains opt-in and uses existing description/capability/session policy; outside A11–A15. |
| A18 | Catalog metadata identifiers | Separate sealed source002; independent source review accepted, uncompiled | SQL_ATTR_METADATA_ID semantics cover all eight catalog APIs, preserving special SQLTables enumeration and guarded primary-key routing; outside A11–A15. |
| A19 | Column-wise prepared command parameter arrays | Dedicated API worker implementing reviewed contract | SDK owns normalized description authority and same-lease owning sequencing; adapter owns pointer/status policy. No SELECT arrays, native batching or active-array cancellation support is promised. Independent source review, fresh consumer rebuild and qualification remain required. |
| P1 | Current private UCS4 packaged runtime | Root / qualification; preparation complete | Runtime files, relative links, external dependencies and relocated loader resolution independently accepted. This does not qualify Driver Manager or Excel execution. |
| P2 | Current packaged native discovery, typed import and prepared refresh | Root / qualification; live execution blocked | Reuse the three checked-in package-import cases and prior native evidence; bind current source/binary to a fresh finite admission. Exact-decimal/NULL, metadata, parameter rebinding and early-close ownership must work on real Redshift. Human testing horizon expired; no live run or resume until renewed authorization and reviewed controls. |
| P3 | First Excel acceptance on this Mac | Root / qualification; application prerequisite blocked | Installed driver connects with verified TLS, browses metadata, imports representative typed rows and refreshes using application-generated queries. Record trust-source configuration and exact application/Driver Manager/runtime. Requires administrator installation of the missing iODBC frameworks; no SecurityAgent bypass. |
| P4 | Pinned official-driver comparison of that same workflow | Root / qualification; not admitted | Same fixture and equivalent verified TLS with pinned AWS 2.2.2.0 binary. Record type/metadata/query/refresh differences. Reference execution bounds and application prerequisites must be closed first; source research alone is not a binary comparison. |

## Following product milestones

These remain ordered after the usable baseline. They are committed roadmap
deliverables, not claims that implementation packages are ready or APIs absent.

1. **Transactions and recovery:** SQLSetConnectAttr autocommit, SQLEndTran,
   commit/rollback and failure/reconnect behavior. Reuse current implementations;
   repair demonstrated gaps and qualify real Redshift semantics.
2. **Cancellation and timeouts:** SQLCancel/SQLCancelHandle, query timeout,
   safe statement reuse or connection retirement after uncertain completion.
3. **Analytical result scale:** bounded large-result memory, chunked retrieval,
   row fetching and close/refill lifecycle, with measured resource evidence.
4. **Broader parity:** remaining catalog APIs, parameter-array/settings support,
   less common types and advanced fetch modes, through the existing parity
   inventory. Advanced S3 work still requires its separate scope decision.

## Scheduling and acceptance

Use [DEVELOPMENT_WORKFLOW.md](DEVELOPMENT_WORKFLOW.md), approved 2026-10-09.
Primary-profile qualification permits coherent intermediate pushes; full matrix
qualification runs every24h for changed heads and promptly for concrete sensitive
changes. Root tracks exact heads and acceptance debt; only final full graph and
green exact-head full CI close the goal/release. The dedicated API lane continues.
This replaces historical per-feature-commit full-graph sequencing below.

The API worker stays on product selection and implementation. Root and existing
reviewers own packaging, qualification, integration and CI. A qualification wait
does not replace ready API work. Every development turn must record actual lane
states and either dispatch the ready successor or state its concrete dependency.

Before starting a production package, name the observable behavior, source-backed
gap, selected APIs/files, related scenarios and completion condition. Select the
successor during current review/builds; do not turn a list of possible tests into
an unbounded queue. A1–A10 and their required repairs are implementation/local/CI accepted. The
current integration batch is A11–A15, the binding and transfer-settings workflow.
A16–A18 remain separate source-reviewed successors. The dedicated API worker
implements A19 while root owns current qualification and integration. Native
qualification remains distinct and requires renewed authority.
The handoff owns changing worker, gate and CI facts; this queue records the
deliveries and their acceptance requirements. Following milestones remain
separate work and do not become qualified when this batch closes.

Tests accompany the product change. Run focused checks during development and relevant primary regression per
coherent batch; use daily full qualification and immediate risk-based extra checks
under DEVELOPMENT_WORKFLOW.md, with bounded repairs for actual failures. Independent cleanup review and exact-head CI remain mandatory.
No passed gate is repeated without new source or a concrete execution gap.

Report delivery as distinct stages: implemented, locally qualified, CI accepted,
native qualified and application accepted. Commit count, test count and worker
activity alone do not establish usable feature delivery.
