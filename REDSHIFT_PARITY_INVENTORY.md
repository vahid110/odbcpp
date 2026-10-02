# Redshift parity inventory — first bounded pass

Recorded 2026-10-02. Preparation only: no Redshift runtime changes, AWS access,
credentials, live tests, driver builds, CI runs or qualification claims in this
pass. This is a starting inventory, not the full RS5 inventory or an exhaustive
upstream audit. All eight requirements in [REDSHIFT_FEATURE_PLAN.md](REDSHIFT_FEATURE_PLAN.md)
remain in scope; no exception or omission is approved here.

The integration owner owns follow-up evidence and estimates. P0 means M2
assessment; P1 means a required frozen M3 beta path; P2 means planned RP1/G14
breadth; P3 means a separate advanced-feature decision. An out-of-beta item is
still pending parity work. These priorities preserve the feature plan and do not
expand the active SDK/MySQL proof.

## Reference identity and evidence rules

The official repository is [aws/amazon-redshift-odbc-driver](https://github.com/aws/amazon-redshift-odbc-driver).
Read-only `git ls-remote`, detached checkout, `git show`, `git rev-parse`, and
`git diff` inspection on 2026-10-02 established:

| Reference | Recorded identity | Meaning |
|---|---|---|
| Pinned source / observed remote HEAD | `56d35297f9bee0cc31c0148581c87ca455639a39`, 2026-09-28 16:47:29 -0700, `Update README.md` | Same commit as the feature plan; all source links below use this SHA |
| Version declaration | [version.txt](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/version.txt): `2.2.4 0` | Source version declaration, not installed-binary verification |
| Official `v2.2.4` tag | [commit `32b51c33be9ee7ac56c5f4f2bbb5cc0f6831c77f`](https://github.com/aws/amazon-redshift-odbc-driver/commit/32b51c33be9ee7ac56c5f4f2bbb5cc0f6831c77f), 2026-09-28 22:51:54 +0000 | Different SHA; its version.txt declares `2.2.3 0` despite the tag name. Tag-to-pin diff changes only CHANGELOG.md, README.md and version.txt, with no runtime source delta |
| Release evidence | [Pinned changelog](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/CHANGELOG.md) | Leads for behavior changes and tests; release notes alone do not establish every implementation branch |
| Mutable AWS documentation | [Options](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html), [authentication](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-authentication-ssl.html), [metadata API](https://docs.aws.amazon.com/redshift/latest/mgmt/discovering-metadata-driver-api.html) | Read 2026-10-02; not SHA-pinned and may diverge from source |

Before differential execution, record the reference binary version/build/hash,
platform, Driver Manager/version/SQLWCHAR width, server identity/version and
non-secret effective settings. Source/binary correspondence remains unverified.
Simba requires its own pinned binary/documentation; no Simba evidence was obtained.

**S** below means directly observed source statements/branches at the pin.
**D** means official documentation or release-note observation.
**R** means proposed requirement/test inference, not observed execution.
All our implementation dispositions are **unaudited** in this pass; source
presence does not mean we provide the feature or that upstream's path works.
Tests below are proposed IDs, not executed results. No implementation was copied.

## First feature/configuration matrix

| ID / user area | Evidence and bounded observation | Applicability / priority | Disposition and next test contract |
|---|---|---|---|
| F-RS1-01 / data sharing | **S:** [connection defaults](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsodbc.h#L1403) initialize `DatabaseMetadataCurrentDbOnly=1`; [catalog branches](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rscatalog.cpp#L610) select SHOW or retained catalog paths. **D:** [metadata API guidance](https://docs.aws.amazon.com/redshift/latest/mgmt/discovering-metadata-driver-api.html) explains cross-database schema identity and SQLTables enumeration. | Connected metadata; provisioned/Serverless, shared and external objects need independent evidence. P0, P1 when discovery workflow requires it, otherwise P2. | Unaudited. R/T-RS1: current/all database, producer/consumer/denied user, same schema/table names in different catalogs, unavailable objects, Unicode and patterns. Include Tables, Columns, PrimaryKeys, ForeignKeys, SpecialColumns, Statistics, ColumnPrivileges, TablePrivileges, Procedures and ProcedureColumns; trace each separately. |
| F-RS2-01 / credential selection and IAM | **S:** [RsIamClient::Connect](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/iam/RsIamClient.cpp#L109) selects plugin, profile, instance profile, static or default credential-chain providers, then provisioned credentials, group-federation credentials or Serverless credentials; native IdP routes differ. Exact role/profile chains are unreviewed. | Method/endpoint dependent. P0; initial database credentials over verified TLS are provisional P1, a required alternative must be promoted before beta. Remaining methods P2. | Unaudited. R/T-RS2-A: usable login, invalid/expired credentials, principal identity, reconnect/renewal contract, AWS/STS endpoint and region selection, proxy, absolute deadline/cancel and secret canaries. Do not access local credential stores for preparation. |
| F-RS2-02 / federation and tokens | **S:** [plugin factory](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/iam/plugins/IAMPluginFactory.cpp#L7) and [names](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/iam/rs_iam_support.h#L113) select ADFS, AzureAD, BrowserAzureAD, BrowserAzureADOAuth2, BrowserSAML, Ping, Okta, JWT, JwtIamAuthPlugin, IdpTokenAuthPlugin and BrowserIdcAuthPlugin; unknown names select an external-provider path. | Browser, external module and IdP configuration/platform dependent; factory routing is not provider qualification. P0/P2, P1 if required. | Unaudited. R/T-RS2-B: separate per-provider option/lifecycle matrix, token type/expiry, denied IAM combinations, browser timeout/cancel, proxy and identity isolation. External native loading requires a separate design against trusted static SDK extension scope; no universal plugin framework is approved. |
| F-RS2-03 / credential lifecycle | **S:** [RsIamHelper](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/iam/RsIamHelper.cpp#L106) has cache eligibility/expiry paths and [cache key construction](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/iam/RsIamHelper.cpp#L550) combines secret-bearing settings. **R:** This is a security review item, not an instruction to reproduce that mechanism. | Authentication generation and cache policy; P0 review, P1 selected method safety, P2 breadth. | Unaudited. R/T-RS2-C: immutable principal/generation binding, expiration boundary, concurrent refresh, no cross-principal reuse or secret keys/logs. Full cache/provider synchronization remains unreviewed. |
| F-RS3-01 / declare/fetch settings | **S:** [keys/default batch](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsodbc.h#L1165) and [resolver](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsconnect.cpp#L590) define `UseDeclareFetch`/`UDF`, `Fetch` and batch default 100 when enabled with nonpositive size. Enabled mode clears streaming and CSC. [Parsing](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsconnect.cpp#L2451) normalizes negative batch values; overflow/malformed cases still need tracing. | DSN/string resolution, forward-only eligibility. P0 design; P2 unless required workload promotes P1. | Unaudited. R/T-RS3-A: omitted/0/negative/positive/overflow/malformed sizes, aliases, order/duplicate key precedence, DSN versus string, streaming/CSC combinations and read-back. Freeze our validation contract explicitly. |
| F-RS3-02 / execution modes | **S:** [portal gate](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rslibpq.c#L1265) requires enabled mode, unprepared execution, no bind parameters/catalog/function call and forward-only cursor. [Eligibility helper](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rslibpq.c#L5365) handles SELECT/WITH and rejects specified modifying/INTO/multi-statement cases. **D:** [streaming option](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html) still says streaming replaces old declare/fetch. | Do not infer prepared/parameterized batching from the option name or changelog. Streaming is a distinct mode. P0/P2 or promoted P1. | Unaudited. R/T-RS3-B: direct versus prepared/parameterized, eligible/ineligible SQL, empty/exact/multiple batches, NULL/chunks, autocommit/transactions, multiple results, cancel/deadline, early close/disconnect, refill failure, memory/cleanup. Verify actual portal semantics before naming it a SQL DECLARE cursor contract. |
| F-RS4-01 / explicit behavior | **D:** [v2.2.0 notes](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/CHANGELOG.md#L39) identify zero-row DML return changes, conversion warnings/errors, attribute scoping and interval changes. **S:** result/conversion and option modules are candidate paths; only option mappings were sampled here. | ODBC version, type and operation dependent. P0 inventory, P1 frozen surface, P2 full coverage. | Unaudited. Link behavior rows below; enumerate conversion/state/transaction/error-recovery branches in finite follow-ups. Release fixes are leads, not a complete test suite. |
| F-RS5-01 / exports, claims and descriptors | **S:** [Linux export list](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/exports.linux.list) contains modern, wide and legacy names; [driver info](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsdrvinfo.cpp#L247) is an entry point for SQLGetInfo claims. Export presence does not prove functionality. | Compare Windows .def, macOS list, build branches and A/W versus DM aliases. P0 full inventory pending; P1 selected APIs, P2 remainder. | Unaudited. R/T-RS5-A: exact symbols, SQLGetFunctions/SQLGetInfo versus behavior, descriptor fields/defaults/ranges, environment/connection/statement scopes, handle sequences, output length/preservation. No full ODBC 3.8 claim. |
| F-RS5-02 / configuration surface | **S:** [property definitions](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsodbc.h#L1138) and resolver expose fetch/refcursor, metadata, table-type, Boolean/Unicode and further options. **D:** [official option reference](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html) lists TLS, auth, proxies, compression, timeouts, logging, sizes and aliases. | Platform-specific GUI/DSN versus string behavior needs individual rows. P0; P1 selected settings, P2 breadth. | Unaudited. R/T-RS5-B: inventory every source key/alias, effective default/range, precedence and actual effect. First seeds: BoolsAsChar, UseUnicode, EnableTableTypes, DatabaseMetadataCurrentDbOnly, compression, ReadOnly, LoginTimeout, MaxVarcharSize/MaxLongVarcharSize, TLS/CA, proxy and logging. No key silently accepted without its effect. |
| F-RS6-01 / SHOW alternatives | **S:** [show_discovery reader](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsutil.c#L17612), [catalog V4 gate](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rscatalog.cpp#L610), [V5 query definitions](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsMetadataAPIHelper.cpp#L178) and [proxy selection](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsMetadataServerProxy.cpp#L246) distinguish retained catalog, per-object SHOW and database-batch SHOW. V5 includes a driver-token usability gate. | Server-reported discovery capability, token and ordinary-user permissions; not just a server-version string. P0/P1 selected APIs/P2 breadth. | Unaudited. R/T-RS6: absent/malformed/V4/V5 capability; absent/valid/invalid token; output schema normalization, current/all DB, patterns/metadata-ID, escaping, case, order and privilege failures. Invalid token selecting per-object SHOW is different from fallback after execution failure. Do not silently convert permission/transport/malformed output to catalog fallback or empty success. |
| F-RS7-01 / legacy APIs | **S:** [statement option mapping](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsoptions.cpp#L301) routes supported old options to attributes and uses HYC00 for unmapped options; [transactions](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rstransaction.cpp#L14) share SQLTransact/SQLEndTran machinery. Remaining old exports are inventory leads. | Negotiated ODBC 2/3 versions, DM mapping and platform/width. P0, P1 required callers, P2 breadth. | Unaudited. R/T-RS7: Alloc/Free legacy handles, connect/stmt options, SetParam/BindParam/ParamOptions, ExtendedFetch, ColAttributes and SQLError mapping. Test direct exports and Windows DM/unixODBC/iODBC UTF-16/UCS-4 separately; record which layer supplied a mapping. |
| F-RS8-01 / S3 bulk transfer | **D:** [v2.1.10 notes](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/CHANGELOG.md#L126) say unsupported client/stdin COPY/UNLOAD code was removed. Official [COPY](https://docs.aws.amazon.com/redshift/latest/dg/r_COPY.html) describes database loading; [UNLOAD](https://docs.aws.amazon.com/redshift/latest/dg/r_UNLOAD.html) describes query export to S3. **R:** These do not establish transparent staging through fetch APIs. | Separate server SQL capability and proposed driver delivery feature; IAM/S3/KMS and deployment dependent. P3 design approval, no implementation commitment. | Unaudited design. R/T-RS8 after approval: explicit eligibility/mode, semantic/type fidelity, transactions/row counts/diagnostics, least privilege, server-side encryption, cleanup, cancel, retry/idempotency, cost. Spectrum external querying is a separate feature; no automatic rerouting is authorized. |

## First observable-behavior matrix

| ID / parent | Observation versus proposed expectation | Planned comparison / dependencies |
|---|---|---|
| B-RS1-01 / F-RS1-01 | **D:** AWS documents SQLTables catalog and schema enumeration conventions and populating TABLE_CAT during cross-database schema discovery. **R:** Output identity, NULL fields and caller-visible objects must be reconciled with ODBC semantics. [Reference](https://docs.aws.amazon.com/redshift/latest/mgmt/discovering-metadata-driver-api.html). | T-RS1 with NULL versus empty inputs, enumeration sentinels, ordinary users and two catalogs with colliding names. Real sharing/privilege evidence required. |
| B-RS2-01 / F-RS2-01 | **S:** Native IdC plugins reject IAM-enabled combinations in [Connect](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/iam/RsIamClient.cpp#L129). **R:** Preserve safe diagnostic class, no credentials sent on locally invalid combinations and deterministic reconnect policy. | T-RS2-A/B: local negative harness then selected real provider. Exact diagnostic chain not yet traced. |
| B-RS3-01 / F-RS3-01 | **S:** Enabled declare/fetch clears streaming/CSC and fills batch default. **R:** Effective mode must be visible/documented consistently regardless of input order. [Resolver](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsconnect.cpp#L590). | T-RS3-A offline parsing/precedence; T-RS3-B real executions and memory. Source/document conflict remains open, not an approved omission. |
| B-RS3-02 / F-RS3-02 | **S:** Portal eligibility excludes prepared/bound/catalog/function/scrollable paths. **R:** An enabled option alone cannot justify a bounded-memory claim for those paths. [Gate](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rslibpq.c#L1265). | Test each excluded path as well as direct SELECT/WITH; measure complete result lifetime and failed refill cleanup on Redshift. |
| B-RS4-01 / F-RS4-01 | **D:** v2.2.0 notes specify SQL_NO_DATA for zero-row INSERT/UPDATE/DELETE and more precise conversion states. **R:** Trace implementation before writing exact assertions. [Changelog](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/CHANGELOG.md#L39). | T-RS4: direct/prepared zero/nonzero DML, SQLRowCount/MoreResults, numeric overflow, truncation/fraction loss, interval/date/time, NULL/binary/chunk retrieval; compare return codes, SQLSTATE order, lengths and recovery. |
| B-RS4-02 / F-RS5-02 | **S:** SQLDriverConnect output has a pre-authentication credential emission branch; auth-resolved credentials are omitted. **D:** v2.2.3 notes report long-password and resolved-identity fixes. **R:** Omitting resolved credentials does not prove a password-free completed string. [Output branch](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsconnect.cpp#L851). | T-RS5-B secret-canary output/log tests, long input, truncation/length and preservation; specification/security decision before matching any secret-bearing output. |
| B-RS6-01 / F-RS6-01 | **S:** SHOW execution errors propagate through the sampled [proxy path](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsMetadataServerProxy.cpp#L272). **R:** Complete fallback and postprocessor behavior still needs branch audit. | T-RS6: permission denial, malformed result names/types, capability/token errors, timeout/transport failure, buffer widths; never treat a failed discovery as verified empty metadata. |
| B-RS7-01 / F-RS7-01 | **S:** Supported statement options map to modern attributes; unmapped options use SQL_ERROR/HYC00. [Mapping](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsoptions.cpp#L325). **R:** Complete option set and negotiated-version behavior remain open. | T-RS7: modern/legacy equivalent calls, invalid handles/options, width-sensitive values, diagnostic APIs and output preservation, direct versus DM traces. |

These seed rows do not cover all attribute values, conversions, diagnostics,
state transitions or metadata APIs. No row is verified match, scheduled
implementation or user-approved exception yet. Split each family into atomic
items with inputs/defaults/aliases/ranges, preconditions, expected outputs,
existing-test links, owner/estimate and evidence at the next bounded pass.

## Security and SDK constraints

[SDK_ARCHITECTURE.md](SDK_ARCHITECTURE.md) requires provider-owned defaults/static
policy, session-owned authentication/protocol/recovery and normalized catalog,
schema/cell/error results. Keep Redshift specialization inside the backend;
shared ODBC orchestration must not gain Redshift branches or native identifiers.
Streaming design must fit the bounded result-delivery seam; a flag cannot remove
hard resource ceilings. [SECURITY_MODEL.md](SECURITY_MODEL.md) requires verified
peer/hostname evidence, method-specific channel policy, bounded resources,
absolute deadlines, retirement on uncertain synchronization, credential
isolation and secret-safe logging/caches.

**D/security decision pending:** AWS's option page documents SSLMode's default
as verify-ca and insecure/downgrade alternatives. Our verified-host policy must
remain intact; source defaults and all transport paths still need tracing.
Do not reproduce unsafe behavior to match upstream. Any incompatible alternative
needs explicit specification/security evidence and the user's exception decision.
[Official SSLMode reference](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html).

## Coverage ledger and live-dependent unknowns

Reviewed selectively, not file-complete: version.txt; tag-to-pin diff;
CHANGELOG.md recent entries; rsodbc.h keys/defaults; rsconnect.cpp selected parse,
DSN/exclusivity/output branches; rslibpq.c portal gate/eligibility; rscatalog.cpp
SHOW gate samples; rsutil.c show_discovery reader; rsMetadataAPIHelper.cpp query
constants/V5/token helpers; rsMetadataServerProxy.cpp selection/error samples;
IAMPluginFactory.cpp dispatch; rs_iam_support.h provider names; RsIamClient.cpp
Connect and credential-request leads; RsIamHelper.cpp cache/expiry/key samples;
rsoptions.cpp legacy statement mapping; rstransaction.cpp common path; Linux
export list and rsdrvinfo.cpp entry-point discovery.

Located upstream test leads: [portal management](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/unittest/portal_management_test.cpp)
(selected names inspected), metadata_api_helper_test.cpp,
rsMetadataAPIPostProcessorTest.cpp, rscatalog_escape_test.cpp,
parse_connect_string_test.cpp, rsoptions_test.cpp, conversion_test_common_sql2c.cpp,
tls_policy_test.cpp, browser_idc_auth_test.cpp, idp_token_auth_plugin_test.cpp,
iam_instance_profile_test.cpp, iam_jwt_sts_timeout_test.cpp, iam_curl_cafile_test.cpp
and sqlcancel_unit_test.cpp. These are upstream coverage leads; none were run or
adopted as evidence for this driver.

Unreviewed: full export/build platform comparison; all descriptors/attributes/
GetInfo/GetFunctions values; full configuration/GUI registry/INI precedence and
aliases; complete authentication provider internals and credential-chain role
behavior; libpq startup/TLS/compression/error/cancel branches; data conversions,
bindings, arrays, SQLGetData/MoreResults; complete transactions/cursor lifecycle;
all metadata SQL/postprocessors and server capability branches; older changelog
coverage; full upstream test assertions and our corresponding tests. Packaging
or cryptographic dependencies are not qualified by observing their source.

Live-dependent unknowns blocking the affected compatibility claim:

- Provisioned versus Serverless endpoint identity/version, SHOW capability/token
  availability and cross-database/shared/external catalog behavior.
- Actual ordinary-user/denied visibility and privileges for every advertised
  catalog path; normalized output/schema/type fidelity and application discovery.
- Selected IAM/SSO/IdC provider success, rejection, expiry/refresh and principal
  binding; custom endpoint, region/CNAME/NLB/proxy behavior and TLS identity.
- Portal/streaming transaction semantics, early-close/cancel and mid-batch failure
  recovery, result memory/limits, prepared-path behavior and multiple results.
- Pinned packaged reference binary/source correspondence, actual DM mappings,
  macOS Unicode widths and requested Windows/macOS application acceptance.
- RS8 S3/KMS permissions, data/transaction fidelity, cleanup/idempotency and cost;
  these require approved design plus resources, not implicit cloud provisioning.

[RELEASE_PLAN.md](RELEASE_PLAN.md) G1/G6/G7/G8/G9b/G14 remain open for their
required real evidence. PostgreSQL simulations, source inspection and absent-
endpoint builds cannot close them. Future shared changes retain PostgreSQL,
common SDK/security and platform regression gates; no such change occurred here.

## Next bounded research package — RP-INV-02

Propose one offline package: finish the **fetch-mode decision inventory** for
F-RS3-01/02 and B-RS3-01/02, with F-RS5 option cross-links. Review only relevant
resolver/DSN/setup controls, rslibpq portal execution/refill/close, result/cancel/
transaction integration and upstream portal/streaming assertions at the same pin.
Produce atomic rows for defaults/aliases/ranges/precedence, supported versus
excluded execution paths, lifecycle/diagnostics and a finite differential fixture
list. Separate source facts from expected ODBC/SDK/security requirements and
record the streaming documentation discrepancy as a decision item.

Stop after those rows and one review checkpoint; leave remaining source modules
in this ledger. No runtime work, credential access, live claims, exhaustive parity
expansion or changes to the active SDK/MySQL proof. Estimate implementation and
live execution separately at M2; user-required bounded workloads may promote the
selected fetch path to P1, while other RS1–RS7 work remains planned for RP1.
