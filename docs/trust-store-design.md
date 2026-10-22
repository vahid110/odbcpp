# TLS trust-store design: operating-system stores and custom CA files

Status: implemented on `codex/trust-store` (baseline `origin/main`
`efe7af379469303415f7ccce3a5d8d626a92a886`, 2026-10-22). Local macOS evidence
only; Windows, Linux, AWS-LC and live Redshift qualification are pending.
Extends the [shared TLS trust-source plan](../SDK_PRODUCT_PLAN.md#shared-tls-trust-source-extension--2026-10-07)
under [SECURITY_MODEL.md](../SECURITY_MODEL.md) and
[CRYPTO_PROVIDER_PLAN.md](../CRYPTO_PROVIDER_PLAN.md).

## 1. Problem

Before this change, verified TLS trusted exactly one of: `SSLCAFILE`,
`SSLCADIR`, or OpenSSL's compiled default verify paths
(`SSL_CTX_set_default_verify_paths`). The last one is *not* the operating
system trust store:

- Windows: OpenSSL's default directory (`OPENSSLDIR\certs`, normally an
  unrelated build prefix) is empty; the Windows certificate store, enterprise
  roots, Group Policy roots and the `Disallowed` store are never consulted.
- macOS: Homebrew/bundled OpenSSL reads its own `cert.pem` snapshot; Keychain
  anchors, administrator/user trust settings and explicit distrust are ignored.
- Linux: a system OpenSSL normally points at the distro bundle, but a
  bundled/static OpenSSL points at its build prefix, so a relocated package
  silently trusts nothing (fails closed, but unusably).

## 2. Reference behavior

Evidence classes: **S** = source read, **D** = vendor documentation,
**R** = observed runtime (none collected for vendors in this change).

### AWS native Redshift ODBC 2.x

