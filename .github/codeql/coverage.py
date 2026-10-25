#!/usr/bin/env python3
"""Check scanner extraction evidence; never execute product or live tests."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
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

STATIC_ASSERT_SOURCE = "tests/unit/test_session_owner.cpp"
STATIC_ASSERT_BINDINGS = {
    STATIC_ASSERT_SOURCE: "7e7a00907d36a26a7e9d87ae8d142fa396d1c8e4e3b008dd0eef64be18bc4161",
    "sdk/internal/odbcpp/session/session_owner.h":
        "776d5a62bf86240671a47203ac35440b7d87c17458fd15ac4f6df1501b01b771",
}


def accepted_static_assert(runs, metrics, expected, extracted, commands, root, build):
    """Recognize one reviewed extractor limitation, never a general error budget."""
    if (runs[0]["tool"]["driver"].get("semanticVersion") != "2.27.3" or
            type(metrics.get("compiler errors")) is not int or
            type(metrics.get("syntax errors")) is not int or
            metrics.get("compiler errors") != 1 or metrics.get("syntax errors") != 0 or
            type(metrics.get("compilations")) is not int or
            metrics["compilations"] <= 1 or
            metrics.get("compilations without errors") != metrics["compilations"] - 1 or
            STATIC_ASSERT_SOURCE not in expected & extracted):
        return None
    for path, digest in STATIC_ASSERT_BINDINGS.items():
        candidate = root / path
        if not candidate.is_file() or hashlib.sha256(candidate.read_bytes()).hexdigest() != digest:
            return None
    notes = [note for invocation in runs[0]["invocations"]
             for note in invocation.get("toolExecutionNotifications", [])]
    warnings = [note for note in notes if note.get("descriptor", {}).get("id") ==
                "cpp/diagnostics/extraction-warnings"]
    if len(warnings) != 1:
        return None
    locations = warnings[0].get("locations", [])
    if len(locations) != 1:
        return None
    physical = locations[0].get("physicalLocation", {})
    if (relative_path(physical.get("artifactLocation", {}).get("uri", ""), root) != STATIC_ASSERT_SOURCE or
            physical.get("region", {}).get("startLine") != 34):
        return None
    path = str(root / STATIC_ASSERT_SOURCE)
    message = (f'Extraction failed in {path} with warning "{path}", line 34: error: static assertion failed\n'
               '  static_assert(!ExposesRawSession<SessionLease> && !ExposesPhysicalSession<SessionLease>);\n'
               '  ^\n\n')
    if warnings[0].get("message", {}).get("text") != message:
        return None
    summaries = [note.get("properties", {}).get("attributes", {}) for note in notes
                 if note.get("descriptor", {}).get("id") == "cpp/extractor/summary"]
    if (len(summaries) != 1 or summaries[0].get("extractor-failures") != 1 or
            summaries[0].get("extractor-successes") != metrics["compilations"] - 1):
        return None
    # A fresh actual GCC build precedes this check. Require its real unit object,
    # not only a requested compiler command or a generic native-build exit code.
    objects = []
    for entry in commands:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        if relative_path(str(source), root) != STATIC_ASSERT_SOURCE:
            continue
        args = entry.get("arguments") or shlex.split(entry["command"])
        if args[0] != "/usr/bin/g++" or "-c" not in args or "-o" not in args:
            return None
        index = args.index("-o") + 1
        if index >= len(args):
            return None
        obj = Path(args[index])
        if not obj.is_absolute():
            obj = Path(entry["directory"]) / obj
        if (relative_path(str(obj), build) is None or not obj.is_file() or
                obj.stat().st_size == 0):
            return None
        objects.append(obj)
    if len(objects) != 1:
        return None
    # Inspect bounded log heads/tails. A hard abort or another partial unit is
    # rejected even if its source happened to acquire a presence diagnostic.
    failures = []
    logs = root / "codeql-evidence/codeql-log/extractor"
    for log in logs.rglob("*.log"):
        with log.open("rb") as stream:
            head = stream.read(8192).decode("utf-8", "replace")
            stream.seek(max(0, log.stat().st_size - 16384))
            tail = stream.read().decode("utf-8", "replace")
        for code in re.findall(r"Extractor exiting with code (\d+)", tail):
            if int(code):
                failures.append((head, int(code), log))
    if (len(failures) != 1 or failures[0][1] != 3 or
            path not in failures[0][0]):
        return None
    return {"source": STATIC_ASSERT_SOURCE, "line": 34, "codeql_version": "2.27.3",
            "reason": "Reviewed private-member requires-expression static assertion; actual GCC assertion compiled",
            "source_sha256": dict(STATIC_ASSERT_BINDINGS),
            "object_sha256": hashlib.sha256(objects[0].read_bytes()).hexdigest(),
            "extractor_log": failures[0][2].relative_to(root).as_posix(),
            "limit": "One partial test TU; source presence is not proof of every function body's extraction"}


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
        limitation = accepted_static_assert(runs, metrics, expected, extracted, commands, root, build)
        accepted_errors = 1 if limitation else 0
        if limitation:
            result["accepted_extractor_limitations"] = [limitation]
        if metrics.get("compiler errors") != accepted_errors or metrics.get("syntax errors") != 0:
            errors.append("Actual-build extraction contains compiler/syntax errors")
        if (not isinstance(metrics.get("compilations"), (int, float)) or
                metrics["compilations"] <= 0 or
                metrics.get("compilations without errors") != metrics.get("compilations") - accepted_errors):
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
    for limitation in report.get("accepted_extractor_limitations", []):
        print("Accepted extractor limitation: " + limitation["source"] + ":" +
              str(limitation["line"]) + "; " + limitation["limit"])
    if not report["passed"]:
        raise SystemExit(1)
    found = len(set(report["expected"]) & set(report["extracted"]))
    print(f"{args.profile}: {found}/{len(report['expected'])} expected files extracted; "
          f"{len(report['exceptions_used'])} documented source-profile exceptions")


if __name__ == "__main__":
    main()
