# Redshift conversion coverage audit: numeric, temporal, VARBYTE

Branch `codex/redshift-special-types`, baseline `efe7af379469303415f7ccce3a5d8d626a92a886`,
audited 2026-10-22 against the active code, not earlier audit prose.
Special types (SUPER, GEOMETRY, GEOGRAPHY) are in
[redshift-special-types.md](redshift-special-types.md).

## Evidence classes

| Code | Meaning | Where |
|---|---|---|
| U | Converter/codec unit test (no ODBC handle) | `test_redshift_data_conversion`, `test_native_types`, `test_pg_protocol_parser` |
| W | Literal protocol frames through a real backend session, optionally the ODBC API | `test_connection_liveness`, `test_redshift_special_types` |
| F | ODBC API over a fake backend returning normalized results | `test_backend_contract`, `test_numeric_scale_conversion`, `test_descriptor_apis` |
| PG | Private PostgreSQL server integration (PostgreSQL profile, not Redshift) | `it_bind_col_real`, `it_prepared_statements_real` |
| L | Real Redshift GoogleTest (`it_redshift_real`), each gated by an admission marker | see per-row names |
| — | No checked-in test found | |

A row is "supported" only where the active path implements it; the generic
character fallback is not counted as support. Executed status in this session:
U/W/F only, macOS/iODBC UCS4 Release (see `CANDIDATE.md` in the session status directory). PG and L rows are
inventory; they were not run here.

## Active paths (verified)

* Result families: `PgProtocolParser::describe_type` (`core/database/postgres/pg_type_info.cpp`)
  plus Redshift overrides in `PgDatabaseConnection::describe_type` (`pg_value.cpp`).
  Results are always text format (`append_u16(out, 0)` in `pg_protocol_parser.cpp`).
* Result cells: `GenericDatabaseConnection` normalization (UTF-8 validation for
  character families; backend codec for Binary/LongVarBinary/Boolean), then the
  ODBC adapter (`odbc/odbc_handles.cpp`) and `TextDataConverter::convert_data`
  (`odbc/text_data_converter.cpp`). Allowed pairs: `ResultTypes::is_conversion_supported`.
* Parameters: `parameter_type_for` maps SQL type (then C type) to
  `QueryParameterType`; Redshift sends VARBYTE (6551) and finite floating values
  in binary format, everything else as typed text (`pg_protocol_parser.cpp`
  `postgres_type_oid`, native IEEE path).
* Legacy `odbc/redshift/redshift_types.cpp`/`redshift_data_converter.cpp` are not
  compiled; `test_redshift_data_conversion` aliases the active `TextDataConverter`.

## SQL source -> C result targets

| SQL family (OID) | C targets accepted | Default C | Refused (07006) |
|---|---|---|---|
| SMALLINT/INTEGER/BIGINT (21/23/20), REAL/DOUBLE (700/701), NUMERIC (1700), BOOLEAN (16) | CHAR, WCHAR, all signed/unsigned integers, FLOAT, DOUBLE, BIT, NUMERIC, BINARY | native | DATE/TIME/TIMESTAMP |
| DATE (1082) | CHAR, WCHAR, BINARY, DATE, TIMESTAMP | TYPE_DATE | numerics, BIT, TIME |
| TIME/TIMETZ (1083/1266) | CHAR, WCHAR, BINARY, TIME, TIMESTAMP | TYPE_TIME | numerics, BIT, DATE |
| TIMESTAMP/TIMESTAMPTZ (1114/1184) | CHAR, WCHAR, BINARY, DATE, TIME, TIMESTAMP | TYPE_TIMESTAMP | numerics, BIT |
| VARBYTE (6551) | CHAR, WCHAR (lower-case hex, existing generic conversion), BINARY | BINARY | everything else |
| INTERVAL Y2M/D2S (1188/1190) | **Unsupported family**: unknown VARCHAR text (`known=false`) | CHAR | — |

## C input -> SQL parameter targets

| SQL target | Wire | Notes |
|---|---|---|
| CHAR/VARCHAR/LONGVARCHAR (+W) | OID 25 text | Character length validated in characters; UTF-8 validated. |
| SMALLINT/INTEGER/BIGINT/TINYINT | 21/23/20 text | TINYINT maps to INT4. |
| REAL/FLOAT/DOUBLE | 700/701 native binary when finite, otherwise text | `RedshiftFloatingParameterTest`. |
| DECIMAL/NUMERIC | 1700 text | `SQL_C_NUMERIC` via `format_numeric` (precision 1-38). |
| DATE/TIME/TIMESTAMP | 1082/1083/1114 text | Character input with zone suffix or `T` separator is refused. |
| BINARY/VARBINARY/LONGVARBINARY | Redshift 6551 binary format (PostgreSQL: bytea hex text) | Character input must be hexadecimal; a trailing unmatched digit is ignored (prior audit batch 265, `CharacterBinaryParameterDecodesHexAndValidatesLength`). |
| INTERVAL, GUID | Refused by `SQLBindParameter` with `HYC00` | Valid SQL types, not in `is_supported_parameter_sql_type` (`odbc_handles.cpp` SQLBindParameter validation). |

