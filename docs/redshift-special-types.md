# Redshift SUPER, GEOMETRY and GEOGRAPHY

Branch `codex/redshift-special-types`, baseline `efe7af379469303415f7ccce3a5d8d626a92a886`.
This document records the inspected baseline, the evidence used, the chosen
contracts and their physical owners. Conversion coverage for numeric, temporal
and VARBYTE values is audited separately in
[redshift-conversion-coverage.md](redshift-conversion-coverage.md).

## Evidence inspected (2026-10-22)

Source, documentation and runtime are separate evidence classes. No Redshift
or vendor runtime was exercised for this work; every runtime row below is
pending a separately admitted live checkpoint.

| Ref | Evidence | Observation |
|---|---|---|
| A1 | AWS ODBC `aws/amazon-redshift-odbc-driver` @ `868e9b0d035f5e219890c2de424b9d29d130f300` (v2.3.0), `src/odbc/rsodbc/rsodbc.h:176-182` | OIDs: GEOMETRY 3000, GEOGRAPHY 3001, GEOMETRYHEX 3999, SUPER 4000, VARBYTE 6551, HLLSKETCH 2935. |
| A2 | same, `rslibpq.c:1701-1745` (OID to SQL type) | SUPER -> `SQL_LONGVARCHAR` (`SQL_WLONGVARCHAR` under UseUnicode); VARBYTE, GEOMETRY, GEOMETRYHEX, GEOGRAPHY, HLLSKETCH -> `SQL_LONGVARBINARY`. |
| A3 | same, `rslibpq.c:2352-2366,2385-2390` | Without a type modifier, reported sizes are fixed constants (`MAX_SUPER_SIZE` 4194304, `MAX_GEOMETRY_SIZE` 1024000, `MAX_GEOGRAPHY_SIZE` 1048316). These are driver constants, not server-reported dimensions. |
| A4 | same, `rsutil.c:2444-2462` (C_CHAR), `5469-5490` (C_BINARY), `2848-2866` (C_WCHAR) | GEOMETRY(3000) payload is treated as raw bytes in **every** format: C_BINARY copies it, C_CHAR hex-encodes it. GEOGRAPHY/GEOMETRYHEX/VARBYTE text-format payloads are treated as hexadecimal: C_BINARY hex-decodes, C_CHAR copies the hex. |
| A5 | same, `rsutil.c:8596-8628` | SQL_DESC_TYPE_NAME: `SUPER`, `GEOMETRY`, `GEOGRAPHY`, `VARBYTE` (upper case); catalog `rscatalog.cpp:3152-3180` SQLGetTypeInfo lists lower-case `super` (LONGVARCHAR, 4194304), `geometry`/`geography` (LONGVARBINARY, 1000000). |
| A6 | same, `rslibpq.c:278-284` | The AWS driver sends startup `client_protocol_version` (default 1, EXTENDED_RESULT_METADATA). ODBCPP sends no such option (base protocol 0). Results are requested in text format by both (`RS_TEXT_FORMAT`, ODBCPP `append_u16(out, 0)`). |
| J1 | AWS JDBC `aws/amazon-redshift-jdbc-driver` @ `773fa16aabbd589e9e2c93684c7ca2543e3ce31a`, `RedshiftResultSet.java:2362-2384,2915-2940` | GEOMETRY(3000) bytes are returned raw by `getBytes` and hex-encoded by `getString`; GEOMETRYHEX(3999) is hex EWKB text that `getString` re-normalizes; GEOGRAPHY/VARBYTE `getBytes` hex-decode text cells. |
| P1 | AWS Python `aws/amazon-redshift-python-driver` @ `248ffd5896c7a46bba115452dc10b8389d3ede54`, `redshift_connector/utils/type_utils.py:518-627`, `test/integration/datatype/_generate_test_datatype_tables.py:54-193` | SUPER is decoded as text. GEOMETRYHEX is hex text. Integration expectations, run for client protocols 0, 1 and 2, compare geometry/geography results to lower/upper-case hex EWKB, e.g. `ST_GeogFromText('POINT(1 1)')` -> `0101000020E6100000000000000000F03F000000000000F03F`. |
| D1 | [ODBC 2.x data type conversions](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-converting-data-types.html) | SUPER `SQL_LONGVARCHAR`; GEOMETRY, GEOGRAPHY, VARBYTE `SQL_LONGVARBINARY`. Type names only; no payload or size claim. |
| D2 | [SUPER limitations](https://docs.aws.amazon.com/redshift/latest/dg/limitations-super.html) | "An individual SUPER object can hold up to 16 MB of data." Casting a non-scalar SUPER value to a string returns NULL; `JSON_SERIALIZE` serializes; result display shows JSON text such as `[1,"abc",null]`. |
| D3 | [Spatial considerations](https://docs.aws.amazon.com/redshift/latest/dg/spatial-limitations.html) | "The maximum size of a GEOMETRY or GEOGRAPHY object is 1,048,447 bytes." UNLOAD writes "hexadecimal EWKB format". |
| D4 | [Simba data types](https://documentation.insightsoftware.com/simba-amazon-redshift-odbc-data-connector-win/content/odbc/re/features/datatypes.htm) | Vendor documentation only: SUPER as LONGVARCHAR/WLONGVARCHAR, spatial binary. No Simba runtime or source was available; vendor runtime behaviour is unobserved. |

Normative ODBC: a `SQL_LONGVARBINARY` column converted to `SQL_C_CHAR`/`SQL_C_WCHAR`
yields two hexadecimal characters per byte; to `SQL_C_BINARY` it yields the
bytes. `SQL_C_DEFAULT` for LONGVARCHAR is `SQL_C_CHAR`, for LONGVARBINARY
`SQL_C_BINARY`. Chunked `SQLGetData` and truncation (`01004`) follow the
generic character/binary rules. These conversions already exist in ODBCPP
(`odbc/odbc_handles.cpp` binary hex/raw paths, `odbc/result_types.cpp`).

### Wire-format inference and its limits

ODBCPP negotiates base protocol 0 and text result format. The AWS ODBC/JDBC
drivers negotiate protocol 1/2. Their handling of OID 3000 as raw bytes, and of
3999/3001/6551 as hexadecimal text, is consistent across A4, J1 and P1. Which
OID Redshift sends for GEOMETRY under protocol 0 has **not** been observed. The
design therefore handles both GEOMETRY OIDs with their reference semantics and
fails closed on an inconsistent payload rather than guessing (see below).

## Baseline current state (efe7af3)

| Path | SUPER | GEOMETRY / GEOGRAPHY |
|---|---|---|
| SQLColumns / catalog (`redshift_catalog_query.cpp:718-727,1889-1891`) | `SQL_LONGVARCHAR`, unsized, TYPE_NAME `super` (unit + live metadata tests). | `SQL_LONGVARBINARY`, unsized. |
| Result RowDescription -> `describe_type` (`pg_type_info.cpp` default, `pg_value.cpp`) | OID 4000 unknown: `VarChar`, `known=false`. Resolver then queries `pg_type` and still yields unknown `VARCHAR`. TYPE_NAME `varchar`. | OIDs 3000/3001/3999 unknown `VARCHAR`. Unknown VarChar cells are validated as UTF-8 text, so raw EWKB containing non-UTF-8 bytes became a deferred cell error, and hex text was presented as character data rather than binary. |
| Result cell normalization (`generic_database_connection.cpp:942-967`) | Unknown text validated as UTF-8. | No binary decoding. |
| SQLGetTypeInfo (`pg_type_catalog.cpp` `redshift_definitions`) | Absent; `SQL_LONGVARCHAR` returns no Redshift row. | Absent. |
| Parameters | Text parameters are sent as OID 25 text. ParameterDescription OID 4000 resolves to unknown VARCHAR. | Binary parameters are Redshift VARBYTE (OID 6551, binary format); not a spatial pathway. |
| Legacy `odbc/redshift/redshift_types.cpp` (SUPER->LONGVARCHAR, spatial->"WKT" LONGVARCHAR) | Not compiled into any target (only listed in `cmake/internal-headers/odbc.txt`); not an active defect. Left unchanged; its spatial WKT comment is contradicted by A4/J1/P1/D3. | same |

## Design

### SUPER (OID 4000)

* Result family `LongVarChar` (`SQL_LONGVARCHAR`), `known=true`, column size
  0 (unknown). The server sends no length modifier; the 16 MB object limit (D2)
  is an object bound, not a character length, so it is not reported per column.
  This deliberately differs from A3's fixed 4194304.
* Cells are server-produced JSON text, validated as UTF-8 and preserved byte for
  byte. No driver-side JSON parsing or reserialization. SQL NULL is a NULL cell
  (`SQL_NULL_DATA`); the JSON literal `null` is the 4-byte text `null`.
* TYPE_NAME / LOCAL_TYPE_NAME `super` for IRD, `SQLColAttribute`,
  `SQLDescribeParam`/IPD, matching SQLColumns' lower-case catalog label.
* SQLGetTypeInfo row `super`, `SQL_LONGVARCHAR`, COLUMN_SIZE 16777216 (D2), no
  literal prefix/suffix (no SUPER literal syntax; a quoted literal is a VARCHAR
  that the server converts into a SUPER *string* scalar), case sensitive.
* Parameters: there is no SUPER parameter type. A character parameter remains
  OID 25 text. Assigning text to a SUPER column yields a SUPER string scalar;
  JSON objects/arrays require explicit `JSON_PARSE(?)`. Both pathways are
  authored as live GoogleTests and remain unrun. Direct binary or SUPER-typed
  binding is unsupported and documented, not emulated.

### GEOMETRY (3000, 3999) and GEOGRAPHY (3001)

* Result family `LongVarBinary` (`SQL_LONGVARBINARY`), `known=true`, size 0
  (unknown). The 1,048,447-byte limit (D3) is advertised only in
  SQLGetTypeInfo, not as a per-column dimension.
* The application value is the EWKB byte string. `SQL_C_BINARY`/`SQL_C_DEFAULT`
  return the bytes (including zero bytes); `SQL_C_CHAR`/`SQL_C_WCHAR` return the
  driver's existing generic lower-case hex of those bytes. No WKT, no SRID rewriting, no SQL
  rewriting.
* Backend codec by OID (Redshift profile only):
  * 3999 GEOMETRYHEX and 3001 GEOGRAPHY: strict even-length hexadecimal
    (shared `rs::util::decode_hex`, as for VARBYTE). Invalid text is a deferred
    cell error (`22018`-class data error at fetch), never raw fallback.
  * 3000 GEOMETRY: the cell bytes are the EWKB value. The first byte of any
    WKB/EWKB value is its byte-order flag (0x00 or 0x01). A non-empty payload
    whose first byte is neither is reported as a deferred cell error. This also
    rejects hexadecimal text (`'0'` is 0x30), so an unexpected hex payload under
    protocol 0 cannot be silently returned as ASCII bytes. No further EWKB
    parsing is performed.
* TYPE_NAME `geometry` / `geography`; SQLGetTypeInfo rows `geometry` and
  `geography`, `SQL_LONGVARBINARY`, COLUMN_SIZE 1048447 (D3).
* Parameters: binary parameters remain Redshift VARBYTE (6551). VARBYTE does not
  convert implicitly to spatial types (Redshift documentation). Supported
  pathway: explicit SQL such as `ST_GeomFromEWKB(?)` with a hex character
  parameter; authored as live GoogleTests, unrun. No implicit rewriting.
* HLLSKETCH (2935) is out of scope and retains the unknown fallback.

### Physical ownership

| Component | Owner file | Rationale | Allowed dependencies | Proof |
|---|---|---|---|---|
| `NativeTypeInfo::type_name` (optional owning backend presentation name) | `sdk/include/odbcpp/database/native_type_info.h` | Provider-neutral normalized contract: a family (`LongVarChar`) cannot identify the native type name an application sees. Not a native ID, no codec. | std only | `test_native_types`, `test_backend_contract` |
| Native-ID-aware cell hook `normalize_native_result_value` | `core/database/generic_database_connection.{h,cpp}` | Shared result normalization already owns cell decoding; it must pass the native ID so a backend can select a codec. Default forwards to the existing family codec. | core/database | `test_connection_liveness` |
| Redshift OID families, names and codecs (4000/3000/3001/3999) | `core/database/postgres/pg_value.cpp` (`PgDatabaseConnection`) | Native OIDs and EWKB/hex framing are Redshift backend semantics; PostgreSQL profile is unchanged. | `odbcpp/util/hex.h` | `test_native_types`, `test_connection_liveness` |
| SQLGetTypeInfo advertisements | `core/database/postgres/pg_type_catalog.cpp` | Existing Redshift DDL advertisement table. | none | `test_backend_contract` |
| TYPE_NAME presentation on IRD/IPD | `odbc/odbc_handles.cpp` | ODBC descriptor adaptation. | SDK contract only | `test_backend_contract` |
| Binary hex/raw, chunking, truncation, NULL | existing `odbc/odbc_handles.cpp` | Reused unchanged; no spatial or JSON framework. | — | existing + new adapter tests |

## Pending evidence

* Live: which GEOMETRY OID and payload Redshift sends under protocol 0;
  SUPER text payload for object/array/scalar/Unicode/large values; text and
  `JSON_PARSE(?)` parameter semantics; `ST_GeomFromEWKB(?)` round trip; SEARCHABLE
  semantics for spatial types. Authored in `tests/integration/it_redshift_real.cpp`,
  skipped unless the `ODBCPP_REDSHIFT_SPECIAL_TYPES_ADMISSION` fixture is admitted.
* Windows/Linux builds, unixODBC/Windows DM and DM2/DM4 behaviour.
* Simba runtime comparison (no binary available).
