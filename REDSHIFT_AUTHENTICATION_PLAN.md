# Official Redshift authentication inventory

Finite planning audit, 2026-10-03. No implementation, private reads, cloud/SQL/probes or live admission. Scope is **all top-level authentication/provider selectors in the current official native ODBC reference**, including source-only subflows; not every unrelated OAuth product. Every row remains an eventual parity obligation unless the user approves an exception. “Present” below means source/documented reference support, not qualification of our driver or every deployment/platform.

## TLS trust dependency and parked AUTH roadmap — 2026-10-07

Human-approved plan clarification: trust stores are a transport/security dependency,
not an additional authentication method. All existing method and credential-source
roadmap obligations remain intact. AUTH is parked at its independently accepted
2026-10-06 two-acquisition/two-verified-login checkpoint until the human resumes it;
this addition assigns no new AUTH implementation or checkpoint.

For each resumed HTTPS-based AWS/STS, credential-source, issuer or IdP adapter,
declare endpoint identity and trust-source selection, default/custom-CA precedence,
secure proxy compatibility where supported, and update ownership. Certificate-chain
and hostname verification must precede credential transmission; no automatic
verification bypass or insecure retry is permitted. Database-channel requirements
remain driver-owned and distinct from provider HTTPS verification. Browser-mediated
flows must document the browser's trust boundary rather than claim SDK control of
its certificate store.

