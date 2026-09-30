# ODBCPP roadmap: shared foundation → connectivity SDK proof → Redshift

Replanned 2026-09-30 after the PostgreSQL Windows delivery and Redshift pilot
preparation. This replaces the old milestone dates,
completion percentages, and open-ended conversion checklist. Historical plans
remain in Git history. The detailed [conformance audit](ODBC_CONFORMANCE_AUDIT.md)
remains evidence, not the release stopping rule.

## Product goals

1. A dependable PostgreSQL implementation to validate shared ODBC and transport behavior.
2. A usable Redshift driver, adapted after the PostgreSQL beta checkpoint.
3. A reusable C++20 connectivity SDK, with database-specific behavior outside
   client API adapters and a path to both ODBC and ADBC.
4. A bounded MySQL 8 reference slice proving that the SDK supports an unrelated
   wire protocol before public SDK stability is promised.

Active implementation scope is PostgreSQL, Redshift and the bounded MySQL SDK
proof in [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md). RDS, Aurora, Athena, MariaDB
and SQL Server/TDS are not added. SDK architecture began in PostgreSQL
consolidation (G9a), is tested across unrelated protocols by MySQL (G12), and is
refined through Redshift specialization (G9b). ADBC shapes the internal boundary
but its API, Arrow integration and packaging are deferred. Public SDK stability
and distribution remain G13. See the
[backend boundary and acceptance criteria](BACKEND_BOUNDARY.md).

The PostgreSQL technical and Windows delivery baseline may support SDK work while
host-dependent G8 application acceptance is deferred by explicit product
decision. This does not close PG-BETA or waive its evidence. Work must
close a release gate, fix a material defect, or establish a reusable boundary
needed by these products. Extra test permutations alone do not justify a batch.

## Current position

The PostgreSQL foundation works and has substantial hardening evidence. At the
baseline, 76 exported symbols cover 49 operations: 11 are fully verified under
the audit's strict rules and 38 remain partial. These are not effort percentages.
All five latest CI jobs passed; the local suite has 34 executables (24 unit,
10 integration). This does not establish application or Redshift compatibility.

Redshift is a build target using the PostgreSQL parser, but current CI uses
PostgreSQL 17. Windows CI runs unit tests, not live database integration.
The SDK has useful interfaces and install exports, but ODBC code still directly
constructs PostgreSQL components and contains PostgreSQL catalog/type logic.

See [the assessment and release gates](RELEASE_PLAN.md) for evidence, scope,
acceptance criteria, investigation budgets, and deferred work.

## Milestones and working estimates

Estimates are engineering working days for one focused implementation stream,
not scheduler wakeups or promised calendar dates. They exclude waiting for
credentials, infrastructure, product decisions, and external application access.
Ranges are provisional, with low confidence until M1 completes. A 30% contingency
is included in the cumulative ranges, rounded upward. It funds discoveries,
not additional features. Re-estimate after the real Redshift pilot.

| Order | Milestone | Base effort | Buffered cumulative target | Exit |
|---|---|---:|---:|---|
| M0 | Freeze the supported release profile and evidence inventory | 2–3 days | 3–4 days | G0 closed; target application/auth/platforms named; A1–A4 inventoried and estimated |
| M1 | PostgreSQL consolidation and usable beta checkpoint | 14–23 days | 21–34 days | PG-BETA below closed; G9a architecture, coherent behavior, application demonstration and installable artifact |
| MS1 | SDK foundation and bounded MySQL proof | 20–32 days | 26–42 days for MS1 | G12 closed; two real sibling backends use the shared contract; no MySQL beta or public ABI claim |
| M2 | Real Redshift pilot and compatibility assessment | 3–5 days | Prior cumulative target resumes when access exists | G1 closed on a real Redshift endpoint |
| M3 | Usable Redshift beta | 8–12 days | Re-estimate after M2 | G6–G8 and G9b closed; PostgreSQL regression gates remain green |
| M4 | Scoped production release candidate | 8–12 days | Re-estimate after M2 | G10–G11 closed; both scoped beta baselines remain green |

The [M0 working inventory](PG_BETA_CHECKLIST.md) sizes architecture at 9–15
days within M1, revising M1 to 14–23 days. These estimates remain provisional,
not a measured forecast.
M0 must enumerate and estimate the finite PostgreSQL consolidation checklist
before committing to a date. M1 includes application and basic packaging work
that the earlier foundation-only estimate omitted. M0 must explicitly estimate
A1–A4/G9a and revise the M1 range if needed; architecture effort must not be
treated as free work or hidden in contingency. The sequence is sequential;
external waiting is excluded. Re-estimate at PG-BETA and after the Redshift pilot.
The live Redshift pilot is currently waiting on restored user account access.
MS1 is approved bounded work during that external wait. It does not replace M2
or authorize a full MySQL driver. Its package estimates, gates and stop rules are
in [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md).

