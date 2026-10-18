# CTest registration of existing live binaries

This standalone, language-free CMake project registers already qualified native
GoogleTest binaries. It does not build the product, stage credentials, admit
cloud use, resume/pause a cluster, or implement a test runner/result parser.
The coordinator owns those operations and the complete inventory's applicability.

Run from a fresh, owned private build directory for each complete collection:

```sh
umask 077
cmake -S tests/development/live_suite -B /absolute/private/ctest-run \
  -DODBCPP_LIVE_INDEX=/absolute/public/index.json \
  -DODBCPP_LIVE_EXPECTED=/absolute/public/expected.json \
  -DODBCPP_LIVE_OUTPUT=/absolute/private/ctest-run/gtest
ctest --test-dir /absolute/private/ctest-run --parallel 1 \
  --output-on-failure --output-junit /absolute/private/ctest-run/ctest-results.xml
```

Do not add `--stop-on-failure`, `-R`, `-E`, `--rerun-failed`, random scheduling or
other case selection to a full collection. Ordinary failures, aborts and timeouts
are CTest failures; CTest continues running later registered cases. There are no
case-to-case success dependencies. RUN_SERIAL and a shared RESOURCE_LOCK prevent
concurrent live cases even if the parallel setting is accidentally increased.
Use a fresh build directory rather than prior CTest timing/failure history.

## Public index, schema_version 1

```json
{
  "schema_version": 1,
  "images": {
    "core_it_metadata_real": {
      "executable": "/absolute/accepted/it_metadata_real",
      "sha256": "<64 lowercase hexadecimal characters>"
    }
  },
  "profiles": {
    "profile_id": {
      "environment_file": "/absolute/private/profile-environment.json"
    }
  },
  "cases": [
    {
      "id": "metadata.case.profile_id",
      "image": "core_it_metadata_real",
      "profile": "profile_id",
      "gtest_filter": "MetadataIntegrationTest.FindsUnicodeCatalogIdentifiers",
      "timeout_seconds": 40
    }
  ]
}
```

`images` are public binary paths/pins. CMake hashes each referenced image once
before registering any runnable test project successfully. `cases` contain the
complete applicable case/profile list in accepted source order, all 524 for the
current inventory. Every ID and image/profile/filter tuple must be unique.
Parameterized GoogleTest names containing `/` are supported; wildcard, negative
or multi-case filters are rejected. The coordinator derives this index and its
independent expected inventory from accepted source/profile data, retaining
source/image/inventory pins and all explicit unavailable profile boundaries.
This module cannot establish completeness from a self-authored smaller expected
list: the expected list's independent accepted provenance is a coordinator input.

The separate expected JSON contains `schema_version: 1` and an ordered `cases`
array with exactly `id`, `image`, `profile`, `gtest_filter` from each expected
entry. Missing/extra, duplicate or reordered entries fail configuration; neither
count alone nor a previous PASS removes an entry from a new complete collection.

The private environment file contains:

```json
{"environment": ["NAME=literal value", "ODBCINI=/absolute/private/odbc.ini"]}
```

Only CMake reads that file. It may contain credential-bearing connection values,
so do not open it in public tools or place its contents in the public index.
Semicolons in values are preserved. Duplicate variable names, newlines and CMake
generator expressions are rejected rather than silently changing values.
For offline smoke only, a profile may instead contain a public `environment`
array of nonsecret values. It cannot also contain `environment_file`.
The coordinator must stage all necessary per-profile variables and launch CTest
with the intended inherited environment; unlisted parent variables are inherited.

Configure requires Unix umask 077, protects the build and XML directories as
0700, and generates CTestTestfile.cmake as 0600. The generated test file contains
resolved private environment values. **Do not archive it publicly, run verbose
CTest, expose `ctest --show-only=json-v1` for a live project, or publish its private
metadata/logs without review.** Keep the same umask for CTest output. Prepare the public inventory and build
readiness while paused. Within the admitted session, create required database
fixtures, stage their concrete profile values, and start CTest promptly.

## Native commands and evidence

Each registered test directly invokes its accepted binary with one exact
`--gtest_filter`, a distinct `--gtest_output=xml:.../case-N.xml`, no color and
`--gtest_also_run_disabled_tests`. CTest owns the process, timeout and result;
there is no Python intermediary, private case scheduler or XML-size acceptance
limit. GoogleTest SKIP is retained in its standard native XML and stdout. It is not
mapped using CTest SKIP_REGULAR_EXPRESSION: that property overrides a nonzero
native exit and can hide a real failure followed by a skip. A CTest success means
process success; it must not be reported as a body PASS without the standard
GoogleTest result. After the whole CTest run, report the raw native XML's actual
PASS/FAIL/SKIP, preserving CTest failures for crashes/incomplete XML. Zero-test output is
an explicit CTest failure rather than a false PASS. Accepted inventory and binary
pins remain necessary because an exit code alone is not an independent proof of
case identity.

Retain native `gtest/case-N.xml`, CTest JUnit, `Testing/Temporary/LastTest.log`,
`Testing/Temporary/LastTestsFailed.log`, and CTest's final exit status. A crash or
timeout may produce no complete GoogleTest XML; retain CTest's failure and raw
output instead of treating missing XML as a pass. JUnit and logs are not subject
to the historical bespoke parser's XML/text size gate. CTest retains process outcomes, while native GoogleTest XML distinguishes body
PASS/FAIL/SKIP. Overall CTest nonzero remains nonzero even when later processes
succeed.
Operational cleanup/admission and protected process/query/principal evidence
remain the coordinator's responsibility, including after nonzero CTest exit.

## Offline regression

```sh
python3 -B tests/development/live_suite/test_live_suite.py
```

Only synthetic fixture processes execute. The test configures a language-free
project, runs actual CTest with six cases (PASS, FAIL, SIGABRT, SKIP, later PASS,
zero-test), verifies sequential continuation and standard GoogleTest SKIP evidence, checks private
file modes/semicolon environment preservation, and rejects six invalid inventory
or image inputs. It retains all CTest artifacts under a fresh printed temporary
path. This is an offline safety regression, not native Redshift proof.

To validate registration against the current complete public inventory without
running any accepted binary or reading any private profile input, add:

```sh
python3 -B tests/development/live_suite/test_live_suite.py \
  --inventory-index /absolute/public/index524-blueprint.json \
  --inventory-expected /absolute/public/expected524.json
```

This replaces image/profile execution data with the synthetic fixture in a fresh
offline copy, configures all identities, then uses `ctest -N` only for that copy.
