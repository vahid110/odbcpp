# Redshift native IdP — hand-off for the next assistant

Branch `codex/redshift-native-idp`, worktree
`/Users/vahidsbr/.codex/worktrees/redshift-native-idp/odbcpp`, based on
`origin/main` @ `b5063e1`. All commits are local; **nothing is pushed**.
Branch head: `df763ab`.

## 1. Objective

Implement and live-verify Redshift native IdP authentication for the ODBCPP
driver, on top of the completed STS AssumeRole work (PR #95, separate branch
`redshift-sts-assume-role`). Three auth paths, additive and opt-in via the
`OrdinaryPassword | TemporaryDatabasePassword | NativeIdpToken` seam:

1. `IdpTokenAuthPlugin` — direct token (works offline; live test written, token
   acquisition blocked on identity-enhanced creds, deprioritized).
2. `GetIdentityCenterAuthToken` — identity-enhanced token (owner implemented;
   live test admission-gated, blocked on TIP credentials not mintable via CLI).
3. `BrowserIdcAuthPlugin` — OAuth2 + PKCE via IdC (fully implemented; live test
   **actively failing on an AWS-side authorization**, see section 4).

Future IdP types (Azure etc.) = new `idp_type` values; no protocol redesign.

## 2. What is implemented and committed

- `REDSHIFT_NATIVE_IDP_PLAN.md` — design doc (wire format, ownership, phases;
  section 7 documents the AWS-side prerequisites, updated 2026-10-24).
- Commits: `28d1802` SDK contract (`Method::NativeIdpToken`, `NativeIdpAttributes`
  with optional `identity_namespace`), `bd10113` backend seam
  (`ConnectionSettings::native_idp`, startup keys, token cleanse), `638ed8c`
  adapter (plugin→`idp_type` derivation, ODBC keywords), `297c6e9` integration +
  admission-gated live tests, `a5773c8` DSN option exposure, `6f89b4f`
  `IdcTokenOwner` (GetIdentityCenterAuthToken), `e75c82c` `BrowserIdcOwner`
  (OAuth/PKCE), `4de5bdd` PKCE unpadded-base64url fix, `3c35afa` AUTH_REQ_IDP
  protocol + optional `identity_namespace`, `df763ab` plan-doc prerequisites.
- Wire format matches the native AWS driver byte-for-byte (verified against
  `~/.local/state/odbcpp/continuation/trust-store-claude-20261022/ref/aws-odbc/src/odbc/rsodbc/rslibpq.c`):
  startup keys `idp_type`/`token_type`/`provider_name`/`web_identity_token`
  (+ `identity_namespace` only for `IdpTokenAuthPlugin`), no password; server
  `AUTH_REQ_IDP` (code 14) answered with an `'i'` packet over verified TLS.
- Tests: unit suites green (122 unit + 31 security + 2 redshift offline;
  `ctest --preset macos-iodbc-redshift-offline`), including new unit coverage for
  the Idp parse/answer/TLS/wire-limit and an offline integration that scripts
  `AUTH_REQ_IDP` and asserts the `'i'` packet for both plugins.
- Build: preset `macos-iodbc-redshift` with
  `-DODBCPP_IODBC_ROOT=/opt/homebrew/opt/libiodbc`
  `-DODBCPP_REDSHIFT_NATIVE_IDC_TOKEN_TESTS=ON`
  `-DODBCPP_AWS_SDK_INSTALL=$HOME/.local/state/odbcpp/aws-sdk-install-1.11.906`.
  AWS SDK built with `sso-oidc` (`BUILD_ONLY=redshift;redshift-serverless;sts;sso-oidc`).
- Offline fake-HTTP tests need `ODBCPP_AUTH_SDK_OFFLINE_FIXTURE=synthetic-fake-http-only`.

## 3. Live test procedure (BrowserIdcAuthPlugin)

Binary: `build-macos-iodbc-redshift/tests/it_redshift_browser_idc_live`.

```sh
cd /Users/vahidsbr/.codex/worktrees/redshift-native-idp/odbcpp/build-macos-iodbc-redshift
ODBCPP_NATIVE_IDC_ISSUER_URL=https://identitycenter.amazonaws.com/ssoins-<instance-id> \
ODBCPP_NATIVE_IDC_REGION=eu-north-1 \
ODBCPP_NATIVE_IDC_LISTEN_PORT=7890 \
ODBCPP_NATIVE_IDP_HOST=odbcpp-redshift-pilot.<account-id>.eu-north-1.redshift-serverless.amazonaws.com \
ODBCPP_NATIVE_IDP_PORT=5439 \
ODBCPP_NATIVE_IDP_DATABASE=odbcpp_pilot \
ODBCPP_NATIVE_IDP_USER=odbcpp-idc-test \
ODBCPP_NATIVE_IDP_PROVIDER=odbcpp_idc \
ODBCPP_NATIVE_IDP_TOKEN_TYPE=ACCESS_TOKEN \
ODBCPP_NATIVE_IDP_CA=$PWD/../root.crt \
./tests/it_redshift_browser_idc_live
```

