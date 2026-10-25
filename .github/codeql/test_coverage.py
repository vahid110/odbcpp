"""Offline scanner-control tests, not substitute product assertions."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("codeql_coverage", Path(__file__).with_name("coverage.py"))
COVERAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COVERAGE)


def sarif(paths, language="cpp", compilations=20, standard=202002):
    metrics = {"compilations": compilations, "compilations without errors": compilations,
               "compiler errors": 0, "syntax errors": 0}
    notes = [
                 {"descriptor": {"id": language + "/diagnostics/successfully-extracted-files"},
                  "locations": [{"physicalLocation": {"artifactLocation": {"uri": p}}}]}
                 for p in sorted(paths)]
    if language == "cpp":
        notes.append({"descriptor": {"id": "cpp/extractor/platform"},
                      "properties": {"attributes": {"std_version": standard}}})
    return [{"tool": {"driver": {"name": "CodeQL"}},
             "invocations": [{"executionSuccessful": True, "toolExecutionNotifications": notes}],
             "properties": {"metricResults": [
                 {"ruleId": "cpp/telemetry/extraction-metrics", "message": {"text": k}, "value": v}
                 for k, v in metrics.items()]}}]


class ActualBuildEvidence(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.build = self.root / "build-codeql"
        self.build.mkdir()
        (self.root / "sdk").mkdir()
        self.headers = ["odbcpp/database/backend_provider.h", "odbcpp/transport/transport_options.h"]
        (self.root / "sdk/contract_headers.txt").write_text("\n".join(self.headers) + "\n")
        self.generated = {"build-codeql/sdk/" + h.replace("/", "_") + ".cpp" for h in self.headers}
        self.tracked = set(COVERAGE.CRITICAL_CPP)
        self.paths = self.tracked | self.generated
        self.commands = [self.command(p) for p in sorted(self.paths)]
        self.commands.append(self.command("product/compiled_backend.cpp"))
        self.cache = dict(COVERAGE.PROFILE)
        self.write_build()

    def command(self, path):
        args = ["/usr/bin/g++", "-std=gnu++20", "-c", str(self.root / path)]
        if path == "product/compiled_backend.cpp":
            args.append("-DODBCPP_ENABLE_REDSHIFT=1")
        return {"directory": str(self.build), "file": str(self.root / path), "arguments": args}

    def write_build(self):
        (self.build / "compile_commands.json").write_text(json.dumps(self.commands))
        (self.build / "CMakeCache.txt").write_text("\n".join(k + ":STRING=" + v for k, v in self.cache.items()))

    def validate(self, runs=None):
        self.write_build()
        return COVERAGE.validate("cpp-redshift", runs or sarif(self.paths), self.tracked,
                                 self.root, self.build, {})

    def test_complete_actual_profile_and_sdk_units_pass(self):
        report = self.validate()
        self.assertTrue(report["passed"], report["errors"])
        self.assertEqual(set(report["generated_sdk_units"]), self.generated)

    def test_critical_test_cannot_disappear_from_compile_inventory(self):
        self.commands = [c for c in self.commands if not c["file"].endswith("test_backend_contract.cpp")]
        self.assertFalse(self.validate()["passed"])

    def test_compiled_file_not_extracted_fails_even_without_a_baseline_entry(self):
        runs = sarif(self.paths - {"tests/unit/test_backend_contract.cpp"})
        report = self.validate(runs)
        self.assertFalse(report["passed"])
        self.assertIn("tests/unit/test_backend_contract.cpp", report["missing"])

    def test_missing_generated_header_unit_fails(self):
        omitted = sorted(self.generated)[0]
        self.commands = [c for c in self.commands if c["file"] != str(self.root / omitted)]
        self.assertFalse(self.validate()["passed"])

    def test_native_compiler_success_does_not_hide_extractor_errors(self):
        runs = sarif(self.paths)
        runs[0]["properties"]["metricResults"][-1]["value"] = 1
        self.assertFalse(self.validate(runs)["passed"])

    def test_multiple_runs_cannot_overwrite_failed_metrics_in_either_order(self):
        bad = sarif(self.paths)
        for metric in bad[0]["properties"]["metricResults"]:
            if metric["message"]["text"] == "compiler errors":
                metric["value"] = 1
            elif metric["message"]["text"] == "compilations without errors":
                metric["value"] -= 1
        good = sarif(self.paths)
        self.assertFalse(self.validate(bad)["passed"])
        self.assertTrue(self.validate(good)["passed"])
        for runs in (bad + good, good + bad):
            with self.subTest(first_errors=runs[0]["properties"]["metricResults"][2]["value"]):
                with self.assertRaisesRegex(ValueError, "exactly one CodeQL run; found 2"):
                    self.validate(runs)

    def test_missing_metrics_fail_closed(self):
        runs = sarif(self.paths)
        runs[0]["properties"]["metricResults"] = []
        self.assertFalse(self.validate(runs)["passed"])

    def test_partial_compilation_capture_fails(self):
        self.assertFalse(self.validate(sarif(self.paths, compilations=1))["passed"])

    def test_live_registration_or_native_auth_enabled_fails(self):
        for key in ("ODBCPP_REDSHIFT_ROUTINE_SCHEMA_LIVE_TESTS", "ODBCPP_NATIVE_PROVISIONED_AUTH_TESTS"):
            with self.subTest(key=key):
                self.cache[key] = "ON"
                self.assertFalse(self.validate()["passed"])
                self.cache = dict(COVERAGE.PROFILE)

    def test_wrong_product_or_unicode_profile_fails(self):
        for key, value in (("TARGET_DATABASE", "POSTGRESQL"), ("ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE", "4")):
            with self.subTest(key=key):
                self.cache[key] = value
                self.assertFalse(self.validate()["passed"])
                self.cache = dict(COVERAGE.PROFILE)

    def test_actual_compiler_flag_not_a_requested_cmake_standard_is_proof(self):
        self.commands[0]["arguments"].remove("-std=gnu++20")
        self.commands[0]["arguments"].append("-std=gnu++17")
        self.assertFalse(self.validate()["passed"])

    def test_each_composition_compile_must_select_redshift(self):
        self.commands[-1]["arguments"].remove("-DODBCPP_ENABLE_REDSHIFT=1")
        self.commands[-1]["arguments"].append("-DODBCPP_ENABLE_POSTGRESQL=1")
        self.assertFalse(self.validate()["passed"])

    def test_external_dependency_language_level_is_not_product_profile(self):
        external = self.command("build-codeql/_deps/googletest-src/gtest-all.cc")
        external["arguments"].remove("-std=gnu++20")
        self.commands.append(external)
        self.assertTrue(self.validate()["passed"])

    def test_command_string_form_preserves_quoted_source_paths(self):
        import shlex
        for entry in self.commands:
            entry["command"] = shlex.join(entry.pop("arguments"))
        self.assertTrue(self.validate()["passed"])


class SourceInventoryEvidence(unittest.TestCase):
    def setUp(self):
        self.root = Path("/checkout")
        self.build = self.root / "build-codeql"

    def validate(self, profile, tracked, extracted, exceptions=None):
        return COVERAGE.validate(profile, sarif(extracted, COVERAGE.LANGUAGE[profile]),
                                 tracked, self.root, self.build, exceptions or {})

    def test_source_inventory_not_the_sarif_baseline_owns_the_denominator(self):
        report = self.validate("cpp-source", {"a.cpp", "new.h"}, {"a.cpp"})
        self.assertFalse(report["passed"])
        self.assertEqual(report["missing"], ["new.h"])

    def test_source_profile_requires_observed_cpp20_not_only_requested_option(self):
        for standard in (201703, None, "202002"):
            with self.subTest(standard=standard):
                report = COVERAGE.validate("cpp-source", sarif({"a.cpp"}, standard=standard),
                                           {"a.cpp"}, self.root, self.build, {})
                self.assertFalse(report["passed"])
                self.assertIn("Supplemental extraction requires actual C++20 platform telemetry",
                              report["errors"])
        runs = sarif({"a.cpp"})
        runs[0]["invocations"][0]["toolExecutionNotifications"].pop()
        self.assertFalse(COVERAGE.validate("cpp-source", runs, {"a.cpp"},
                                          self.root, self.build, {})["passed"])

    def test_mixed_source_platform_standards_cannot_hide_cpp17(self):
        runs = sarif({"a.cpp"})
        notes = runs[0]["invocations"][0]["toolExecutionNotifications"]
        notes.append({"descriptor": {"id": "cpp/extractor/platform"},
                      "properties": {"attributes": {"std_version": 201703}}})
        self.assertFalse(COVERAGE.validate("cpp-source", runs, {"a.cpp"},
                                          self.root, self.build, {})["passed"])

    def csharp(self, paths):
        runs = sarif(paths, language="cs")
        runs[0]["properties"]["metricResults"] = [
            {"ruleId": "cs/telemetry/extraction-information",
             "message": {"text": key}, "value": value}
            for key, value in [("Is extracted with build-mode set to 'none'", 1),
                               ("Number of lines of code", 100)]]
        return runs

    def test_csharp_requires_every_standalone_tracked_source(self):
        tracked = {"helper.cs", "second.cs", "embedded.ps1", "main.cpp"}
        report = COVERAGE.validate("csharp", self.csharp({"helper.cs"}), tracked,
                                   self.root, self.build, {})
        self.assertFalse(report["passed"])
        self.assertEqual(report["missing"], ["second.cs"])
        report = COVERAGE.validate("csharp", self.csharp({"helper.cs", "second.cs"}),
                                   tracked, self.root, self.build, {})
        self.assertTrue(report["passed"])

    def test_csharp_errors_cannot_hide_behind_extracted_file_diagnostic(self):
        for kind in ("cs/diagnostics/extraction-errors", "csharp/diagnostic/database-quality"):
            with self.subTest(kind=kind):
                runs = self.csharp({"helper.cs"})
                runs[0]["invocations"][0]["toolExecutionNotifications"].append(
                    {"descriptor": {"id": kind}})
                report = COVERAGE.validate("csharp", runs, {"helper.cs"},
                                           self.root, self.build, {})
                self.assertFalse(report["passed"])

    def test_csharp_rejects_absent_or_empty_code_telemetry(self):
        for metrics in ([], [{"ruleId": "cs/telemetry/extraction-information",
                             "message": {"text": "Number of lines of code"}, "value": 0}]):
            with self.subTest(metrics=metrics):
                runs = self.csharp({"helper.cs"})
                runs[0]["properties"]["metricResults"] = metrics
                self.assertFalse(COVERAGE.validate("csharp", runs, {"helper.cs"},
                                                   self.root, self.build, {})["passed"])

    def test_exception_is_exact_and_profile_scoped(self):
        exceptions = {"cpp-source": {"legacy.h": "Reviewed unused placeholder"}}
        report = self.validate("cpp-source", {"a.cpp", "legacy.h"}, {"a.cpp"}, exceptions)
        self.assertTrue(report["passed"])
        report = self.validate("cpp-source", {"a.cpp", "legacy.h", "new.h"}, {"a.cpp"}, exceptions)
        self.assertFalse(report["passed"])
        self.assertEqual(set(report["exceptions_used"]), {"legacy.h"})

    def test_all_tracked_python_and_workflows_are_required(self):
        for profile, paths in (("python", {"tool.py", ".github/codeql/coverage.py"}),
                               ("actions", {".github/workflows/new.yml", ".github/workflows/old.yaml"})):
            with self.subTest(profile=profile):
                self.assertFalse(self.validate(profile, paths, {sorted(paths)[0]})["passed"])
                self.assertTrue(self.validate(profile, paths, paths)["passed"])

    def test_empty_or_failed_invocation_is_not_a_clean_scan(self):
        with self.assertRaises(ValueError):
            COVERAGE.evidence(sarif(set()), "cpp", self.root)
        runs = sarif({"a.cpp"})
        runs[0]["invocations"][0]["executionSuccessful"] = False
        with self.assertRaises(ValueError):
            COVERAGE.evidence(runs, "cpp", self.root)

    def test_each_profile_rejects_zero_or_multiple_runs(self):
        for profile, language in COVERAGE.LANGUAGE.items():
            for runs in ([], sarif({"a"}, language) * 2):
                with self.subTest(profile=profile, count=len(runs)):
                    with self.assertRaisesRegex(ValueError, "exactly one CodeQL run"):
                        COVERAGE.validate(profile, runs, {"a"}, self.root, self.build, {})

    def test_wrong_scanner_or_language_cannot_supply_expected_evidence(self):
        runs = sarif({"a.py"}, "cpp")
        with self.assertRaisesRegex(ValueError, "No successful-extraction diagnostics"):
            COVERAGE.evidence(runs, "py", self.root)
        runs[0]["tool"]["driver"]["name"] = "Other scanner"
        with self.assertRaisesRegex(ValueError, "Expected raw CodeQL SARIF"):
            COVERAGE.evidence(runs, "cpp", self.root)

    def test_uri_normalization_and_traversal_do_not_forge_coverage(self):
        self.assertEqual(COVERAGE.relative_path("file:///checkout/a%20b.cpp", self.root), "a b.cpp")
        self.assertIsNone(COVERAGE.relative_path("%2e%2e/a.cpp", self.root))
        self.assertIsNone(COVERAGE.relative_path("/elsewhere/a.cpp", self.root))


class RawSarifInput(unittest.TestCase):
    def test_one_file_preserves_raw_input_and_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cpp.sarif"
            raw = json.dumps({"runs": sarif({"a.cpp"})}) + "\n"
            path.write_text(raw)
            files, runs = COVERAGE.read_sarif(Path(directory))
            self.assertEqual(files, [path])
            self.assertEqual(runs, json.loads(raw)["runs"])
            self.assertEqual(path.read_text(), raw)

    def test_missing_or_multiple_files_fail_without_combining_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, "exactly one raw SARIF file; found 0"):
                COVERAGE.read_sarif(root)
            bad = sarif({"a.cpp"})
            bad[0]["properties"]["metricResults"][2]["value"] = 1
            good = sarif({"a.cpp"})
            for first, second in ((bad, good), (good, bad)):
                with self.subTest(first_errors=first[0]["properties"]["metricResults"][2]["value"]):
                    contents = {"a.sarif": json.dumps({"runs": first}),
                                "z.sarif": json.dumps({"runs": second})}
                    for name, raw in contents.items():
                        (root / name).write_text(raw)
                    with self.assertRaisesRegex(ValueError, "exactly one raw SARIF file; found 2"):
                        COVERAGE.read_sarif(root)
                    for name, raw in contents.items():
                        self.assertEqual((root / name).read_text(), raw)


if __name__ == "__main__":
    unittest.main()
