# Redshift native IdP — hand-off (state as of 2026-10-25)

Branch `codex/redshift-native-idp` (draft PR, WIP). Worktree on the development
host: `/Users/vahidsbr/.codex/worktrees/redshift-native-idp/odbcpp`, based on
`origin/main` @ `b5063e1`. Design: [REDSHIFT_NATIVE_IDP_PLAN.md](REDSHIFT_NATIVE_IDP_PLAN.md).

Account-specific values are written as placeholders (`<account-id>`,
`ssoins-<instance-id>`, `d-<identity-store-id>`, `<idc-user-id>`,
`apl-<managed-app-id>`, `o-<org-id>`). The real values are in the private
operator handoff (`~/.local/state/odbcpp/continuation/`) and the AWS console.

## TL;DR

- The driver work is **done and offline-green**. Our wire format matches the
  official AWS drivers.
- The live login is **blocked by an AWS-side defect**: Redshift's own
  `identitystore:DescribeUser` call at login is denied. Both official AWS
  drivers (ODBC 2.x and Python `redshift_connector`) fail identically, through
  both the browser flow and the external trusted-token-issuer flow. Another AWS
  service (S3 Access Grants) succeeds against the same identity store and user.
- Next actions: get an AWS answer (re:Post question posted; a paid support case
  needs a Developer plan), or try creating the Redshift IdC application from the
  **Redshift console** instead of the CLI (untested; see section 6).

## 1. Objective

Redshift native IdP authentication for the ODBCPP driver, additive and opt-in
via the `OrdinaryPassword | TemporaryDatabasePassword | NativeIdpToken` seam:

1. `IdpTokenAuthPlugin` — caller supplies the token (`token_type` e.g.
   `ACCESS_TOKEN`, `EXT_JWT`).
2. `GetIdentityCenterAuthToken` — identity-enhanced credentials obtain the token.
3. `BrowserIdcAuthPlugin` — OAuth2 authorization code + PKCE against IAM Identity
   Center (public client registered with `sso-oidc RegisterClient`, scope
   `redshift:connect`, redirect `http://127.0.0.1:<listen_port>`).

Future IdP types (Azure etc.) are new `idp_type` values; no protocol redesign.

## 2. What is implemented (commits on this branch)

| Commit subject | Content |
|---|---|
| provider-neutral NativeIdpToken contract | SDK `Method::NativeIdpToken`, `NativeIdpAttributes` |
| backend startup seam + consumer | `ConnectionSettings::native_idp`, startup keys, token cleansing |
| adapter option resolution | `plugin_name` → `idp_type`, ODBC keywords |
| offline integration + admission-gated live test | scripted `AUTH_REQ_IDP` server |
| expose options in DSN setup model | minimal DSN exposure |
| GetIdentityCenterAuthToken owner | `IdcTokenOwner` |
| BrowserIdcAuthPlugin owner | `BrowserIdcOwner` (OAuth/PKCE, loopback listener) |
| Fix BrowserIdc PKCE challenge | unpadded base64url |
| Answer AUTH_REQ_IDP with the native token packet | protocol code 14 → `'i'` packet; optional `identity_namespace` |
| Document verified AWS-side prerequisites | plan doc section 7 |
| Send plugin_name and allow an unset user | matches official drivers: `plugin_name` always sent, `user` only when set; unset user binds the fixed internal principal `native-idp-token-identity` (never sent on the wire) |

Wire format (verified against the AWS ODBC 2.x source `rslibpq.c`/`fe-auth.c` and
`redshift_connector`): startup keys `idp_type`, `token_type`, `provider_name`,
`plugin_name`, `web_identity_token` (+ `identity_namespace` for
`IdpTokenAuthPlugin`), `user` only if set, no password; the server's
`AUTH_REQ_IDP` (14) is answered with an `'i'` packet over verified TLS.

**Offline qualification (macOS/iODBC):** `ctest --preset macos-iodbc-redshift-offline`
122/122 passed (`test_odbc_trace_reload` skipped as usual);
`it_redshift_native_idp` 3/3; gtest control-flow check 0 findings
(`python3 -B tools/development/check_changed_gtest_control_flow.py --base b5063e1 <paths>`).
**Not yet done:** Linux and Windows builds, rebase onto current `origin/main`.

