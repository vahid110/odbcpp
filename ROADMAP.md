# ODBCPP roadmap: shared foundation → Redshift → driver SDK

Replanned 2026-09-23 against `4f6de2a`. This replaces the old milestone dates,
completion percentages, and open-ended conversion checklist. Historical plans
remain in Git history. The detailed [conformance audit](ODBC_CONFORMANCE_AUDIT.md)
remains evidence, not the release stopping rule.

## Product goals

1. A dependable PostgreSQL implementation to validate shared ODBC and transport behavior.
2. A usable Redshift driver, adapted after the PostgreSQL beta checkpoint.
3. A reusable C++20 driver framework, with database-specific behavior outside the shared ODBC core.

Active implementation scope is PostgreSQL and Redshift only. RDS, Aurora and
Athena are not added to this plan. SDK architecture is a required part of
PostgreSQL consolidation (G9a), proven through Redshift (G9b). Only separate SDK
packaging and public stability guarantees are deferred (G12). See the
[backend boundary and acceptance criteria](BACKEND_BOUNDARY.md).

Finish a coherent, scoped PostgreSQL beta before shifting implementation to
Redshift. This does not require exhaustive PostgreSQL conformance. Work must
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
| M2 | Real Redshift pilot and compatibility assessment | 3–5 days | 25–41 days | G1 closed on a real Redshift endpoint |
| M3 | Usable Redshift beta | 8–12 days | 36–56 days | G6–G8 and G9b closed; PostgreSQL regression gates remain green |
| M4 | Scoped production release candidate | 8–12 days | 46–72 days | G10–G11 closed; both scoped beta baselines remain green |

The [M0 working inventory](PG_BETA_CHECKLIST.md) sizes architecture at 9–15
days within M1, revising M1 to 14–23 days. These estimates remain provisional,
not a measured forecast.
M0 must enumerate and estimate the finite PostgreSQL consolidation checklist
before committing to a date. M1 includes application and basic packaging work
that the earlier foundation-only estimate omitted. M0 must explicitly estimate
A1–A4/G9a and revise the M1 range if needed; architecture effort must not be
treated as free work or hidden in contingency. The sequence is sequential;
external waiting is excluded. Re-estimate at PG-BETA and after the Redshift pilot.
Redshift access preparation and documentation review may proceed earlier, but
must not displace PostgreSQL consolidation. A separate SDK release and unrelated
database backends have no delivery commitment in this plan.

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

## Next three implementation batches

1. **PostgreSQL consistency inventory (G0):** classify all partial audit rows,
   identify required workflows, inconsistencies and A1–A4 backend coupling, and estimate the finite
   PG-BETA checklist. Reuse existing evidence; avoid new permutations without
   an identified contract gap.
2. **Close PostgreSQL beta and architecture blockers (G1–G5/G8/G9a):** implement
   the bounded backend extractions alongside correctness work; prioritize incorrect results,
   state/diagnostic inconsistencies, truthful capabilities and required metadata.
   Missing database access must fail release validation rather than skip tests.
3. **Package and demonstrate PG-BETA (G8/basic G11):** clean installation,
   examples, application workflow, limitations, and a recorded beta checkpoint.
   Redshift implementation starts after this checkpoint passes.

## Execution and stop rules

Every batch names a gate ID and expected acceptance evidence. Retain focused
checks during development and the full relevant gates before an implementation
push. Do not rerun database gates for prose-only changes. Record new discoveries
in the [decision/backlog table](RELEASE_PLAN.md#deferred-work-and-revisit-triggers).

PostgreSQL consolidation ends at PG-BETA, including G9a. Redshift beta ends when
G0–G8 plus G9a/G9b pass for its frozen profile and no known blocker remains. Release-candidate work
ends when G10–G11 also pass. G12 is a deferred SDK acceptance definition.
A remaining `Partial` audit row is acceptable only if its residual behavior is
explicitly outside the release profile and safely handled. No serious defect
in a supported path may be deferred to meet a date.

The scheduler remains paused. This plan does not resume it or authorize cloud
resource creation. On an explicit resume, its saved instructions should be
updated to select work by these gates and stop at the agreed milestone.
