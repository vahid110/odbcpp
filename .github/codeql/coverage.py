#!/usr/bin/env python3
"""Check scanner extraction evidence; never execute product or live tests."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
from urllib.parse import unquote, urlparse


LANGUAGE = {"cpp-redshift": "cpp", "cpp-source": "cpp",
            "python": "py", "actions": "actions"}
CRITICAL_CPP = {
    "core/database/postgres/pg_protocol_parser.cpp",
    "odbc/odbc_api.cpp",
    "product/compiled_backend.cpp",
    "sdk/src/security/openssl_tls_client.cpp",
    "sdk/src/session/session_owner.cpp",
    "tests/unit/test_backend_contract.cpp",
}
PROFILE = {
    "TARGET_DATABASE": "REDSHIFT",
    "BUILD_TESTING": "ON",
    "BUILD_EXAMPLES": "OFF",
    "CMAKE_BUILD_TYPE": "Debug",
    "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
    "ODBCPP_CRYPTO_PROVIDER": "OPENSSL",
    "ODBCPP_CRYPTO_LINKAGE": "SYSTEM_SHARED",
    "ODBC_DRIVER_MANAGER_FLAVOR": "UNIXODBC",
    "ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE": "2",
    "ODBCPP_EXPECT_DM_SQLWCHAR_SIZE": "2",
    "ODBCPP_CRYPTO_TLS_PROOF": "OFF",
    "ODBCPP_NATIVE_PROVISIONED_AUTH_TESTS": "OFF",
}


def relative_path(value, root):
    """Normalize SARIF URIs and compile-command paths to the checkout."""
    value = unquote(value)
    if value.startswith("file:"):
        value = urlparse(value).path
    path = Path(value)
    if path.is_absolute():
        try:
            return path.relative_to(root).as_posix()
        except ValueError:
            return None
    if ".." in path.parts:
        return None
    return path.as_posix()


def evidence(runs, language, root):
    if len(runs) != 1:
        raise ValueError(f"Expected exactly one CodeQL run; found {len(runs)}")
    extracted, metrics, diagnostics = set(), {}, Counter()
    for run in runs:
        if run.get("tool", {}).get("driver", {}).get("name") != "CodeQL":
            raise ValueError("Expected raw CodeQL SARIF")
        invocations = run.get("invocations", [])
        if not invocations or any(i.get("executionSuccessful") is not True
                                  for i in invocations):
            raise ValueError("CodeQL invocation is missing or incomplete")
        for invocation in invocations:
            for note in invocation.get("toolExecutionNotifications", []):
                kind = note.get("descriptor", {}).get("id", "")
                diagnostics[kind] += 1
                if kind == language + "/diagnostics/successfully-extracted-files":
                    for location in note.get("locations", []):
                        uri = location.get("physicalLocation", {}).get(
                            "artifactLocation", {}).get("uri", "")
                        path = relative_path(uri, root)
                        if path:
                            extracted.add(path)
        for metric in run.get("properties", {}).get("metricResults", []):
            if metric.get("ruleId") == "cpp/telemetry/extraction-metrics":
                metrics[metric.get("message", {}).get("text")] = metric.get("value")
    if not extracted:
        raise ValueError("No successful-extraction diagnostics; file counts alone are insufficient")
    return extracted, metrics, dict(diagnostics)


def read_sarif(directory):
    files = sorted(directory.glob("*.sarif"))
    if len(files) != 1:
        raise ValueError(f"Expected exactly one raw SARIF file; found {len(files)}")
    return files, json.loads(files[0].read_text())["runs"]


def cache_values(text):
    result = {}
    for line in text.splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line:
            name, value = line.split("=", 1)
            result[name.split(":", 1)[0]] = value
    return result


def compiled_sources(commands, tracked, root, generated):
    expected, errors = set(), []
    backend_records = 0
    for entry in commands:
        file = Path(entry["file"])
        if not file.is_absolute():
            file = Path(entry["directory"]) / file
        path = relative_path(str(file), root)
        if path not in generated and not (path in tracked and path.endswith(".cpp")):
            continue  # External dependency compilation has its own language level.
        expected.add(path)
        args = entry.get("arguments") or shlex.split(entry["command"])
        if not any(a in ("-std=c++20", "-std=gnu++20") for a in args):
            errors.append("Missing actual C++20 compiler flag: " + path)
        if path == "product/compiled_backend.cpp":
            backend_records += 1
            if "-DODBCPP_ENABLE_REDSHIFT=1" not in args or any(
                    a.startswith("-DODBCPP_ENABLE_POSTGRESQL") for a in args):
                errors.append("Product composition is not REDSHIFT: " + path)
    if backend_records < 2:
        errors.append("Both core and driver composition compiler records are required")
    return expected, errors


def validate(profile, runs, tracked, root, build, exceptions):
    extracted, metrics, diagnostics = evidence(runs, LANGUAGE[profile], root)
    result = {"profile": profile, "extracted": sorted(extracted),
              "extraction_metrics": metrics, "diagnostics": diagnostics,
              "errors": [], "exceptions_used": {},
              "codeql_versions": [run["tool"]["driver"].get("semanticVersion") or
                                  run["tool"]["driver"].get("version") for run in runs]}
    errors = result["errors"]
    if profile == "cpp-redshift":
        cache = cache_values((build / "CMakeCache.txt").read_text())
        result["cmake_profile"] = {k: cache.get(k) for k in PROFILE}
        for key, value in PROFILE.items():
            if cache.get(key) != value:
                errors.append("Unexpected CMake profile: " + key)
        for key, value in cache.items():
            if key.startswith("ODBCPP_") and key.endswith("LIVE_TESTS") and value != "OFF":
                errors.append("Live-test option enabled: " + key)
        headers = (root / "sdk/contract_headers.txt").read_text().splitlines()
        headers = [h.strip() for h in headers if h.strip() and not h.startswith("#")]
        relative_build = build.relative_to(root).as_posix()
        generated = {relative_build + "/sdk/" + h.replace("/", "_") + ".cpp"
                     for h in headers}
        commands = json.loads((build / "compile_commands.json").read_text())
        expected, command_errors = compiled_sources(commands, tracked, root, generated)
        errors.extend(command_errors)
        errors.extend("Required source not compiled: " + p
                      for p in sorted((CRITICAL_CPP | generated) - expected))
        for name in ("compilations", "compilations without errors", "compiler errors", "syntax errors"):
            if not isinstance(metrics.get(name), (int, float)):
                errors.append("Missing independent extraction metric: " + name)
        if metrics.get("compiler errors") != 0 or metrics.get("syntax errors") != 0:
            errors.append("Actual-build extraction contains compiler/syntax errors")
        if (not metrics.get("compilations") or
                metrics.get("compilations without errors") != metrics.get("compilations")):
            errors.append("Actual-build extraction has incomplete/error compilations")
        if metrics.get("compilations", 0) < len(expected):
            errors.append("Fewer extracted compilations than owned source units")
        result["generated_sdk_units"] = sorted(generated)
        result["compile_commands_sha256"] = hashlib.sha256(
            (build / "compile_commands.json").read_bytes()).hexdigest()
        result["cmake_cache_sha256"] = hashlib.sha256(
            (build / "CMakeCache.txt").read_bytes()).hexdigest()
        result["scope"] = "Actual Linux REDSHIFT compile graph; no runtime/platform/native AWS acceptance"
    elif profile == "cpp-source":
        expected = {p for p in tracked if p.endswith((".cpp", ".h"))}
        standards = [note.get("properties", {}).get("attributes", {}).get("std_version")
                     for invocation in runs[0]["invocations"]
                     for note in invocation.get("toolExecutionNotifications", [])
                     if note.get("descriptor", {}).get("id") == "cpp/extractor/platform"]
        result["cpp_standard_versions"] = standards
        if not standards or any(type(version) is not int or version < 202002
                                for version in standards):
            errors.append("Supplemental extraction requires actual C++20 platform telemetry")
        result["scope"] = "Supplemental inferred source extraction; quality metrics are reported, not actual-build proof"
    elif profile == "python":
        expected = {p for p in tracked if p.endswith(".py")}
        result["scope"] = "All tracked Python source; no runtime or secret-history proof"
    else:
        expected = {p for p in tracked if p.startswith(".github/workflows/")
                    and p.endswith((".yml", ".yaml"))}
        result["scope"] = "All tracked Actions workflows; not standalone shell/PowerShell/CMake analysis"
    missing = expected - extracted
    allowed = exceptions.get(profile, {})
    result["expected"] = sorted(expected)
    result["missing"] = sorted(missing)
    result["exceptions_used"] = {p: allowed[p] for p in sorted(missing & allowed.keys())}
    errors.extend("Source was not successfully extracted: " + p for p in sorted(missing - allowed.keys()))
    result["passed"] = not errors
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=LANGUAGE, required=True)
    args = parser.parse_args()
    # This workflow helper accepts no caller-selected input or output paths.
    root = Path(__file__).resolve().parents[2]
    output = root / "codeql-evidence/coverage.json"
    report = {"profile": args.profile, "passed": False}
    try:
        tracked = set(subprocess.check_output(["git", "ls-files", "-z"], cwd=root,
                                              text=True).split("\0")) - {""}
        files, runs = read_sarif(root / "codeql-results")
        exceptions = json.loads((root / ".github/codeql/coverage-exceptions.json").read_text())
        report = validate(args.profile, runs, tracked, root, root / "build-codeql", exceptions)
        report["source_head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root,
                                                        text=True).strip()
        report["sarif_sha256"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        report["errors"] = [str(error)]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    for error in report.get("errors", []):
        print(error)
    if not report["passed"]:
        raise SystemExit(1)
    found = len(set(report["expected"]) & set(report["extracted"]))
    print(f"{args.profile}: {found}/{len(report['expected'])} expected files extracted; "
          f"{len(report['exceptions_used'])} documented source-profile exceptions")


if __name__ == "__main__":
    main()
