# Redshift native IdP authentication — design & delivery plan

Branch `codex/redshift-native-idp`. Scope: the two **native IdP** login paths that
Redshift accepts at the PostgreSQL startup boundary — `IdpTokenAuthPlugin` (direct
token) and `BrowserIdcAuthPlugin` (Identity Center OAuth/PKCE). These are a
*database login material* distinct from the STS `AssumeRole` credential-source
work in PR #95; they do not depend on it.

## 1. Reference findings (source of truth: pinned native driver `56d3529`)

### 1.1 Native-IdP startup wire format

The native driver sends these as **PostgreSQL startup parameters** and sends **no
`password`** (`rslibpq.c:421-451`, constants in `rsodbc.h:1322-1337`):

| startup key       | value (native)                      | our field            |
|-------------------|-------------------------------------|----------------------|
| `idp_type`        | `"AwsIdc"` (or `"AzureAD"`)         | `idp_type`           |
| `provider_name`   | the `CREATE IDENTITY PROVIDER` name | `provider_name`      |
| `token_type`      | `"ACCESS_TOKEN"` or `"EXT_JWT"`     | `token_type`         |
| `identity_namespace` | namespace shorthand (e.g. `awsidc`) | `identity_namespace` |
| `web_identity_token` | the token (JWT)                  | `token` (secret)     |
| `user`            | the IdC-mapped DB user              | existing `user`      |

After startup, the server validates the token and returns `AuthenticationOk`
(protocol code 0). Our `PgProtocolParser::parse_auth_request` already maps code 0
to `Type::None` and `create_auth_response` returns an empty payload, so the
no-password handshake is already supported; native IdP requires only the **startup
parameters**, not a new authentication-response path.

### 1.2 Identity Center token acquisition (IdC OAuth)

`BrowserIdcAuthPlugin` uses OAuth **authorization code + PKCE** against the IdC
issuer (`https://identitycenter.amazonaws.com/ssoins-<instance-id>` →
`/oauth/authorize`, `/oauth/token`), with the Redshift managed application's
client id. Exact client id/issuer/scopes come from SSO Admin at implementation
time; `IdpTokenAuthPlugin` (direct token) needs no OAuth at all.

## 2. Regression answer

Native-token startup causes **no regression** because it is purely **additive and
opt-in**:

- The existing `password` startup path is untouched; the startup `params` map is
  only extended with the six native-IdP keys when a native-IdP material is selected.
- Ordinary password / `GetClusterCredentials` / `AssumeRole`(STS) / TLS all keep
  their exact bytes and bindings.
- A byte-for-byte startup-message test asserts the ordinary path is unchanged
  (see §5).

The correct "support now and future" shape is a **single authentication-material
seam**: `OrdinaryPassword | TemporaryDatabasePassword | NativeIdpToken`. A future
IdP (e.g. AzureAD) is a new `idp_type` value with no structural change.

## 3. Ownership split (per `AGENTS.md`)

| component | owner | file(s) |
|---|---|---|
| Provider-neutral material/classification (`Method`, `SourceKind`, `MaterialKind`, `NativeIdpAttributes`) | `sdk/` | `sdk/internal/odbcpp/auth/auth_core.{h,cpp}` |
| Native-IdP → Redshift startup wire mapping, startup params | backend | `core/database/postgres/` (parser + connection + consumer) |
| `ConnectionSettings` native-IdP fields (backend-owned spelling) | backend | `sdk/include/odbcpp/database/i_database_connection.h` |
| ODBC option parsing, `plugin_name`/`provider_name`/… , DSN GUI | adapter | `odbc/` (`setup_model.h`, `setup/`, options) |
| IdC OAuth/PKCE token acquisition (HTTP) | backend (auth) | `sdk/src/auth/aws/…` (later phase) |

The SDK contract models the token as a generic **secret + native-IdP attributes**
(strings); it never encodes Redshift wire keys or protocol framing. The backend
owns translating those attributes into the exact startup keys above.

## 4. Design

### 4.1 SDK contract (`auth_core.{h,cpp}`)

- `enum class Method { OrdinaryPassword, TemporaryDatabasePassword, NativeIdpToken }`
- `enum class SourceKind { ExternalPassword, TrustedTemporaryDbIssuer, ExternalNativeIdpToken }`
- `enum class MaterialKind { OrdinaryPassword, TemporaryDatabasePassword, NativeIdpToken }`
- New `struct NativeIdpAttributes { std::string idp_type, provider_name, token_type, identity_namespace; }`
  (all-or-nothing: empty iff none set).
- Extend `Material::create` with an optional `NativeIdpAttributes`; reject when
  `kind != NativeIdpToken` or when attributes/secret are inconsistent.
- `Material::native_idp_attributes()` accessor.

`Validity` is reused verbatim (the token has `TrustedMonotonicExpiry` for the IdC
issuer; direct-token is caller-supplied and treated like an external secret).

### 4.2 Backend startup seam (`core/database/postgres/`)

- `ConnectionSettings` gains backend-owned optional native-IdP fields (token bytes
  + `idp_type`/`provider_name`/`token_type`/`identity_namespace`), mutually
  exclusive with `password`.
