# Conversion and timestamp escapes

ODBCPP translates these escapes for its PostgreSQL and Redshift products in
direct statements, prepared statements and `SQLNativeSql`. `SQL_ATTR_NOSCAN`
continues to bypass escape translation. Native SQL type checking, range errors
and NULL propagation remain server behavior.

> **Redshift capability reporting.** The Redshift product translates these
> escapes, but reports none of them in `SQLGetInfo` (`SQL_CONVERT_FUNCTIONS`,
> the `SQL_CONVERT_*` pairs, `SQL_TIMEDATE_ADD_INTERVALS`,
> `SQL_TIMEDATE_DIFF_INTERVALS`, and the TIMESTAMPADD/TIMESTAMPDIFF bits of
> `SQL_TIMEDATE_FUNCTIONS`) until the generated SQL passes a Redshift live
> acceptance (#144). Applications that check capabilities first therefore
> do not emit them on Redshift yet.

## CONVERT

`{fn CONVERT(expression, SQL_type)}` uses a native cast with the following
defaults. The second argument must be one type keyword, not a length, precision,
expression or parameter marker.

| ODBC target | PostgreSQL target | Redshift target |
| --- | --- | --- |
| SMALLINT / INTEGER / BIGINT | corresponding integer type | corresponding integer type |
| REAL | REAL | REAL |
| FLOAT / DOUBLE | DOUBLE PRECISION | DOUBLE PRECISION |
| NUMERIC / DECIMAL | unconstrained NUMERIC | DECIMAL(38,18) |
| BIT | BOOLEAN | BOOLEAN |
| DATE / TIME / TIMESTAMP | corresponding temporal type | corresponding temporal type |
| CHAR / VARCHAR / LONGVARCHAR, including W variants | TEXT | VARCHAR(65535) |
| BINARY / VARBINARY / LONGVARBINARY | BYTEA | VARBYTE |
| GUID | UUID | refused |
| TINYINT and all interval targets | refused | refused |

Every target in the table uses its `SQL_` prefix in an escape. Character
conversion uses a varying Unicode-capable native representation; it does not
promise fixed-width padding. Native result metadata is retained. This follows
the latitude illustrated by Microsoft's CHAR-to-Oracle-TO_CHAR example, whose
native result is VARCHAR2. The Redshift 65,535-byte bound is this driver's cast
policy, not a claim that every Redshift string expression has that limit.
Native explicit-cast truncation rules apply. Redshift decimal conversion allows
20 integral and 18 fractional digits; extra fractional digits can round and an
out-of-range integral part can fail. Use native CAST when a different precision
or character length is required.

`SQL_CONVERT_*` reports source-to-target pairs, not all target spellings for
every source. Numeric sources expose numeric and character targets; character
sources expose numeric, character, binary and temporal targets, plus BOOLEAN
and UUID on PostgreSQL. Binary sources expose binary and character targets. Boolean exposes BOOLEAN
and, on PostgreSQL only, character targets. Redshift BOOLEAN-to-character
conversion is not advertised. DATE exposes DATE, TIMESTAMP and characters; TIME exposes TIME
and characters; TIMESTAMP exposes all three temporal targets and characters.
PostgreSQL UUID exposes UUID and characters. Other pairs remain unadvertised.
Valid conversion support does not mean every input value is representable.

**Character and binary conversions differ by product.** On PostgreSQL, a
character value converted to a binary target is parsed as `bytea` input
syntax: `\x41` becomes one byte, and a lone backslash is an error. A binary
value converted to a character target is rendered in the session's
`bytea_output` format, hexadecimal `\x...` by default. On Redshift, VARBYTE
to and from VARCHAR follows Redshift's own casts. Do not rely on
byte-for-byte round trips between the products.

## TIMESTAMPADD and TIMESTAMPDIFF

`{fn TIMESTAMPADD(SQL_TSI_unit, count, value)}` supports SECOND, MINUTE, HOUR,
DAY, WEEK, MONTH, QUARTER and YEAR. PostgreSQL uses calendar interval arithmetic;
Redshift uses DATEADD with an INTEGER count. Backend range errors remain errors.
Native month/year clipping differs: adding one year to 2024-02-29 produces
2025-02-28 on PostgreSQL and 2025-03-01 on Redshift; adding twelve months produces
2025-02-28 on both. This product difference is covered by explicit expectations.

`{fn TIMESTAMPDIFF(SQL_TSI_unit, start, end)}` supports the same units except
WEEK. ODBCPP chooses signed **calendar-boundary counts**, matching Redshift
DATEDIFF: crossing midnight by a microsecond is one DAY, and reversing the
operands gives -1. PostgreSQL uses endpoint bucket ordinals rather than rounding
elapsed durations. Calendar units truncate both endpoints to their bucket starts
before using AGE, so only whole calendar months enter the ordinal calculation. This is an explicit ODBCPP contract;
Microsoft's function table does not prescribe a universal rounding rule.

FRAC_SECOND is refused for both functions: ODBC defines it in nanoseconds,
whereas these backends have microsecond temporal resolution. WEEK differences
are refused until a common week-origin contract is qualified. These units have
no capability bits. Unknown units are also refused, rather than passed through.

Typed TIME operands acquire the current server date; DATE operands acquire
midnight. Character TIME input must use colon-separated `HH:MM[:SS[.fraction]]`
notation; native alternative spellings are not a portable contract. Conversion
uses local wall-clock TIMESTAMP fields, not timezone-aware elapsed arithmetic.
The raw input expression appears once, preserving parameter-marker association.
PostgreSQL month/quarter/year ordinals scale the whole-month interval relative
to a fixed epoch, retaining expression scope for window and aggregate operands
and single evaluation of volatile operands. SQL itself does not promise left-to-right
evaluation order.

`SQL_TIMEDATE_ADD_INTERVALS`, `SQL_TIMEDATE_DIFF_INTERVALS` and the corresponding
`SQL_TIMEDATE_FUNCTIONS` bits expose exactly this interval subset. Broader scalar
and SQL-92 capability reporting is tracked in issue #142.

## References and qualification

- [Microsoft CONVERT specification](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/explicit-data-type-conversion-function)
- [Microsoft time/date functions](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/time-date-and-interval-functions)
- [Oracle TO_CHAR return type](https://docs.oracle.com/en/database/oracle/oracle-database/21/sqlrf/TO_CHAR-number.html)
- [PostgreSQL datetime arithmetic](https://www.postgresql.org/docs/current/functions-datetime.html)
- [PostgreSQL interval implementation](https://github.com/postgres/postgres/blob/REL_17_STABLE/src/backend/utils/adt/timestamp.c): `interval_mul` scales stored months; `interval2itm` divides by twelve for the extracted year field. Truncated calendar-boundary AGE intervals contain whole months, so factors twelve, four and one yield month, quarter and year ordinals.
- [Redshift DATEADD](https://docs.aws.amazon.com/redshift/latest/dg/r_DATEADD_function.html)
- [Redshift DATEDIFF](https://docs.aws.amazon.com/redshift/latest/dg/r_DATEDIFF_function.html)
- [Redshift character limits](https://docs.aws.amazon.com/redshift/latest/dg/r_Character_types.html)
- [Redshift numeric types](https://docs.aws.amazon.com/redshift/latest/dg/r_Numeric_types201.html)

Checked-in translation, public capability and connected SQL tests cover these
contracts. Their presence is not a live acceptance claim: the PR records actual
results separately for each product, platform and driver-manager profile.

## Capability reporting and provider migration

`SQL_BATCH_SUPPORT` and `SQL_BATCH_ROW_COUNT` are zero for both products.
A direct, unparameterized `SQLExecDirect` simple-query sequence is narrower
than ODBC explicit-batch support: `SQLPrepare`/`SQLExecute` and parameterized
execution use one protocol Parse, which cannot contain multiple commands.
Prepared and parameterized semicolon batches remain unsupported; use separate
statements. No automatic splitting, interpolation or fallback is implemented.

`SQL_MULT_RESULT_SETS` describes available result sequences, not support for
all batch execution paths. PostgreSQL's direct result sequences and the
existing prepared-select parameter-array facet independently justify `Y`.
A provider that genuinely qualifies full SELECT batches also implies `Y`.
A provider with neither reports `N`; the Redshift SQL declaration makes no
new direct-sequence claim, while its existing prepared-array facet still
supplies multiple results. The checked-in direct-sequence and prepared-array
unit tests exercise `SQLMoreResults` separately from the zero batch masks.
See the [ODBC SQLGetInfo definitions](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetinfo-function)
and [PostgreSQL extended-query restriction](https://www.postgresql.org/docs/current/protocol-flow.html#PROTOCOL-FLOW-EXT-QUERY).

Redshift's newly proposed SQL92, subquery, aggregate, datetime-literal and
outer/relational-join masks remain zero pending connected qualification under
#142. Existing scalar escape flags remain available. The connected tests retain
their SQL behavior assertions, explicitly expect withheld Redshift mask bits,
and can collect future qualification evidence without advertising those bits
prematurely. Conversion and timestamp-add/diff claims remain withheld under
#220. A successful direct-only batch probe cannot qualify the broader batch
masks; prepared and parameterized support needs separate implementation/proof.

**Public SDK source compatibility:** `BackendCapabilities::outer_joins` has
been replaced by the detailed `BackendCapabilities::sql.join_*` declarations.
Out-of-tree providers that assigned the old member must migrate and recompile;
this is a source-breaking change, not an automatic compatibility alias. Declare
only qualified forms; do not mechanically set every new field to true.
`sql.direct_result_sequences` is separate from `sql.batch_select`,
`sql.batch_command` and `sql.batch_individual_counts`: the latter require the
full direct/prepared/parameterized batch contract. All fields default false.
