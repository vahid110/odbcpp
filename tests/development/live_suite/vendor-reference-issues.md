# Vendor reference discrepancies

Vendor comparisons supplement ODBCPP qualification. They are reported separately
from tests using our driver. The human accepted the following discrepancy as
non-blocking for ODBCPP PRs and cross-platform qualification on 2026-10-18.

## VENDOR-001: NULL decimal digits for INTEGER

- Reference: Amazon's official Redshift ODBC driver, macOS iODBC UCS4.
- Test: `RedshiftPackageImportLiveTest.CatalogDescriptorsDrivePreparedImport`
  in `it_redshift_official_import_live` (checkpoint case C0517).
- Observed: `SQLColumns` reports `TYPE_NAME=int4` and
  `DECIMAL_DIGITS=NULL` for the integer fixture column.
- Expected: integer decimal digits are `0` under the
  [ODBC decimal-digits contract](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/decimal-digits).
  The provider-specific name `int4` is valid and is not the failure.
- ODBCPP: the corresponding UCS2 and UCS4 cases both pass with decimal digits `0`.
- Disposition: external vendor conformance finding, retained as FAIL. No driver
  fix, assertion waiver, skip or vendor-binary modification is applied. Later
  import steps in this vendor case were not reached after the assertion failed.

The complete local collection on source
`108f704e4d96a8ce27c34d5b54999b3e1c872707` ran all 524 cases: 523 PASS, 1 FAIL,
0 SKIP and 0 unrun. All 520 ODBCPP cases passed; the four vendor reference checks
produced three passes and this failure. Cross-platform CI remains separate proof.

Evidence identities retained in the private checkpoint archive:

| Evidence | SHA-256 |
| --- | --- |
| Installed vendor library | `f8af78bf0f386de07f152e79282bf492b5ad506c19fce6a630c2f6acf02a5518` |
| C0517 GoogleTest XML | `ead40309ba422f374a7394577e76a467b90b74166550bf9e4d83929541c7bafe` |
| Complete collection | `427f4b2883a925d3689361364954cb148976f7304c214b798ae57e4c5770ab46` |
| Independent result and cleanup review | `415c348e71705c91c5fd7341f850c4213119ec8fe0de068a804b4a41eb122baa` |