Source pins inspected: `56d35297f9bee0cc31c0148581c87ca455639a39`
(`version.txt` 2.2.4, 2026-09-28, the repository's recorded pin) and current
remote HEAD `868e9b0d035f5e219890c2de424b9d29d130f300` (`version.txt` 2.3.0,
2026-10-19). Neither changed trust-source handling between the pins.

- **S** `src/odbc/rsodbc/rslibpq.c`: when SSL is not `disable`, the driver
  always passes `ssldefaultrootcert=<driver dir>/root.crt`; `TrustStore`/`TS`
  becomes `sslrootcert`, else `CaFile`, else `CaPath/root.crt`.
- **S** `src/pgclient/src/interfaces/libpq/fe-secure.c`: the bundled
  `root.crt` is loaded *additively* if it exists; `sslrootcert` (or
  `~/.redshift/root.crt`) is loaded only if `stat()` succeeds — a missing
  custom file is silently skipped and the bundled root alone is used.
- **S** No Windows CryptoAPI, macOS Security framework or distro-bundle code
  path exists in either pin. Trust is OpenSSL/AWS-LC file based only.
- **S/D** `SSLMode` default became `verify-full` in 2.3.0, but an unset
  `SSLMode` with a hostname mismatch only logs a warning; `verify-ca` and
  `require` remain selectable. ([options page][aws-opts], `CHANGELOG.md` 2.3.0)
- **D** [Options page][aws-opts] documents `CaFile` only for IAM HTTPS on
  Linux and does not document `TrustStore`; [SSL page][aws-ssl] tells Windows
  users to save the Amazon bundle as `root.crt` in the driver `lib` folder.

### Simba Amazon Redshift ODBC (vendor-authored guides)

- 1.4.16 guide (July 2020, AWS-hosted,
  SHA-256 `f04d9a74908d4a06179f3398477b93f5c27374b8baa11977a54c13918e8537ff`)
  and Simba 1.5.14 guide (July 2024, Posit-hosted vendor PDF,
  SHA-256 `af3dd4c62dfaa973e16412ad1ac7303daa7a74d788dc47d9e4bc8c61b67b74bc`).
- **D** `UseSystemTrustStore` (default 1): use a CA from the system trust
  store, or 0 to use a specified `.pem` file. "Only available on Windows."
- **D** `SSLCertPath`: full path of the root certificate file; default is the
  folder containing the driver DLL.
- **D** `CheckCertRevocation` (Windows store only, default 0) and
  `AllowSelfSignedServerCert` (default 0, system store only).
- No Simba binary was run; no black-box compatibility is claimed.
  JDBC `SSLTrustStore`/Java keystores are unrelated and out of scope.

### Comparison and decisions

| Concern | AWS native 2.2.4/2.3.0 | Simba 1.4/1.5 | ODBCPP (this change) |
|---|---|---|---|
| OS store | None | Windows only | Windows, macOS, Linux/Unix |
| Custom CA | `TrustStore`/`CaFile`/`CaPath` file, additive with bundled `root.crt` | `SSLCertPath` file | `SSLCAFILE` file or `SSLCADIR` OpenSSL hashed dir, exclusive |
| Missing custom file | Silently ignored, bundled root still used | Not documented | **Error**, no fallback |
| Default | Bundled `root.crt` | System store (Windows) | System store |
| Hostname | verify-full default, warn-only when unset | per SSL mode | Always enforced (DNS and IP) |
| Insecure toggles | `require`, `verify-ca`, `ssl_insecure` (IdP) | `AllowSelfSignedServerCert` | None |
| Revocation | CRL file if `sslcrl` | `CheckCertRevocation` (Windows) | Not checked (boundary, §6) |

We adopt Simba's `UseSystemTrustStore` key name because it is an established
Redshift ODBC key with the same meaning, and we keep the existing ODBCPP
`SSLCAFILE`/`SSLCADIR`. We deliberately do **not** copy the native driver's
silent missing-file fallback, additive bundled root, warn-only hostname
mismatch, `AllowSelfSignedServerCert` or non-verifying SSL modes. Native
`TrustStore`/`CaFile`/`CaPath` and Simba `SSLCertPath` aliases are not added in
this change: their semantics differ (additive, directory+`root.crt`, IAM-only)
and silent reinterpretation would be unsafe; adding exact aliases later is a
separate parity row.

## 3. Options, defaults and precedence

| Key | Values | Meaning |
|---|---|---|
| `SSL` | `1/0`, `true/false`, `yes/no`, `on/off` | Existing. Omitted means verified TLS. |
| `UseSystemTrustStore` | same Boolean tokens | New. `1`: platform trust store. `0`: custom CA required. Omitted: derived. |
| `SSLCAFILE` | path | Existing. PEM bundle; exclusively trusted. |
| `SSLCADIR` | path | Existing. OpenSSL hashed (`c_rehash`) directory; exclusively trusted. |

Keys merge with the existing precedence (connection string > DSN > driver
section > built-in). An empty value (`SSLCAFILE=`) is the same as absent, so a
connection string can clear a DSN's custom CA. After merging, the
provider-neutral SDK selector (`select_tls_trust_source`) decides:

| `SSL` | `UseSystemTrustStore` | `SSLCAFILE` / `SSLCADIR` | Result |
|---|---|---|---|
| off | absent | none | plaintext (existing) |
| off | set | any | **error**: trust settings require TLS |
| off | any | set | **error** (existing) |
| on | any | both set | **error** (existing) |
| on | absent | none | **System** (new default; was OpenSSL default paths) |
| on | absent | file / dir | **Custom file / directory** (unchanged) |
| on | `1` | none | **System** |
| on | `1` | file or dir | **error**: conflicting trust sources |
| on | `0` | file / dir | **Custom file / directory** |
| on | `0` | none | **error**: custom trust requested without a CA |
| any | malformed Boolean | any | **error** |

There is no combined system+custom mode. Every common need is met by one
source: public endpoints use the OS store; private CAs or corporate TLS
inspection use a concatenated PEM bundle (or an OS-installed enterprise root).
A union mode would make a typo in a private-CA path quietly broaden trust to
the whole public root set, which is exactly the failure this design forbids.

Compatibility decision: connections that set neither CA key now use the
platform trust store instead of OpenSSL's compiled default paths. On Linux
with a system OpenSSL this is the same distro bundle; on macOS it replaces a
Homebrew/bundle snapshot with Keychain trust; on Windows it replaces an
effectively empty store. Anyone relying on `SSL_CERT_FILE` on macOS/Windows
must now set `SSLCAFILE`. Custom-CA behavior is unchanged except for clearer,
earlier errors (§5).

## 4. Trust source semantics per platform

All modes keep: TLS 1.2 minimum, TLS 1.3 maximum, `SSL_VERIFY_PEER`, a server
certificate required, and the SDK's own hostname/IP check
(`X509_check_host`/`X509_check_ip_asc`, SNI only for DNS names) after the
handshake. The platform verifier evaluates the chain and purpose; the SDK
always checks the name, so name semantics are identical across platforms.

### Windows (CryptoAPI, `crypt32`)

OpenSSL's chain verification is replaced for the handshake by a verify
callback that copies the peer leaf and peer-supplied intermediates into a
memory store and calls `CertGetCertificateChain` on the current-user chain
engine (`HCCE_CURRENT_USER`) with requested usage server authentication, then
`CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, AUTHTYPE_SERVER)`.

- The current-user engine's Root/CA logical stores include LocalMachine,
  enterprise and Group Policy roots plus CurrentUser roots, and honor the
  `Disallowed` store and locally installed CTLs. Services running under a
  service account see that account's user store plus machine roots.
- Explicit distrust, missing server-auth EKU, non-CA intermediates, expiry
  and untrusted roots fail. Revocation is not requested.
- No network retrieval: `CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL`,
  `CERT_CHAIN_DISABLE_AIA` and `CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE`.
  CryptoAPI only bounds URL fetches per URL (`dwUrlRetrievalTimeout`), never in
  aggregate, so fetching could not honor the connection deadline. As on macOS,
  the server must send its intermediates. The root must also already be in the
  local Root store: Windows preinstalls only a small set of roots and normally
  downloads the others on demand through automatic root update, which is now
  disabled for driver verification. A fresh or locked-down image without
  Amazon Root CA 1 or Starfield Services Root CA G2 fails closed. Preinstall
  the roots (Group Policy, enterprise deployment, `certutil -generateSSTFromWU`
  plus import, or an earlier OS fetch), or use `SSLCAFILE`.

### macOS (Security framework)

The same verify callback builds `SecCertificateRef`s from DER, evaluates
`SecTrustCreateWithCertificates` with `SecPolicyCreateSSL(true, NULL)` and
`SecTrustEvaluateWithError` (macOS 10.14+). This honors system roots, System
and login keychain anchors, admin/user trust settings (including purpose
restrictions and explicit "Never Trust") and Apple's distrust lists. Network
fetching is disabled (`SecTrustSetNetworkFetchAllowed(false)`); servers must
send intermediates, as Redshift does. No keychain is modified and no anchor is imported into OpenSSL.

### Linux and other Unix (distro bundles)

There is no OS verification API, so the SDK loads the distribution's
*extracted trusted-anchor* bundle into a per-context `X509_STORE` and lets
OpenSSL verify. First match wins; if a selected source fails to load, the
connection fails (no fallthrough):

1. `SSL_CERT_FILE` / `SSL_CERT_DIR` environment variables, if non-empty
   (the conventional administrator override, also honored by OpenSSL/Go).
   `SSL_CERT_DIR` may list several directories separated by `:`. As in
   OpenSSL, both are ignored in set-user/group-ID processes
   (`secure_getenv` on glibc, a real/effective ID check elsewhere).
2. Files: `/etc/ssl/certs/ca-certificates.crt` (Debian/Ubuntu/Alpine/Arch),
   `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem` (RHEL/Fedora 7+),
   `/etc/pki/tls/certs/ca-bundle.crt` (RHEL 6), `/etc/ssl/ca-bundle.pem`
   (SUSE), `/etc/pki/tls/cacert.pem`, `/etc/ssl/cert.pem` (Alpine/BSD),
   `/usr/local/etc/ssl/cert.pem`, `/usr/local/share/certs/ca-root-nss.crt` (FreeBSD).
3. Directories: `/etc/ssl/certs`, `/etc/pki/tls/certs`.
4. None found: error "no operating system trust store found"; never the
   OpenSSL compiled default.

p11-kit/`update-ca-trust` and `update-ca-certificates` exclude distrusted
anchors and keep only server-auth anchors in these extracted bundles, so
administrator distrust is respected through the distro tooling.

## 5. Custom CA semantics

- `SSLCAFILE`: must be a readable regular file (symlinks followed) of at most
  16 MiB containing at least one PEM `CERTIFICATE` or `TRUSTED CERTIFICATE`
  (CRL blocks are loaded too). Missing, directory, empty, unreadable,
  oversized, malformed or certificate-free input fails before the TLS
  handshake, and the provider's error detail is kept in the diagnostic.
- `SSLCADIR`: must be an existing directory; OpenSSL hash-named lookup.
  A directory with no matching hash fails at verification.
- Paths are UTF-8 (ODBC wide input is converted); spaces and non-ASCII paths
  are supported. Files are opened by the provider (`BIO_new_file`, which
  converts UTF-8 to a wide path on Windows); type checks use the wide
  `std::filesystem` spelling.
- Every certificate in a custom source is a trust anchor candidate exactly as
  in libpq/OpenSSL: intermediates in the bundle do not make a chain valid
  without its root (no `X509_V_FLAG_PARTIAL_CHAIN`); intermediate CA flags
  and the server-auth purpose are checked by OpenSSL. A self-signed leaf
  placed in the bundle is an explicit administrator pin and is honored.
- Custom sources replace, never extend, system trust.

## 6. Lifetime, reuse and limits

- Each new connection creates a fresh provider context from the resolved
  settings: CA files are re-read and the OS store is consulted live, so
  rotation applies to the next connection. Established sessions are never
  re-verified.
- Deadlines: native evaluation is a synchronous OS call that cannot be
  preempted. Without network retrieval it depends only on local data, but its
  duration is not guaranteed. The verifier therefore rejects a trusted verdict
  that arrives at or after the caller's original deadline. Both the sync and
  async transports convert a handshake success, or a TLS verification failure,
  observed after that deadline into a timeout (`TLS handshake timeout` /
  `TLS handshake deadline expired`) and never publish late TLS. The same
  post-handshake check covers custom/Linux OpenSSL verification, including
  hashed-directory lookups.
- The cancel-request sibling (`verified_sibling`) snapshots custom/Linux
  store objects already loaded; in Windows/macOS system mode it re-evaluates
  against the live OS store and fails closed if trust was removed meanwhile.
- No process-global `SSL_CTX`, store or environment is modified.
- Diagnostics carry the trust-source category (`system`, `custom_file`,
  `custom_directory`) and a bounded failure reason; certificate bytes,
  private keys and provider objects never leave the security adapter.
- Not provided: revocation checking (OCSP/CRL), client certificates,
  keystore/password formats, `AllowSelfSigned`-style bypasses, combined mode.

## 7. Ownership

| Component | Owner file | Reuse rationale | Allowed dependencies | Proof |
|---|---|---|---|---|
| Trust selection contract (`select_tls_trust_source`) | `sdk/include/odbcpp/database/i_database_connection.h` | Provider-neutral; every backend/session resolves the same way | std only (constexpr) | `test_tls_trust_resolution.cpp` |
| Connection option input | `sdk/include/odbcpp/database/backend_provider.h` | Normalized option passed by adapters | none new | `test_tls_trust_resolution.cpp` |
| TLS client trust modes | `sdk/src/security/openssl_tls_client.cpp`, `sdk/internal/odbcpp/security/tls_client.h` | Single private provider adapter (OpenSSL API, AWS-LC compatible calls) | OpenSSL, platform adapter | `test_tls_trust_store.cpp`, `test_tls_client.cpp`, `tests/security/test_verified_tls.py` |
| OS trust adapters | `sdk/internal/odbcpp/security/platform_trust.h`, `sdk/src/security/platform_trust.cpp` | Private; OS APIs never cross the security boundary | Security.framework/CoreFoundation, crypt32 | `test_platform_trust.cpp` |
| Transport deadline hand-off and late-verdict rejection | `sdk/src/transport/tls_transport.cpp`, `async_tls_transport.cpp` | Shared transports | security contract | `test_transport_deadlines.cpp` (delayed-verdict FIFO controls), `test_async_tls_transport.cpp` |
| PostgreSQL/Redshift resolution | `core/database/postgres/pg_backend_provider.cpp` | Backend owns defaults/messages | SDK resolver | `test_tls_trust_resolution.cpp`, `test_backend_provider.cpp` |
| Session validation | `core/database/generic_database_connection.cpp` | Backend session | SDK resolver | `test_transport_deadlines.cpp` |
| ODBC keyword parsing and `trust_source` log field | `odbc/odbc_handles.cpp` | Adapter only | SDK options | `test_api_validation.cpp` |
| Windows DSN model/UI | `odbc/setup_model.h`, `odbc/setup/*` | Adapter only | Win32 | `test_setup_model.cpp` (portable model); dialog compile pending Windows |
| Build linkage | `CMakeLists.txt` (`APPLE`: Security, CoreFoundation; `WIN32`: existing crypt32) | Artifact link closure | platform frameworks | macOS link of `libodbcpp.dylib` and tests |

## 8. Provider/platform boundaries

- Implemented against the OpenSSL 3 API used by the production profile. The
  calls used (`SSL_CTX_set_cert_verify_callback`, `X509_STORE_CTX_get0_cert`,
  `X509_STORE_CTX_get0_untrusted`, `i2d_X509`, `PEM_X509_INFO_read_bio`) also
  exist in AWS-LC and LibreSSL, but AWS-LC remains an unqualified profile and
  LibreSSL is not a supported profile; neither was compiled here.
- macOS: compiled and unit-tested locally (iODBC). Windows: code compiled only
  by CI/Windows owner (pending). Linux: bundle selection logic is
  unit-tested on macOS through an injected file system view; native Linux
  execution is pending CI.
- Positive OS-store acceptance of a real public certificate requires network
  access or a store modification and is therefore deferred to live/platform
  qualification; local tests prove rejection of private CAs by the OS store
  and native chain/purpose evaluation with test-injected anchors.

[aws-opts]: https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html
[aws-ssl]: https://docs.aws.amazon.com/redshift/latest/mgmt/connecting-ssl-support.html
