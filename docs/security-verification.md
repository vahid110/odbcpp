# Security verification evidence

Security checks provide evidence within a defined scope. They do not certify that
all code is secure. The CodeQL badge reports a workflow result; the OpenSSF Best
Practices badge reports the project's assessed development practices. An
in-progress badge is not a Passing badge.

## Reachable Git history: 2026-10-25

An isolated repository mirror captured 282 local and fetched remote refs and
1,590 reachable commits. Gitleaks 8.30.1 scanned all 1,589 commits containing added
text, including merge-introduced changes (`--all --full-history -m`); the remaining
commit had no added text. Approximately 39.92 MB was scanned. The official scanner
archive was checked against its published SHA-256 checksum.

Default detector rules were used. No repository ignore file was supplied, and
`gitleaks:allow` comments were ignored. Output was fully redacted; the retained
finding report contains only rule, file, line and commit metadata. Four matches
received independent source review:

- One deterministic test-harness marker was not an external authentication key.
- One match was architecture prose, not credential material.
- Two connection-string values were intentional local PostgreSQL test credentials.
  Their decoded values matched the same-commit loopback-only fixture setup,
  including the Unicode/escaping case.

No leaked production credential was established by these findings. No detector
allowlist, alert dismissal, provider login or credential rotation was used to make
the scan pass. A later scan of the 15 security-change commits from
`1ff3fd6453881ea79225ab01ff12ff00172429b7` through
`d96404bef8cf2ad3e932b8a70b270c8cea32f67c` scanned approximately 152.88 KB and found
no additional matches.

This is not an exhaustive absence-of-secrets claim. Unreachable/deleted refs,
external forks, host credential stores, private inputs and release artifacts were
outside this history inventory. Pattern detectors have false negatives and
built-in allowlists. Test credentials must never be reused for an exposed service.
Report suspected exposed credentials through the [private security reporting
channel](../SECURITY.md), without copying secret values into public issues.

## Static analysis and build coverage

[The CodeQL workflow](../.github/workflows/codeql.yml) defines separate analyses
for Python, GitHub Actions, an actual compiled Redshift C++20 profile, and a
supplemental C++ source-only profile. [Its configuration](../.github/codeql/config.yml)
retains the security-extended queries and explicit local-input threat models.
[Coverage checks](../.github/codeql/coverage.py) compare extracted source with
tracked inventory and record profile boundaries before publishing results.

A source-only analysis does not prove a platform builds. A compiled Linux profile
does not establish Windows, macOS, every optional provider or every dormant
backend's runtime behavior. Separate platform builds and tests remain required.
A pull-request analysis may be diff-informed: its alert count is not a complete
new baseline for `main`. Full default-branch analyses and review of individual
findings are necessary before claiming remediation.

CodeQL success means the configured analysis completed; it does not mean zero
alerts. Original failed or timed-out scans remain part of the evidence. Suppressed
or excluded source must have a specific documented reason, never merely a desire
for a green badge.

## Dependencies and test evidence

Dependency review must use actual source/archive identities and transitive
components, not just top-level package names or an empty Dependabot dashboard.
Assess whether affected APIs are compiled and reachable; distinguish a latent
vulnerable dependency from a demonstrated driver exploit. System vendor backports
require the full package revision and support policy. See the
[build profiles](../BUILD_SYSTEM.md) for source and runtime identity requirements.

GoogleTest/CTest results, sanitizers, driver-manager checks and real database tests
provide separate evidence from static analysis. Preserve original failures,
configuration and source identity; report skipped and unavailable tests explicitly.
The [development workflow](../DEVELOPMENT_WORKFLOW.md) defines these requirements.