## Case matrix

Status: **C** covered, **P** partial, **G** gap (no test), **D** demonstrated defect/limitation.

### Numeric

| # | Case | Expected (ODBC / Redshift) | Active behaviour | Evidence | Status |
|---|---|---|---|---|---|
| N1 | BIGINT min/max to SBIGINT/UBIGINT, scientific text | Exact; negative to unsigned 22003 | Exact digit path, no float detour | U `BigintLimitsStayExact`, `BigintScientificNotationStaysExact`; PG `BigintTextConversionAtExactLimits`; L `PreparedBigintExtremesNullOverflowAndRecovery`, `IntegerBoundariesAndNarrowing` | C |
| N2 | Narrowing to TINYINT/SHORT/LONG, fraction | 22003 / 01S07 | Implemented | PG `*TinyintGetDataChecksRangeAndFraction` etc.; L `IntegerBoundariesAndNarrowing` | C |
| N3 | DECIMAL(38,s) exact, overflow, NULL | Exact; 22003 when C precision too small | SDK `rescale_decimal` | F `NumericScaleApiTest.*`; L `PreparedDecimal38ExactNullOverflowAndRecovery`, `ExactDecimalAndNull` | C |
| N4 | SQL_C_NUMERIC rounding vs truncation of excess scale | Truncation with 01S07 | Toward zero | F `KnownNumericTargetsTruncateFractionTowardZeroAcrossAliases` | C |
| N5 | Negative scale / exponent bounds | Bounded, exact | Implemented | U `SignedScaleExponentLimitsAreExactAndBounded` | C |
| N6 | REAL/DOUBLE NaN, ±Infinity to FLOAT/DOUBLE | IEEE special | Recognised spellings `NaN`, `Infinity`, `-Infinity` | PG `FloatingSpecialValuesPreserveIeeeResults`; U `NonfiniteNumeric*` | C |
| N7 | Non-finite to integer/NUMERIC/CHAR | 22003 (numeric), raw text for CHAR | Policies tested | F `Nonfinite*`, `CharacterNonfinite*` | C |
| N8 | Signed zero | `-0` preserved for floats | Parameters: tested; results: PG only | U `FullLiteralWiresKeepExtremesSubnormalsAndSignedZero`; PG | P (no Redshift result case) |
| N9 | Float underflow to zero on narrowing | 22003 | Rejects | PG `FloatingConversionRejectsUnderflowToZero` | C |
| N10 | **DOUBLE result precision** | `SQL_C_DOUBLE` should equal the stored IEEE value | Results are server text. ODBCPP sets no `extra_float_digits`; the pinned AWS JDBC driver sets it to 3 or 2 at startup (`ConnectionFactoryImpl.java:425-428,910` @773fa16), which is reference evidence that default output is rounded. If the server emits 15 significant digits, `0.1::float8+0.2::float8` arrives as `0.3` and a rounded maximum can overflow. Not observed on Redshift. | — | **G (P1)**: author L case; decide on session `extra_float_digits` only after live proof |
| N11 | NUMERIC with no typmod | Unknown size 0 | `NUMERIC` size 0 | U `PreservesNumericAndTemporalModifiers` | C |

### Temporal