- **Region must be eu-north-1** (IdC instance replicated there; us-east-1 fails
  with "Failed to validate auth token").
- A browser opens at `http://127.0.0.1:7890`; the human must click
  **Allow Access** (or sign in again if the IdC session was cleared; the IdC
  test user is `odbcpp-idc-test`, password known to the user).
- The test currently has an intentional temporary `DIAG` debug print
  (uncommitted) that dumps the full backend error; remove it before committing.
- `it_redshift_native_idp_live` (direct token) and
  `it_redshift_native_idc_token_live` (GetIdentityCenterAuthToken) are
  admission-gated siblings in the same directory.

## 4. The live blocker (state as of 2026-10-24 ~21:00 UTC)

**Symptom:** every live run fails with SQLSTATE `28000`,
`Authentication failed: Insufficient privileges to access AWS IdC API.`

**CloudTrail (eu-north-1) shows, per attempt:**
- `sso-oauth.amazonaws.com CreateTokenWithIAM` — **succeeds** (caller
  `AWSService`, invokedBy `redshift.amazonaws.com`).
- `identitystore.amazonaws.com DescribeUser` — **AccessDenied**:
  `AWS Internal is not authorized to perform: identitystore:DescribeUser on
  resource: arn:aws:identitystore::<account-id>:identitystore/d-<identity-store-id> ...
  because no resource-based policy allows the identitystore:DescribeUser action`
  (also denied on `arn:aws:identitystore:::user/<idc-user-id>`).
- `kms Decrypt` by redshift shows `DryRunOperationException` with message
  "The request would have succeeded, but the DryRun option is set" — KMS dry-run
  **success**, KMS is healthy.

**Error progression observed earlier:** us-east-1 → "Failed to validate auth
token"; eu-north-1 without app assignment → "Invalid scope. User's credentials
are not authorized to connect to Redshift"; after assignment → the current
identitystore denial.

**Everything verified present and correct:**
- Redshift IdC app `odbcpp-idc-app2` (`IdcOnboardStatus: Completed`,
  `ApplicationType: None`), ARN
  `arn:aws:redshift:eu-north-1:<account-id>:redshiftidcapplication:<app-id>`,
  managed SSO app `apl-<managed-app-id>` (display name ODBCPP-Redshift2).
- ActorPolicy (IAM auth method): allows `sso-oauth:CreateTokenWithIAM` +
  `IntrospectTokenWithIAM` for `redshift.amazonaws.com` +
  `redshift-serverless.amazonaws.com`, source-account condition.
- Grants: `refresh_token` + `urn:ietf:params:oauth:grant-type:token-exchange`.
- Access scope: `redshift:connect` with empty AuthorizedTargets, installed via
  `aws redshift modify-redshift-idc-application` `ServiceIntegrations.Redshift[].Connect.Authorization=Enabled`
  (direct `sso-admin put-application-access-scope` on a Redshift-managed app is
  rejected: "supported through integration with its associated AWS service").
- User assignment: `<idc-user-id>` assigned to
  `apl-<managed-app-id>`; `AssignmentRequired: true`.
- Integration role `odbcpp-idc-integration-role`: trust = redshift +
  redshift-serverless with `sts:AssumeRole`,`sts:SetContext`; inline policies
  `odbcpp-idc-integration-rolePolicy` (GetCredentials on the workgroup) and
  `odbcpp-idc-validation` (sso:DescribeApplication/DescribeInstance on the
  instance + app ARNs, redshift-serverless List/Get, sts:SetContext,
  identitystore Describe/List/IsMemberInGroups).
- Role now attached to the namespace `iamRoles` (was empty; fixed 2026-10-24).
- DB provider row (`SELECT * FROM svv_identity_providers;`): name `odbcpp_idc`,
  type `awsidc`, namespc `awsidc`, instanceid = the managed app ARN, params
  `{iam_role, instance_arn, identity_store_id: d-<identity-store-id>}`, enabled.
- IdC KMS key `mrk-<kms-key-id>` (alias
  `odbcpp-idc-replica`): policy has all baseline provisions including
  `AllowManagedApps`; account is in org `o-<org-id>`.
- Driver-side: startup keys and `'i'` packet byte-identical to AWS's driver.

**Ruled out:** role permissions (the data plane never assumes the integration
role during login — only `AWSServiceRoleForRedshift` at unrelated times);
namespace lakehouse registration (`update-lakehouse-configuration` requires
federated-permissions registration first — different feature); KMS; assignment;
grants; scope propagation delay (denials recurred 1.5h after the scope install).