- New consumer `connect_bound_native_idp_until` in
  `sdk/src/adapters/pg_credential_consumer.cpp`, mirroring the ordinary/temporary
  consumers: validate binding + TLS (`verify-full`, explicit CA), copy token into
  `ConnectionSettings`, then `connect_until`.
- `PgDatabaseConnection::connect` routes `MaterialKind::NativeIdpToken` to it.
- `GenericDatabaseConnection::connect_impl` emits the native-IdP startup keys into
  the `params` map when the native-IdP fields are present, and leaves `password`
  empty. `create_startup_message` already accepts arbitrary `params` and rejects
  embedded NULs — no parser change required beyond validation of the new keys.

### 4.3 Adapter options + DSN GUI (`odbc/`)

- New options: `plugin_name` (`IdpTokenAuthPlugin` / `BrowserIdcAuthPlugin`),
  `provider_name`, `token_type` (`ACCESS_TOKEN` / `EXT_JWT`),
  `identity_namespace`, and `token` (secret). IdC OAuth fields (`idc_region`,
  `issuer_url`, `client_display_name`, `listen_port`, `response_timeout`) are
  added in the BrowserIdc phase.
- `setup_model.h`: an authentication-method `Choice` (`Password`, `Native IdP token`,
  `Identity Center`) plus conditional native-IdP fields; `token` is cleared on
  teardown like `password` (no default persistence in the DSN file).
- `setup/setup_dialog.cpp`: render the method selector + dependent fields.

## 5. Test plan

- **Unit (SDK, offline):** `NativeIdpAttributes` equality/emptiness; `Material::create`
  accepts a native-IdP token with attributes + rejects mismatched kind/secret/missing
  attributes; `SecretBytes` limits still apply to the token.
- **Unit (backend, offline):** `create_startup_message` emits `idp_type`,
  `provider_name`, `token_type`, `identity_namespace`, `web_identity_token` in
  order and **no** `password`; **byte-for-byte regression** — the ordinary
  password startup message is identical to the pre-change golden bytes.
- **Integration (synthetic transport):** full `connect()` with a scripted transport
  that captures startup bytes, returns `AuthenticationOk` for native IdP (success)
  and asserts the password path still returns `Cleartext`/SCRAM; a wrong native-IdP
  token maps to the server's error cleanly.
- **Live (admission-gated):** obtain an IdC token, native login to
  `odbcpp-redshift-pilot`, `SELECT 1` + a catalog query. This also verifies the
  test-user password (`Sabbar12#`) by using it in the IdC browser/credential flow.
  Run only under the existing live-checkpoint admission; cluster paused afterward.

## 6. Phases & action items

1. **Contract** — `NativeIdpAttributes` + `Material` extension + unit tests.
2. **Backend seam** — `ConnectionSettings` fields, `connect_bound_native_idp_until`,
   `connect` routing, startup-param emission + regression test.
3. **Adapter + DSN GUI** — option parsing, `setup_model.h`, dialog; unit tests.
4. **Live** — admission-gated IdC login test; token acquisition (direct-token
   first, then BrowserIdc OAuth/PKCE in a follow-up).

Deliver each phase as a reviewed local commit; no push/PR/live run until the
human confirms the live checkpoint.

## 7. Live environment prerequisites (verified 2026-10-24)

The AWS-side IdC/Redshift configuration is a hard prerequisite for the live
native-IdP tests. Verified against the pilot account (eu-north-1):

- The Redshift IdC application (`odbcpp-idc-app2`, managed SSO app
  `apl-<managed-app-id>`) must have the **RedshiftConnect service integration
  enabled** (`modify-redshift-idc-application` `ServiceIntegrations.Redshift[].Connect.Authorization
  = Enabled`). Redshift installs the `redshift:connect` access scope on the
  managed app only when this integration is enabled; without it the data plane
  fails `identitystore:DescribeUser` with "no resource-based policy allows ..."
  (CloudTrail `identitystore.amazonaws.com`, caller `redshift.amazonaws.com`).
  Direct `sso-admin put-application-access-scope` on a Redshift-managed app is
  rejected ("supported through integration with its associated AWS service").
- The managed app needs: IAM authentication method ActorPolicy allowing
  `sso-oauth:CreateTokenWithIAM`/`IntrospectTokenWithIAM` for
  `redshift.amazonaws.com` + `redshift-serverless.amazonaws.com`, the
  `token-exchange`/`refresh_token` grants, the user/group assignment, and the
  DB `CREATE IDENTITY PROVIDER` row pointing at the app ARN. The IdC KMS key
  policy (multi-region replicas included) must keep the baseline provisions.
- The IdC app role (`odbcpp-idc-integration-role`) needs `sso:DescribeApplication`
  + `sso:DescribeInstance` and the Redshift trust principals; the integration
  role's own `identitystore:*` grants do **not** cover the data-plane
  `DescribeUser` (that call is made by the Redshift service, not the role).
- Native login auto-creates the DB user on first login; `AUTO_CREATE_ROLES`
  only affects group-to-role mapping. Users without IdC groups connect with
  PUBLIC only, which suffices for the connect/`SELECT 1` live assertions.