Build: preset `macos-iodbc-redshift` with
`-DODBCPP_IODBC_ROOT=/opt/homebrew/opt/libiodbc -DODBCPP_REDSHIFT_NATIVE_IDC_TOKEN_TESTS=ON
-DODBCPP_AWS_SDK_INSTALL=$HOME/.local/state/odbcpp/aws-sdk-install-1.11.906`
(AWS SDK built with `BUILD_ONLY=redshift;redshift-serverless;sts;sso-oidc`).
The live binaries are **not** in the default target; build them explicitly:
`--target it_redshift_browser_idc_live it_redshift_native_idp_live it_redshift_native_idc_token_live it_redshift_native_idp`.
Offline fake-HTTP tests need `ODBCPP_AUTH_SDK_OFFLINE_FIXTURE=synthetic-fake-http-only`.

## 3. Live tests

Browser flow (needs a human to click **Allow Access**; leave
`ODBCPP_NATIVE_IDP_USER` unset to match the official drivers):

```sh
cd build-macos-iodbc-redshift
env -u ODBCPP_NATIVE_IDP_USER \
ODBCPP_NATIVE_IDC_ISSUER_URL=https://identitycenter.amazonaws.com/ssoins-<instance-id> \
ODBCPP_NATIVE_IDC_REGION=us-east-1 ODBCPP_NATIVE_IDC_LISTEN_PORT=7890 \
ODBCPP_NATIVE_IDP_HOST=<workgroup>.<account-id>.us-east-1.redshift-serverless.amazonaws.com \
ODBCPP_NATIVE_IDP_PORT=5439 ODBCPP_NATIVE_IDP_DATABASE=<db> \
ODBCPP_NATIVE_IDP_PROVIDER=<db identity provider name> ODBCPP_NATIVE_IDP_TOKEN_TYPE=ACCESS_TOKEN \
ODBCPP_NATIVE_IDP_CA=$PWD/../root.crt ./tests/it_redshift_browser_idc_live
```

Direct token (`IdpTokenAuthPlugin`, e.g. an external JWT): `it_redshift_native_idp_live`
with `ODBCPP_NATIVE_IDP_TOKEN`, `ODBCPP_NATIVE_IDP_TOKEN_TYPE=EXT_JWT`,
`ODBCPP_NATIVE_IDP_NAMESPACE=awsidc`, `ODBCPP_NATIVE_IDP_USER=<any label>` (this test
still requires a user value).

Notes:
- `ODBCPP_NATIVE_IDC_REGION` must be the region where the IdC instance lives
  (currently us-east-1 only; see section 5).
- The first login to an idle Serverless workgroup can exceed the test's 20 s
  connect timeout ("Database operation timed out"); retry once when warm.
- A temporary diagnostic print of the server error was used during diagnosis
  and is **not** committed. To print the server text, add to
  `it_redshift_browser_idc_live.cpp` before the `ASSERT_TRUE(connected)`:
  `if (!connected) std::cerr << connected.backend_error().native_state.value_or("") << " " << connected.backend_error().message << "\n";`
  (with `#include <iostream>`), and remove it again before committing.
- Cross-check with AWS's Python driver: `redshift_connector` 2.2.0,
  `credentials_provider="BrowserIdcAuthPlugin"`, `issuer_url`, `idc_region`,
  `listen_port=7890`, `ssl=True`, `sslmode="verify-full"`. Its exception text
  includes the server file/line.

## 4. The live blocker

**Symptom (every client, every flow):** SQLSTATE `28000`
`Insufficient privileges to access AWS IdC API.` (server
`identity_provider_auth.cpp` line 107, `R=AuthFail`).

**CloudTrail, every attempt, same second:**
1. `sso-oauth CreateTokenWithIAM` by `redshift.amazonaws.com` — **succeeds**
   (grant `token-exchange` for the browser flow, `jwt-bearer` for the external JWT flow).