**Open hypotheses for the next assistant:**
1. The identitystore's internal "resource-based policy" for the Redshift service
   is created by some app-creation step our CLI path skipped, even though the
   app shows `Completed`. Worth testing: delete `odbcpp-idc-app2` and re-create
   the IdC application **through the Redshift console wizard** (not CLI), then
   re-point the DB provider.
2. The IdC instance is multi-region-replicated (primary us-east-1, replicas
   eu-north-1 + us-west-2) and was created by an earlier session; the
   identitystore resource policy may exist only in the home region. A fresh
   single-region instance would isolate this.
3. AWS-internal issue: an identical setup on Stack Overflow
   (question 78077294, "AWS Redshift Serverless IAM Identity Center
   Authentication not working") was only resolved via an AWS support ticket
   (their error later changed to `ISACGetSessionException`).

## 5. Diagnostic tooling

- Profiles: `adbc-coordinator` (SSO admin), `adbc-provisioner` (IAM writes,
  KMS, redshift IdC-app modify, CloudTrail lookup), `odbcpp-redshift-pilot`
  (GetCredentials + read-only serverless).
- CloudTrail (both regions matter; events lag ~1–5 min):
  `aws cloudtrail lookup-events --profile adbc-provisioner --region eu-north-1 --start-time "$(date -u -v-90M +%s)" --query 'Events[].CloudTrailEvent'`.
- Admin DB channel (works): `PGPASSWORD=$(cat /tmp/odbcpp_admin_pw.txt) psql
  "host=odbcpp-redshift-pilot.<account-id>.eu-north-1.redshift-serverless.amazonaws.com
  port=5439 dbname=odbcpp_pilot user=odbcpp_pilot_admin sslmode=verify-ca
  sslrootcert=$PWD/root.crt"`.
- Native driver reference: `~/.local/state/odbcpp/continuation/trust-store-claude-20261022/ref/aws-odbc/`
  (`rslibpq.c`, `fe-auth.c`, `pqcomm.h`, `BrowserIdcAuthPlugin.cpp`).
- AWS SDK install: `~/.local/state/odbcpp/aws-sdk-install-1.11.906/`.

## 6. AWS resources created by this work (cleanup candidates)

Permission set `odbcpp-idc-test-ps` + role
`AWSReservedSSO_odbcpp-idc-test-ps_90ce6db419dd9e4b`, role
`odbcpp-idc-identity-enhanced`, policies `odbcpp-sso-iam-provisioning`,
`odbcpp-idc-sts`, role `odbcpp-idc-integration-role` + the two IdC apps
(`odbcpp-idc-app2` active / `odbcpp-idc-app` failed onboarding), SAML provider
for the permission set, KMS key `mrk-<kms-key-id>` +
alias, SG `sg-06227a706295e3f05` (IP re-scoping may still be pending).
Cluster must be **PAUSED** during gaps and at the end.

## 7. After the live test passes (remaining driver work)

1. Remove the `DIAG` debug block + `<iostream>` include from
   `it_redshift_browser_idc_live.cpp`; re-run
   `python3 -B tools/development/check_changed_gtest_control_flow.py --base b5063e1`
   and the offline preset; commit.
2. Expect a possible DB-role-mapping step: the provider was created without
   `AUTO_CREATE_ROLES` (defaults FALSE) and the user has no IdC groups — a bare
   connect should succeed with PUBLIC, but verify; fixes: `ALTER IDENTITY
   PROVIDER odbcpp_idc AUTO_CREATE_ROLES TRUE;` and/or create IdC groups +
   matching DB roles `awsidc:<group>`.
3. Remaining extensions: `IdpTokenAuthPlugin` default-credentials variant,
   Azure native IdP (`JWT` / `BrowserAzureADOAuth2` → `idp_type=AzureAD`),
   DSN GUI auth-method redesign (currently only minimal option exposure),
   cross-platform builds (macOS/Linux/Windows) for the PR.
4. Per-project gates before any push/PR (needs the human's confirmation):
   control-flow check, full offline CTest, then the three-platform builds.

## 8. Repo rules that still apply

- Product/Auth/roadmap work is **PAUSED**; this branch is the only active
  workflow replacement. Local commits only — no pushes, PRs, or cloud/live
  execution without the human's confirmation. Keep the migration candidate,
  DSN GUI changes, builds and protected inputs intact.
- Preserve valid GoogleTest assertions; run the gtest control-flow check before
  expensive builds when C++ tests change; explicit braces around macro-using
  control bodies.
- Keep source/diff, config, toolchain, runtime identity, inventory, exit codes
  and test XML together; never promote an interrupted command's pass.
- Target ≥ 5 GiB free disk; delete only exact reviewed obsolete artifacts.