### PG-BETA: mandatory handoff checkpoint

- G0 frozen for PostgreSQL; G1 evidence integrity and G2–G5 pass for PostgreSQL.
- G9a passes: backend creation, dialect, native types/catalogs and capabilities
  are behind an exercised boundary; ownership, errors and deadlines are defined.
- Each promised query, parameter, result, metadata, transaction and diagnostic
  workflow is consistent; unfinished optional behavior is explicitly unsupported.
- G8 application workflow passes on PostgreSQL, including required metadata.
- Basic G11 delivery subset passes: install/configure/uninstall, runnable examples,
  TLS/auth instructions, supported features and limitations, and a versioned beta
  artifact. Full production delivery and soak gates remain M4 work.
- No known serious supported-path defects. Record the demonstration, evidence,
  accepted limitations and beta baseline before starting Redshift implementation.

Application scope update (2026-09-26): SQL Server linked server/OPENQUERY,
Power BI Desktop on Windows, and Excel on macOS are requested acceptance targets.
See the [application matrix](RELEASE_PLAN.md#application-acceptance-targets--added-2026-09-26).
The effort table is the prior one-application baseline; M1 and cumulative dates
require re-estimation for these three tracks. Windows live validation is now
required. No extra application scope is charged to contingency.

## Next implementation sequence

1. **S1 contract freeze:** inventory dependency direction and publish the SDK
   ownership, lifecycle, pooling/reuse, cache, result and extension contracts
   without behavior changes.
2. **S2 shared proof harness:** make only the extractions required by the audit;
   keep PostgreSQL, iODBC, Windows and sanitizer gates green.
3. **S3 MySQL vertical slice:** implement and live-test the bounded MySQL 8
   workflow through shared ODBC orchestration.
4. **S4 SDK usability proof:** publish the backend test kit, extension guide and
   minimal out-of-tree sample; stop and review G12.
5. **M2 Redshift pilot:** run as soon as valid endpoint access is available.
   PostgreSQL G8 application evidence remains open and recorded meanwhile.

## Execution and stop rules

Every batch names a gate ID and expected acceptance evidence. Retain focused
checks during development and the full relevant gates before an implementation
push. Do not rerun database gates for prose-only changes. Record new discoveries
in the [decision/backlog table](RELEASE_PLAN.md#deferred-work-and-revisit-triggers).

PostgreSQL consolidation ends at PG-BETA, including G9a. The explicit 2026-09-30
reprioritization permits MS1 before host-dependent G8 evidence is available;
PG-BETA remains open. Redshift beta ends when
G0–G8 plus G9a/G9b pass for its frozen profile and no known blocker remains. Release-candidate work
ends when G10–G11 also pass. G12 is the bounded internal SDK/MySQL proof; G13 is
the deferred public SDK preview.
A remaining `Partial` audit row is acceptable only if its residual behavior is
explicitly outside the release profile and safely handled. No serious defect
in a supported path may be deferred to meet a date.

The scheduler remains paused. This plan does not resume it or authorize cloud
resource creation. On an explicit resume, its saved instructions should be
updated to select work by these gates and stop at the agreed milestone.

### Windows delivery scope clarification — 2026-09-29

PG-BETA now explicitly requires W1–W4 from RELEASE_PLAN.md: native Windows DSNs,
connection-string interoperability, a minimal ODBC Administrator setup GUI and
x64 installer/uninstaller. Estimate 4.5–8 base engineering days plus separate 30%
contingency, replacing overlapping Windows delivery allowance. The earlier M1
range remains historical, not a revised completion forecast. Complete A4/G9a
first; then W1/W2 before Windows applications, and W3/W4 before packaged G8
acceptance. Redshift remains after PG-BETA. Broader installer polish and
interactive connection prompts remain conditional, not part of these packages.


### Architecture checkpoint — batch 12

A1–A4 implementation and fake-backend acceptance are complete; the dependency
review is recorded in BACKEND_BOUNDARY.md. Close G9a upon this batch's green full
local gates and exact-revision CI. Next is Windows W1–W4, followed by G8 real
applications. The application-host reminder is not due while Windows delivery
work is still next. That work is now complete; G8 remains open and host-dependent.
The 2026-09-30 reprioritization permits bounded MS1 work during the Redshift
access wait. Public SDK packaging remains G13.