Reuse the shared SDK TLS trust-source extension in
[SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md#shared-tls-trust-source-extension--2026-10-07)
where compatible. Vendor HTTP stacks retain their optional adapter boundary and
must prove equivalent policy translation; do not force AWS/vendor dependencies
into mandatory SDK core or assume a database CA bundle also qualifies an IdP.
When a method resumes, include trusted/custom-CA success, untrusted/expired peer,
wrong hostname, invalid CA configuration, cancellation/deadline, safe diagnostics
and connection/cache trust isolation in that method's finite acceptance package.
Priority is a security prerequisite of each resumed provider, not a reason to
resume parked AUTH ahead of Redshift/Excel. Existing native login evidence proves
its selected explicit-CA database channel, not every provider trust store/platform.

## Mandatory parity target and phased delivery

User clarification: all authentication methods in the supported native Redshift reference are mandatory eventual parity requirements. This is a planning and completion obligation, not an instruction to implement every method immediately. Implement each at its appropriate checkpoint based on architectural prerequisites, available qualified environments and product priority. Preserve platform/version-specific support explicitly; do not silently omit a method, treat unavailable fixtures as passed, or make every method a prerequisite for the next bounded catalog package. No deadline or immediate all-method delivery is implied.

## Reference identity and version caveats

- Audited official source remains commit `56d35297f9bee0cc31c0148581c87ca455639a39`, verified local clone HEAD, whose CHANGELOG identifies v2.2.4 (2026-09-28). [Pinned tree](https://github.com/aws/amazon-redshift-odbc-driver/tree/56d35297f9bee0cc31c0148581c87ca455639a39).
- [Official releases](https://github.com/aws/amazon-redshift-odbc-driver/releases/tag/v2.2.4) currently marks v2.2.4 latest, but the releases page links commit `32b51c3`. **The audited main pin is not established as the release commit or installed-binary source.** Before differential acceptance, resolve full tag SHA, source delta, package hashes and exact OS/architecture/library versions. Do not silently change the pin or claim source/binary equivalence.
- Current [ODBC2.x authentication table](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-authentication-ssl.html) and [option reference](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html) are mutable and incomplete relative to source: the plugin option list omits BrowserAzureADOAuth2/JwtIamAuthPlugin while other docs/source include them. Direct token documentation does not exhaust new IdpToken source subflows.
- [ODBC platform documentation](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-install.html) lists64-bit Windows/Linux/macOS and discontinued32-bit updates except urgent security patches. This does not prove every interactive plugin on each platform. Native external SAML executables are specifically Windows-only below; browser availability and headless mode need separate evidence.

## Three distinct layers

1. **AWS credential source:** access key/secret/session token, profile, environment, role, container/instance metadata, web identity, SSO or process. It authorizes AWS requests, not database login by itself.
2. **Issuance/federation:** STS SAML/web-identity/role exchange, Redshift credential API, Identity Center/native IdP token flow. An IdP password is not a database password; an AWS session token is not a Redshift DB password.
3. **Database login material:** ordinary username/password, issued temporary DB credentials, or Redshift native IdP token/startup mechanism. Names, groups/roles, resource and TLS binding remain method-specific.

## Complete top-level native ODBC method matrix

Source abbreviations refer to `src/odbc/rsodbc/` in the pinned tree. Each plugin is selected by `iam/plugins/IAMPluginFactory.cpp:7–62` and exact identifiers are declared at `iam/rs_iam_support.h:113–124`; names are **native ODBC selectors**, not Java class names.

| Method / exact native selector | Flow and important configuration | Source anchors / applicability | Our current disposition; next proof |
|---|---|---|---|
| Standard; no plugin | Explicit database UID/PWD; TLS policy | Current auth table; `rsconnect.cpp:3911–3967` distinguishes ordinary/IAM/native paths | Implemented supplied credentials; selected Serverless001/provisioned proof. Future checked-in wrong-password/fresh-valid test, not admitted here. |
| IAM explicit credentials; no plugin | `IAM`, AccessKeyID/SecretAccessKey, optional SessionToken; target/region/database and API selection | `iam/RsIamClient.cpp:120–181`, `iam/core/IAMFactory.cpp:105–118` | Native acquisition absent.009 proves external issued DB credential consumption only. Test exact source/expiry/target, no secret output. |
| IAM AWS profile; no plugin | `Profile`; static, role/source_profile, credential_source, credential_process or plugin-backed profile | `iam/core/IAMProfileCredentialsProvider.cpp:91–196,395–427`; explicit conflict/cycle/precedence branches | Absent. Synthetic protected profiles and controlled fake sources first; live role/process/SSO fixtures separately. |
| IAM instance profile / default chain; no plugin | `InstanceProfile`; or default chain when no explicit selector matches. IAM source policy must be explicit in our design | `RsIamClient.cpp:152–163,952–1035`; `IAMFactory.cpp:115–126` | Absent. Never silently contact metadata endpoints during local discovery. SDK source table below. |
| AuthProfile | Stored Redshift connection-option configuration retrieved via AWS; not a new credential algorithm | `iam/RsIamHelper.cpp:577–601`; [authentication profiles](https://docs.aws.amazon.com/redshift/latest/mgmt/connecting-with-authentication-profiles.html) | Absent. Exact option merge/precedence/identity and stale/denied/malformed profile tests; no inferred trust from profile contents. |
| `ADFS` | AD FS credential/SAML acquisition, then STS SAML and Redshift DB credentials; UID/PWD, IdP host/port, loginToRp/preferred role | `IAMAdfsCredentialsProvider.cpp`, `IAMSamlPluginCredentialsProvider.cpp:89–254` | Absent. Requires AD FS application/tenant, role trust and MFA/denial fixtures. |
| `AzureAD` | Nonbrowser Microsoft Entra/Azure acquisition; tenant/client ID/client secret plus IdP user credentials; SAML provider family | `IAMAzureCredentialsProvider.cpp`, SAML base | Absent. Tenant/admin setup and actual grant policy can prohibit password flow; do not promise bypass of MFA/conditional access. |
| `Okta` | Okta login/application -> SAML -> STS -> Redshift temporary credentials; IdP host/app ID/name | `IAMOktaCredentialsProvider.cpp`, SAML base | Absent. Tenant/application/role fixture, MFA/denial and format change tests. |
| `Ping` | PingFederate -> SAML -> STS -> Redshift temporary credentials; IdP host/port, optional partner_spid | `IAMPingCredentialsProvider.cpp`, SAML base | Absent. Dedicated provider admin/test environment needed; HTML/SAML failure handling. |
| `BrowserAzureAD` | Interactive Azure browser authentication, SAML-family AWS credential acquisition | `IAMBrowserAzureCredentialsProvider.cpp`, SAML base | Absent. Desktop browser/redirect, cancellation, MFA and headless refusal; not equivalent to OAuth2 selector. |
| `BrowserSAML` | Interactive generic SAML portal, including configured Okta/Ping/ADFS; login_url, listen_port, IdP_Response_Timeout | `IAMBrowserSamlCredentialsProvider.cpp`, SAML base | Absent. Shares SAML exchange, but each portal/configuration needs real compatibility proof. |
| `JWT` | Supplied web_identity_token for native Redshift IdP login, provider_name; not necessarily a DB-password issuance path | **Actual routing** `rsconnect.cpp:3930–3935` selects native AzureAD-type flow; helper `RsIamHelper.cpp:269–303` distinguishes token vs UID/PWD | Absent. Native startup/protocol token acceptance and configured CREATE IDENTITY PROVIDER prerequisite must be separately designed/proved. Do not infer flow solely from factory class inheritance. |
| `JwtIamAuthPlugin` | Supplied JWT -> STS AssumeRoleWithWebIdentity -> selected Redshift DB credential API; role_arn/role_session_name | `plugins/JwtIamAuthPlugin.cpp:31–64`, `IAMJwtPluginCredentialsProvider.cpp:37–65` and STS request path | Absent. OIDC role trust/audience/issuer and exact target/derived principal proof; distinct from JWT native flow. |
| `BrowserAzureADOAuth2` | Interactive Azure OAuth2 obtains native IdP token; tenant/client ID/scope/provider_name and redirect settings | `rsconnect.cpp:3946–3951`, `RsIamClient.cpp:185–205`, `IAMBrowserAzureOAuth2CredentialsProvider.cpp:67–91` | Absent. Azure native IdP integration, OAuth callback/security/cancellation and configured DB role mapping. Not BrowserAzureAD SAML. |
| `BrowserIdcAuthPlugin` | Identity Center browser authorization code/token flow; idc_region, issuer_url, client display name/response timeout/listen port | `BrowserIdcAuthPlugin.cpp:207–277,309–315`; native selector at `rsconnect.cpp:3880–3901` | Absent. Client registration, authorization scopes/admin assignment, PKCE/state/loopback/cancel, token/cache isolation and native login. |
| `IdpTokenAuthPlugin`: direct token | Supplied `token` and `token_type`: ACCESS_TOKEN or EXT_JWT; native Identity Center/connected IdP flow | `IdpTokenAuthPlugin.cpp:25–58`; [official token/driver identifier table](https://docs.aws.amazon.com/redshift/latest/mgmt/redshift-iam-access-control-idp-connect-oauth.html) | Absent. Trusted token source, audience/issuer/type and exact DB identity binding; no local JWT-decoding-as-authentication claim. |
| `IdpTokenAuthPlugin`: identity-enhanced credentials | Explicit AWS AccessKeyID/SecretAccessKey/SessionToken -> provisioned or Serverless GetIdentityCenterAuthToken -> native subject token | `IdpTokenAuthPlugin.cpp:40–55,74–80,368–480` | Source-present beyond simple docs token table. Requires supported identity-enhanced AWS context/admin mappings; separate service permission/live fixture. |
| `IdpTokenAuthPlugin`: default AWS credentials | No explicit AWS credentials/direct token -> SDK default source -> GetIdentityCenterAuthToken -> native token | Same source flow selection and GetSubjectToken paths | Source-present; exact issuer provenance/source constraints must be explicit, not implicit ambient credential discovery. |
| External SAML provider executable | `plugin_name` full executable path; profile/provider arguments -> external assertion -> federation | Factory fallback at62; `IAMExternalCredentialsProvider.cpp:190–209`: Windows CreateProcess; non-Windows returns empty string. [Windows external credentials documentation](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-authentication-ssl.html) | Absent. Windows-specific eventual compatibility. Executing arbitrary configured programs is a trust/process boundary; no automatic POSIX port or in-process plugin loading implied. |

The direct plugins are all factory branches; external executable fallback is included. LDAP/Kerberos, arbitrary JVM-only classes, query-editor OAuth and unrelated service IdPs are not added to the native ODBC inventory without an evidenced selector. TLS certificate verification is mandatory channel policy, not an interchangeable database-auth method.

Identifier caveat: the documentation table's `BasicJwtCredentialsProvider` wording is not an established native `plugin_name` alias. The pinned factory accepts `JWT` (case-insensitive comparison), dispatches `CreateJwtPlugin`, and separately accepts `JwtIamAuthPlugin`; `web_identity_token` is the configuration key, not a plugin identifier (`rs_iam_support.h:92,120,122`, factory44–51). Treat the documentation label/version correspondence as unresolved rather than accepting an invented alias. Similarly BrowserAzureADOAuth2 is an evidenced source/routing selector despite its omission from the option-reference plugin list. AuthProfile remains configuration retrieval, not an additional identity method; the Windows executable row is a provider boundary, not a new database protocol.

## AWS credential-source subinventory

Pinned `IAMProfileCredentialsProvider.cpp` has plugin, role_arn/source_profile, role_arn/credential_source, credential_process and static profile branches; source-profile cycles reject. credential_source supports Environment, Ec2InstanceMetadata and EcsContainer. credential_source+source_profile conflicts; credential_source requires role_arn; source takes precedence over credential_process in that role branch. These are observed behaviors needing explicit parity tests, not recommended defaults.

Pinned source README says last-tested AWS SDK C++1.11.895, **not a verified dependency lock**. The [SDK1.11.895 default-chain source](https://raw.githubusercontent.com/aws/aws-sdk-cpp/1.11.895/src/aws-cpp-sdk-core/source/auth/AWSCredentialsProviderChain.cpp) no-argument constructor used by the ODBC factory selects environment, ProfileCredentialsProvider, STS web identity, SSO, then conditional general HTTP/container or instance metadata. Its configured constructor also adds LoginCredentialsProvider; do not claim that extra branch is used by the ODBC no-argument call. Profile internals can assume roles/run processes; SDK-specific supported providers/order require the actual binary's SDK pin, not current generic docs. Credentials sourced through SSO are AWS credentials and remain distinct from BrowserIdc/native subject-token database login.

## Database issuance and versions/platforms

- `RsIamClient.cpp:168–181`: Serverless uses GetCredentials; provisioned groupFederation uses GetClusterCredentialsWithIAM; otherwise GetClusterCredentials. These APIs have different requested/returned principal, groups/AutoCreate and refresh fields. They cannot be one interchangeable “password factory.” The prior admission-design report compares exact API fields.
- Pinned CHANGELOG: JwtIamAuthPlugin introduced2.0.0.8 when previous JWT provider was repurposed; Identity Center plugins2.0.0.9, BrowserIdc removed2.0.0.11 then supported again2.1.3; Identity Center cache added later. Current [native federation docs](https://docs.aws.amazon.com/redshift/latest/mgmt/redshift-iam-access-control-native-idp.html) give BrowserIdc minimum ODBC2.1.3 vs JDBC2.1.0.30, and IdpToken ODBC2.0.0.9 vs JDBC2.1.0.19. Do not extrapolate a uniform minimum across all methods.
-2.2.2 adds IdpToken identity-enhanced credentials;2.2.3 fixes long temporary passwords/resolved-credential echo and JWT/OAuth AutoCreate;2.2.4 fixes IAM CA UTF-8 and STS timeout/endpoint honoring. Older versions have consequential auth differences. These source changelog facts need package correspondence before differential testing.
- [JDBC2.x authentication](https://docs.aws.amazon.com/redshift/latest/mgmt/jdbc20-configure-authentication-ssl.html) uses fully qualified Java plugin classes and JVM TLS/provider infrastructure. The two official IDC examples use `com.amazon.redshift.plugin.IdpTokenAuthPlugin` / `com.amazon.redshift.plugin.BrowserIdcAuthPlugin`; native ODBC uses the short names. Java or Python support never establishes native ODBC support. This is a native inventory, not a promised JDBC implementation.
- ODBC1.x is a separate legacy product: [migration documentation](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-odbc10-driver-differences.html) explicitly changes options/trust behavior. This audit does not establish every1.x auth method or a blanket deprecation date. Preserve version-specific compatibility research rather than silently port1.x SSL/logging defaults. AuthType UI choice is documented Windows DSN-specific; native connection-string/non-Windows inference differs. External executable support is Windows-only; all other rows are present in common2.x source, while exact platform/architecture/interactive acceptance remains unqualified.

## Evidence and completion obligations

Our public M2/live reports establish selected password+verifiedTLS001/provisioned and external IAM009 principal/scalar/invalid-password evidence only. `it_redshift_real.cpp:913–1043` contains the external IAM and new ordinary-password recovery tests; the new case is a future proof unless separately admitted. CredentialContext is local generation/expiry/revocation, not a native issuer or provenance verifier. No row above is completed by parser-key acceptance or green offline CI.

For **every** row: explicit options/aliases/defaults/precedence/GUI/platform disposition; issuer/source and target identity; success/invalid/expired/denied/refresh-reconnect; cancellation/one deadline; concurrent/cache isolation; redaction/secret cleanup; real ordinary-user principal and privilege observation; official-driver differential result; platform/Driver Manager/crypto packaging gates. Source-family shared tests reduce effort but cannot replace a configured IdP acceptance run. Unknown or absent environments block that row; they do not become skipped green qualification.

Blunt blockers: all-method completion needs external IdP tenants/app registrations/role trusts, administrators, MFA/conditional-access users, browser-capable desktop agents, protected CI secrets and controlled network/proxy endpoints. Native identity flows may create/map database principals and roles; fixture authority is separate from the current pilot's SQL allowance. No such configuration, grants or paid admission is authorized by this inventory. An “all supported methods” commitment is considerably larger than one successful password/IAM window.

This planning checkpoint does not pause the implementation timer. No source/binary mismatch is hidden, no row silently dropped, and no insecure SSL/executable default copied just to emulate upstream.