2. `identitystore DescribeUser` by `redshift.amazonaws.com` — **AccessDenied**:
   `AWS Internal is not authorized to perform: identitystore:DescribeUser on resource:
   arn:aws:identitystore::<account-id>:identitystore/d-<identity-store-id> because no
   resource-based policy allows the identitystore:DescribeUser action`.

When a Redshift IdC application is onboarded, IdC writes the identity store
policy `aws:identitystore:idc:application-instance-policy:<redshiftidcapplication ARN>`:
principals `redshift.amazonaws.com` + `redshift-serverless.amazonaws.com`, actions
`identitystore:Describe*/Get*/List*/Search*/Is*`, conditions
`aws:SourceAccount = <account-id>`, `aws:SourceOrgID = ${aws:ResourceOrgID}` and
`ArnEquals aws:SourceArn = <redshiftidcapplication ARN>`.
The login-time call is attributed to "AWS Internal" and does not match.

**Control test (2026-10-25):** S3 Access Grants associated with the same IdC
instance works end to end for the same user (customer managed IdC app with a
Cognito trusted token issuer → `CreateTokenWithIAM` jwt-bearer → identity-enhanced
`AssumeRole` → `GetDataAccess` → `GetObject`). S3's `identitystore:DescribeUser`
on the same user **succeeds**. The policy IdC writes for S3
(`aws:identitystore:idc:application-policy:us-east-1:access-grants.s3.amazonaws.com`)
has only `aws:SourceOrgID = ${aws:ResourceOrgID}`. The only difference from the
Redshift policy is `aws:SourceAccount` + `aws:SourceArn`, which suggests that
Redshift's login-time call does not carry its source context.

**Ruled out, with evidence:**

