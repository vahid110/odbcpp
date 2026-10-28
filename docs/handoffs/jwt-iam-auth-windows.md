# Handoff: JwtIamAuthPlugin — Windows and WSL qualification

**Temporary file.** When the steps below are done, record the results in the PR
(a comment, plus the PR description's status table), then **delete this file in
a commit** named like `Record Windows/WSL results and remove the Windows handoff`.
No credentials, tokens or connection strings belong in this file or in the PR.

## Context

- Branch: `codex/redshift-jwt-iam-auth`, based on `origin/main` `3dc4ee7`.
- The PR for this branch is the authoritative change summary.
- Change: Redshift `JwtIamAuthPlugin` as an SDK-level credential source. A JWT
  plus `role_arn` go through an unsigned STS `AssumeRoleWithWebIdentity`, and
  the assumed-role credentials feed the existing Serverless `GetCredentials`
  path (PR #95). The role ARN parser moved into the SDK-free STS contract
  (`sdk/src/auth/sts_assume_role_response.cpp`) and is shared with the profile
  role-assumption path.
- Already verified on macOS arm64 (static OpenSSL 3.6.5, AWS SDK 1.11.906):
  offline `test_jwt_iam_auth` 12/12, PR #95 regressions 18/18, 20/20 and 16/16,
  and **live** `it_redshift_serverless_jwt_iam_auth` 3/3 on the eu-north-1 pilot
  workgroup.

## What Windows/WSL can and cannot exercise

- `cmake/NativeProvisionedAuth.cmake` (the AWS SDK module holding the JWT owner,
  `test_jwt_iam_auth` and the live JWT test) builds **only** on Darwin arm64 with
  bundled static OpenSSL. On Windows and WSL these targets do not exist; their
  absence is expected, not a failure. Do not try to enable the module there.
- What changes on Windows/WSL builds:
  - AuthCore: `parse_sts_role_arn` and its tests in `test_sts_assume_role_response`
    (`StsRoleArnTest.*`, 2 new cases).
  - `cmake/Tests.cmake`: `test_jwt_iam_auth` and
    `it_redshift_serverless_jwt_iam_auth` are excluded from the ordinary unit
    and integration globs. Confirm neither appears in any CTest inventory.
  - `tools/ci/sanitizer_evidence.py`: `test_jwt_iam_auth` joins the optional
    set (Linux sanitizer job; nothing to run locally unless convenient).
- No ODBC connection keyword, DSN field or setup-dialog change is in this PR.
  The DSN GUI run below is a **regression** check that the dialog and setup
  model are unchanged.

## Steps

Use a fresh owned build directory per profile inside the checkout. Record the
exact HEAD, configuration and raw CTest/GoogleTest results.

### 1. Windows offline (MSVC, Redshift)

```powershell
git fetch origin; git checkout codex/redshift-jwt-iam-auth
cmake --preset windows-redshift -B build-windows-redshift-jwt   # the host's usual OpenSSL BUNDLED_SHARED settings
cmake --build build-windows-redshift-jwt --config Release
ctest --preset windows-redshift-offline --test-dir build-windows-redshift-jwt
```

Expect the usual full pass. Specifically confirm:
- `test_sts_assume_role_response` includes and passes `StsRoleArnTest.SplitsPartitionAccountAndRoleNameWithPath`
  and `StsRoleArnTest.RefusesForeignShapes`;
- no `test_jwt_iam_auth` or `it_redshift_serverless_jwt_iam_auth` entry in
  `ctest -N`.

### 2. Windows DSN GUI (do not skip)

Follow [WINDOWS_DSN.md](../../WINDOWS_DSN.md), "Automated local checks without UAC":

```powershell
# Configure the separate Redshift DSN graph for this exact checkout first.
./tools/development/test_windows_dsn_gui.ps1 -BuildDirectory build-windows-dsn-redshift
```

Under a standard (non-elevated) token, expect all owning GoogleTests (provider,
setup-model and native-dialog suites) to pass with no skips. Inspect the native
captures of all four sections: they must match `main`, with no new or missing
fields (this PR adds none). Report the `gui-local-*` result record path and counts.

### 3. WSL Ubuntu offline

```sh
cmake --preset unixodbc-redshift -B build-unixodbc-redshift-jwt && cmake --build build-unixodbc-redshift-jwt -j2
ctest --preset unixodbc-redshift-offline --test-dir build-unixodbc-redshift-jwt
cmake --preset linux-iodbc-redshift -B build-linux-iodbc-redshift-jwt && cmake --build build-linux-iodbc-redshift-jwt -j2
ctest --preset linux-iodbc-redshift-offline --test-dir build-linux-iodbc-redshift-jwt
```

Same confirmations as step 1 for both presets.

### 4. Optional: live regression through the driver managers

Only if the host already holds the usual pilot admission and fixtures. The
ordinary password login path is unchanged, so this is a smoke check. Use the same
procedure as earlier Windows handoffs: a temporary per-user DSN removed afterwards
on Windows, private ini registration on WSL. The JWT live test cannot run here
(Darwin-only module). Restore any temporary security-group rule afterwards.

## Reporting

Post one PR comment with, per profile: HEAD, toolchain, CTest pass/fail/skip
counts, GoogleTest counts, the two `StsRoleArnTest` results, the CTest inventory
confirmation, and the DSN GUI record. Report any failure verbatim, and keep
first failures even if a re-run passes. Then delete this file as described at
the top.
