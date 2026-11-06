# PR #172: Redshift scalar capability evidence

The 17 scalar capability flags below are **newly advertised** by PR #172;
their translations already existed. The other proposed flag, CURDATE, is
withheld because no executed connected receipt was located. Its translation
remains available and its offline golden stays tested.

This is bounded reuse of historical execution evidence for unchanged scalar
semantics, **not** a current-head live PASS, complete live qualification, or
Windows/Linux qualification. No new cloud operation was performed. Other
unproved Redshift SQL92/join/aggregate/literal/batch claims remain zero (#142),
and conversion/TIMESTAMPADD/TIMESTAMPDIFF claims remain zero (#220).

## Source and image binding

The retained run is `dm-live-macos-7d678b9-a`, source
`7d678b9f805dc1d861624ee0fd532c1d4296ed9f`. Its plan identifies that source and
pins index SHA-256
`0fcfccc6e247d7061c68546e09341d995a1980376046878da54b3a36e8c20377`.
The pinned index and staged index agree on both core image digests. The
[sanitized receipt](pr172-redshift-scalars.json) contains those image hashes,
all 16 selected case names, completed/run status, zero failure/skip results,
and hashes of the original GoogleTest XML. It excludes connection inputs.
The original evidence is retained under the run name in the private
`redshift-pilot` evidence directory; the plan is retained under
`continuation/dm-live-macos-20261025b`.

All selected receipts are **core/direct-driver** cases. The tests actually
execute SQL, fetch and check the returned values; a translation-only pass is
not used as connected proof. In particular, IFNULL uses core C0212: a Driver
Manager variant may return early after a failed prepare, so its nominal PASS
would not prove IFNULL execution.

## Per-function mapping

| Newly advertised functions | Executed core receipts | Behavioral assertions |
| --- | --- | --- |
| UCASE, LCASE | C0206 `NativeSqlIntegrationTest.ExecutesEscapesInDirectAndPreparedSql` | Direct UCASE fetches `MIXED`; prepared LCASE fetches `prepared`. |
| IFNULL | C0212 `NativeSqlIntegrationTest.BackendTranslationErrorsPreservePreparedSqlAndRecover` | The retained prepared IFNULL executes after each refused replacement and fetches integer 42. |
| NOW, CURTIME | C0466–C0467 `RedshiftTemporalEscapeRealTest` | Direct A/W and prepared table projections match native GETDATE and TIME values, with rebinding/recovery. |
| LOCATE (two arguments only) | C0468–C0469 `RedshiftLocateEscapeRealTest` | Table searches, character positions, NULL/empty values, parameter order and recovery. |
| LENGTH | C0470–C0471 `RedshiftLengthEscapeRealTest` | Padded/Unicode/empty/NULL lengths and prepared rebind/recovery. |
| YEAR, MONTH, DAYOFMONTH | C0472–C0473 `RedshiftCalendarEscapeRealTest` | Date/timestamp fields, leap boundaries, NULL and prepared rebind/recovery. |
| HOUR, MINUTE, SECOND | C0474–C0475 `RedshiftClockEscapeRealTest` | Time/timestamp fields, fractional boundaries, NULL and prepared rebind/recovery. |
| DAYOFWEEK, DAYOFYEAR | C0477–C0478 `RedshiftDayOrdinalEscapeRealTest` | Date/timestamp ordinals, NULL and prepared rebind/recovery. |
| LOG, LOG10 | C0479–C0480 `RedshiftLogFloat8EscapeRealTest` | FLOAT8 natural/base-ten values, NULL/small values and prepared rebind/recovery. |

## Source applicability and limits

Compared historical source to the candidate containing test commit `1c53514`:

- The complete fixture/test region from `class RedshiftTemporalEscapeRealTest`
  to (excluding) `class RedshiftLoginTimeoutOptionsRealTest` is byte-identical.
  This includes the helpers, fixtures and tests for all seven Redshift suites
  above. Its SHA-256 is in the sanitized receipt.
- Both named `NativeSqlIntegrationTest` test bodies are byte-identical; their
  individual SHA-256 values are in the receipt.
- `core/database/postgres/pg_sql_dialect.cpp` has 220 inserted lines and no
  removed/modified old lines. The additions are a vector include, new
  CONVERT/TIMESTAMPADD/TIMESTAMPDIFF helpers, and a dispatch selecting exactly
  those three names. Every existing scalar branch and helper is unchanged;
  the 17 names above do not enter the new dispatch. The receipt includes the
  old/new full-file hashes, allowing reviewers to verify that comparison.
- PR #172 changes declarations and SQLGetInfo adaptation, not these scalar
  translations or their execution paths. Exact-mask tests exercise the new
  metadata, and current offline translation goldens exercise every listed
  scalar plus unadvertised CURDATE. Other changes to the wider driver since
  the historical run are **not** being presented as live-qualified here.

Before declaring a new complete checkpoint green, run its complete applicable
live inventory on its own source and profiles. #142 remains open for that
qualification and for CURDATE/withheld feature groups; this evidence does not
close it.