| # | Case | Expected | Active behaviour | Evidence | Status |
|---|---|---|---|---|---|
| T1 | DATE leap day / invalid day | Valid 29 Feb on leap years; 22007 otherwise | `valid_date` | U `ConvertDataDate`; F date tests | C |
| T2 | TIMESTAMP to DATE with time | 01S07 | Implemented | U `TimestampToDateReportsLostTime`; PG; L `TimestampDateTimeProjectionWarningsNullAndRecovery` | C |
| T3 | Fraction > 9 digits / precision loss | 01S07 | Implemented | U `TemporalFractionTruncationIsReported` | C |
| T4 | TIME to TIMESTAMP | Current date | Local date | U `TimeToTimestampUsesCurrentLocalDate` | C |
| T5 | **TIMETZ / TIMESTAMPTZ to struct** | ODBC structs carry no zone | Offset accepted and dropped without diagnostic; char targets keep the zone | U `ConvertDataTime` (`+02:00` dropped, only fraction warns); PG `TimeTextKeepsSecondsAndZone`; no Redshift L case | **P (P2)**: deliberate psqlODBC-like projection, undocumented to users; add L case `TIMESTAMPTZ` with non-UTC session zone |
| T6 | **BC dates and years > 9999** | Redshift DATE range 4713 BC – 294276 AD | `parse_date` requires exactly 10 chars: PostgreSQL-family ISO output such as `0044-03-15 BC` (not observed on Redshift) and `10000-01-01` fail with 22007 (format), not 22008 (overflow) | U `BcAndFiveDigitYearsRefuseWithoutPublishing` (added; refusal only, SQLSTATE not pinned) | **D (P2)**: honest refusal, wrong SQLSTATE class; L case still needed |
| T7 | TIME `24:00:00` | PostgreSQL accepts it; Redshift not observed | Rejected (hour > 23) with 22007 | U `ConvertDataTime` | P (P3): deliberate refusal, no Redshift L observation |
| T8 | **INTERVAL YEAR TO MONTH / DAY TO SECOND** | AWS maps to `SQL_INTERVAL_*` | Unknown VARCHAR text; TYPE_NAME `varchar`; parameter description triggers `pg_type` discovery | — | **D (P2)**: unsupported family; explicit design needed (not started) |
| T9 | Character input with zone or `T` to DATE/TIME/TIMESTAMP parameter | Refuse rather than silently shift | Refused | F/PG | C |
| T10 | TIMESTAMP precision metadata 0–6 | Size/digits by typmod | Implemented | U `PreservesNumericAndTemporalModifiers` | C |

### VARBYTE

| # | Case | Expected | Active behaviour | Evidence | Status |
|---|---|---|---|---|---|
| V1 | Result raw vs hex | BINARY raw bytes; CHAR two hex digits per byte (driver emits lower case) | Strict plain hex decode, `\x`/octal rejected | U `RedshiftVarbyteDecodesOnlyStrictPlainHexAndPreservesBytea`; W `ExplicitRedshiftVarbyteOwnsDecodedResultsAndDeferredErrors`; L `VarbyteConstantDirectPreparedResultContract` | C |
| V2 | Embedded NUL / empty / NULL results | Distinct | Distinct | W; L | C |
| V3 | Chunked SQLGetData / bound truncation | 01004, remaining length | Implemented | L `VarbyteConstantDirectPreparedResultContract` | C |
| V4 | Malformed hex cell | Deferred 22018, next row usable | Implemented | W | C |
| V5 | PostgreSQL OID 6551 isolation | Unknown text | Implemented | W `PostgresDoesNotInterpretRedshiftVarbyteOidOrHex` | C |
| V6 | Prepared hex-text input (`FROM_HEX(?)`) | Bytes | Implemented | L `PreparedHexTextToVarbyteBytes` | C |
| V7 | **Raw `SQL_C_BINARY` -> `SQL_LONGVARBINARY` parameter on Redshift** | Bytes preserved incl. 0x00 | 6551 binary-format bind | U/W `RedshiftParameterContractTest.*`, F | **G (P1)**: no live case; authored `RedshiftSpecialTypesRealTest.RawBinaryVarbyteParameterRoundTrip` |
| V8 | Parameter wire budget / maximum size | Refuse before send | Exact budget | W `RedshiftVarbyteMetadataRejectionOccursAfterPreparedExchange` | C (server maximum unverified) |
| V9 | Parameter arrays with binary columns | Per-row values | Generic array/deferred paths | F binary inputs in `DeferredInputTest.*` and `PreparedSelectArrayTest.*` | P (no Redshift L with arrays) |
| V10 | Result size metadata | Unknown (0 / SQL_NO_TOTAL) | Implemented; SQLGetTypeInfo advertises 16777216 | L `ContemporaryDdlTypeInfoPolicyContract` | C (16 MB advertisement unverified against docs here) |

## Prioritised findings

1. **P1 N10 — DOUBLE result precision.** Unknown whether Redshift text output
   round-trips IEEE doubles. A live case is authored
   (`RedshiftSpecialTypesRealTest.DoubleResultRoundTripsStoredValue`); no session
   setting is changed without that evidence.
2. **P1 V7 — raw binary VARBYTE parameters** lack live evidence; authored.
3. **P2 T8 — INTERVAL families** are presented as untyped VARCHAR. Requires a
   design (SQL_INTERVAL_* metadata, struct conversion, parameters); out of scope.
4. **P2 T6 — BC / five-digit years** fail with 22007. Correct handling (22008 or a
   documented refusal) is a narrow follow-up; not changed in this batch because
   it does not affect SUPER/spatial correctness.
5. **P2 T5 — time-zone projection** to structs is silent; document or diagnose.
6. **P3 T7 — `24:00:00`.**

No numeric/temporal/VARBYTE code was changed in this batch; the only
conversion-adjacent change is the special-type work.