| Suspect | Test | Result |
|---|---|---|
| Our driver | AWS ODBC 2.3.0.0 (iODBC, `BrowserIdcAuthPlugin`) and `redshift_connector` 2.2.0 | identical 28000 |
| Sign-in flow | Browser PKCE (`ACCESS_TOKEN`) and Cognito trusted token issuer (`EXT_JWT`, our driver + Python) | exchange succeeds, same DescribeUser denial |
| IdC replica region | Fresh stack in the primary region (us-east-1) | same |
| Customer managed multi-region KMS key / replication | IdC reduced to single region on an AWS owned key; fresh workgroup + IdC app | same |
| Young organization (`aws:SourceOrgID`) | S3 Access Grants passes the same condition | not the cause |
| Identity namespace mismatch, DB provider | Aligned to `awsidc`; dropped/re-created provider | same |
| Integration IAM role | Redshift never assumes it during login (only the service-linked role) | not involved |
| Scope / assignment | `redshift:connect` via service integration; user assigned; `AssignmentRequired=true` | present |
| Simba / AWS Simba 1.6.x ODBC | No IdC support in that driver (Simba's licensed 1.5.10+ has it) | not testable |

Side effect seen with the external JWT: after one denied login, further logins
with the same token returned `28000 Token credentials expired or invalid.` with no
new `CreateTokenWithIAM` in CloudTrail (looks like a cached failure).

**Public question:** https://repost.aws/questions/QUA05qzfocSZGls9xF0JDI_A
(no answers yet as of 2026-10-25). A similar unresolved thread (IdC managed from
the management account, query editor v2 "Databases couldn't be listed"):
https://repost.aws/questions/QUkOzqsv7jScuzYDjeR9pZUQ. The account is on the
Basic support plan (`aws support` → `SubscriptionRequiredException`); a technical
case needs the Developer plan. A full private case draft with request IDs exists
in the operator's notes.

## 5. Current AWS state (after the 2026-10-25 experiments)

- IdC organization instance (the account is the org's management account):
  **single region us-east-1, AWS owned key** — the human chose to keep this
  simpler setup. The replicas (eu-north-1, us-west-2) and the customer managed
  multi-region key were removed from the instance; the key itself still exists.
  To restore: `sso-admin update-instance` with the CMK, then `add-region`.
- The original eu-north-1 Redshift IdC apps (`odbcpp-idc-app2` active,
  `odbcpp-idc-app` failed onboarding) still exist but **cannot work** while the
  instance has no eu-north-1 replica. The eu-north-1 pilot workgroup is the
  ordinary password/IAM test target and must stay paused between sessions.
- All temporary experiment stacks (us-east-1 workgroups/namespaces, SGs, Redshift
  IdC apps, S3 Access Grants instance, custom IdC apps, trusted token issuers,
  Cognito pools, bucket, IAM roles) are **torn down**.
- Older cleanup candidates from earlier sessions: permission set
  `odbcpp-idc-test-ps` and its reserved role, role `odbcpp-idc-identity-enhanced`,
  policies `odbcpp-sso-iam-provisioning` and `odbcpp-idc-sts`, role
  `odbcpp-idc-integration-role` (still used by the IdC apps), KMS key alias
  `odbcpp-idc-replica`.

## 6. How to continue

1. **Watch the re:Post question / get an AWS support case** (needs a plan upgrade
   by the human). Ask whether Redshift's login-time `DescribeUser` lacks
   `aws:SourceArn`/`aws:SourceAccount`, and why the external-JWT retry is cached.
2. **Untested lead:** create the Redshift IdC application through the **Redshift
   console** (IAM Identity Center integration page) instead of
   `create-redshift-idc-application`. A re:Post answer says the console also
   creates a second, hidden app for query editor v2. Recipe: temporary us-east-1
   Serverless workgroup (base 8 RPU, public, /32 SG), console-created IdC app with
   Redshift Connect enabled, assign the user, `CREATE IDENTITY PROVIDER <name> TYPE
   AWSIDC NAMESPACE 'awsidc' APPLICATION_ARN '<managed app ARN>' IAM_ROLE '<role ARN>'`,
   run the browser live test, check CloudTrail, tear down.
3. **Option not yet run:** Redshift in a fresh **member account** of the
   organization (IdC stays in the management account). Needs a new account
   email and an `organizations:CreateAccount` grant from the human.
4. Once a live login passes:
   - Remove any diagnostic print; re-run the control-flow check and offline preset.
   - A DB role mapping step may be needed (`ALTER IDENTITY PROVIDER <name>
     AUTO_CREATE_ROLES TRUE;` or IdC groups + DB roles `awsidc:<group>`).
   - Make `ODBCPP_NATIVE_IDP_USER` optional in `it_redshift_native_idp_live` too.
   - Rebase onto current `origin/main`; macOS, Linux and Windows builds; review.
5. Later extensions: `IdpTokenAuthPlugin` default-credentials variant, Azure
   native IdP (`idp_type=AzureAD`), DSN GUI auth-method design.

## 7. Diagnostic tooling (development host)

- AWS profiles: `adbc-coordinator` (IdC/SSO admin), `adbc-provisioner`
  (administrator role: IAM, KMS, Redshift, S3, Cognito, CloudTrail lookup),
  `odbcpp-redshift-pilot` (GetCredentials + read-only Serverless). Use bash, not
  zsh, for loops that word-split `$P`-style argument variables.
- CloudTrail lags 5–15 minutes. Useful filters: event sources
  `sso-oauth.amazonaws.com` and `identitystore.amazonaws.com`, and event name
  `PutResourcePolicy` (shows the identity store policy IdC writes per app).
- AWS ODBC 2.x reference source: `~/.local/state/odbcpp/continuation/trust-store-claude-20261022/ref/aws-odbc/`.
- AWS SDK install: `~/.local/state/odbcpp/aws-sdk-install-1.11.906/`.

## 8. Repo rules that still apply

- AGENTS.md: general feature/AUTH/roadmap work is held; the human lifted the
  AUTH hold **for this branch only**. Every PR needs actual macOS, Linux and
  Windows builds before it leaves draft.
- Explicit braces around GoogleTest-macro control bodies; run the changed-diff
  control-flow check before expensive builds.
- Keep credentials in 0600 files, never in reports, commits or this public repo.
- Keep Serverless/cluster resources paused or torn down between sessions; remove
  temporary /32 SG rules.
