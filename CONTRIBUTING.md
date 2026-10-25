# Contributing to ODBCPP

Use [GitHub issues](https://github.com/vahid110/odbcpp/issues) for ordinary bug
reports, questions and feature proposals. For security vulnerabilities, follow
[SECURITY.md](SECURITY.md). Describe the affected version or commit, platform,
driver manager, build options and a reproducible example with synthetic data.

Submit a focused pull request against `main` from a separate branch. Explain the
problem, resulting behavior, tests run and remaining limitations. Preserve
unrelated work and the evidence of any failures you diagnose. Include tests for
new functionality and regressions; do not weaken valid assertions to obtain a
passing result.

Follow [AGENTS.md](AGENTS.md), the architecture and ownership rules it references,
and [DEVELOPMENT_WORKFLOW.md](DEVELOPMENT_WORKFLOW.md). Use the existing C++20
style and explicit braces around control-flow bodies containing GoogleTest
assertions. Keep provider-neutral functionality in the SDK, native protocol
semantics in the backend and ODBC adaptation in the adapter.

Build and test instructions are in the
[command matrix](tools/development/COMMANDS.md). Use CMake and CTest with the
actual product and driver-manager profile. Run relevant offline regression tests
before requesting review. Product pull requests require actual macOS, Linux and
Windows builds. Live database testing requires separate operational setup;
never commit credentials or substitute a missing live result with an offline pass.

Changes require substantive review of the diff and relevant results by someone
other than the author before integration. Review correctness, ownership,
security boundaries, test assertions and the applicability of the evidence.
Maintainers resolve review findings and required CI failures before merging.

Project code is distributed under the [MIT License](LICENSE). Preserve existing
copyright and third-party notices when contributing; dependencies retain their
own licenses.
