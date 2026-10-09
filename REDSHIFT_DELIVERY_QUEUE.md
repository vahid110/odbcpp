# Redshift delivery queue

Updated 2026-10-09. This executes the existing order in
[REDSHIFT_FEATURE_PLAN.md](REDSHIFT_FEATURE_PLAN.md); it does not expand cloud
authority, change acceptance gates or resume AUTH/MySQL.

## Current and next deliveries

| Order | Product delivery | Owner / state | Acceptance and remaining work |
|---|---|---|---|
| A1 | Consistent exact-decimal prepared-parameter metadata through SQLBindParameter, SQLDescribeParam and SQLExecute | Dedicated API worker; implementation assigned at 30f151a | Reproduce DECIMAL versus NUMERIC binding discrepancy first. Preserve caller-bound precision/scale across that closed alias pair without inventing server dimensions or changing native type identity. Three scenarios: equivalent alias bindings; exclusion of unrelated types/unknown unbound dimensions; NULL/error/rebind recovery. Production patch, focused checks, independent review, applicable shared regression and exact-head CI. |
| A2 | Close remaining common-type blockers to the prepared import workflow | API worker; next selection, not an implementation-ready package | Use the existing integer/decimal/text/temporal/binary evidence to select a concrete missing input/result behavior that prevents the workflow. Publish the exact API, observed defect, owned files and three related scenarios while A1 is being qualified. If no blocker is established, explicitly record that result; no speculative boundary-test batch. |
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

The API worker stays on product selection and implementation. Root and existing
reviewers own packaging, qualification, integration and CI. A qualification wait
does not replace ready API work. Every development turn must record actual lane
states and either dispatch the ready successor or state its concrete dependency.

Before starting a production package, name the observable behavior, source-backed
gap, selected APIs/files, related scenarios and completion condition. Select the
successor during current review/builds; do not turn a list of possible tests into
an unbounded queue. A2 is presently a selection task, not a promised feature fix.

Tests accompany the product change. Run focused checks during development and
the required shared graph once per coherent batch, with bounded repairs for
actual failures. Independent cleanup review and exact-head CI remain mandatory.
No passed gate is repeated without new source or a concrete execution gap.

Report delivery as distinct stages: implemented, locally qualified, CI accepted,
native qualified and application accepted. Commit count, test count and worker
activity alone do not establish usable feature delivery.
