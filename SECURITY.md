# Security policy

## Reporting a vulnerability

Please report suspected security vulnerabilities through
[GitHub private vulnerability reporting](https://github.com/vahid110/odbcpp/security/advisories/new).
Do not put credentials, private connection details, or an unpublished exploit in
a public issue. Ordinary bugs and feature requests belong in the
[issue tracker](https://github.com/vahid110/odbcpp/issues).

Include the affected commit or release, platform, driver manager, build options,
expected and observed behavior, and a minimal reproduction using synthetic data.
Sanitize logs and connection strings. If the issue concerns a dependency, include
its actual installed or packaged version and how the affected code is reached.

We aim to acknowledge private reports within 14 days. We will investigate the
impact, coordinate a fix and disclosure with the reporter, and credit the reporter
unless they prefer anonymity. Critical confirmed issues receive priority.

## Maintained versions

ODBCPP is currently under development. Security fixes target the current `main`
branch; historical development snapshots do not receive a separate backport
guarantee. Before distributing a stable release, its supported versions and
security-update policy will be recorded here. A passing build or test run is not
a statement that a development snapshot is production-ready.

## Security expectations

See [SECURITY_MODEL.md](SECURITY_MODEL.md) for the driver and SDK security model.
Keep certificate and hostname verification enabled and protect credentials and
DSN files using the operating system's access controls. Report authentication,
TLS, input-validation, memory-safety, logging and packaging issues through the
private channel above.

Static analysis, dependency review, tests and independent code review provide
different evidence. Badge claims must describe the checks or practices actually
verified; they do not guarantee that the software has no vulnerabilities.
