"""Offline controls for the manual build's tracing boundary."""

from pathlib import Path
import re
import subprocess
import unittest
import xml.etree.ElementTree as ET


WORKFLOW = Path(__file__).resolve().parents[1] / "workflows/codeql.yml"
CONFIGURE = "Configure the actual REDSHIFT C++20 profile"
INITIALIZE = "Initialize CodeQL"
BUILD = "Compile product, SDK header units and existing test sources without running tests"
ANALYZE = "Analyze and retain full diagnostics before publication"
CSHARP_BUILD = "Compile the original standalone C# helper as a library without executing it"
DM_CONTEXT = "Compile the supplemental manager header context without linking or execution"


def library_sources(text):
    project = ET.fromstring(text)
    if (project.findtext("./PropertyGroup/OutputType") != "Library" or
            project.findtext("./PropertyGroup/EnableDefaultCompileItems") != "false"):
        raise ValueError("Expected an explicit library compilation")
    return {item.attrib["Include"].removeprefix("../../")
            for item in project.findall("./ItemGroup/Compile")}


def tracing_boundary(text):
    blocks = re.split(r"(?m)^      - name: ", text)[1:]
    steps = [block.splitlines()[0] for block in blocks]
    for name in (CONFIGURE, INITIALIZE, BUILD, ANALYZE):
        if steps.count(name) != 1:
            raise ValueError("Expected one step: " + name)
    positions = [steps.index(name) for name in (CONFIGURE, INITIALIZE, BUILD, ANALYZE)]
    if positions != sorted(positions):
        raise ValueError("Configure must precede tracing; build must follow initialization")
    for name in (CONFIGURE, BUILD):
        if "        if: matrix.profile == 'cpp-redshift'\n" not in blocks[steps.index(name)]:
            raise ValueError("Manual operation has an unexpected profile condition")
    if re.search(r"(?m)^        if:", blocks[steps.index(INITIALIZE)]):
        raise ValueError("Every matrix profile must initialize CodeQL")


def fresh_extraction(text):
    job = text.split("  analyze:\n", 1)[1].split("    steps:\n", 1)[0]
    if not re.search(r"(?m)^    env:\n(?:      [^\n]*\n)*"
                     r"      CODEQL_OVERLAY_DATABASE_MODE: none$", job):
        raise ValueError("Automatic overlay databases must be disabled for this job")
    init = text.split("      - name: " + INITIALIZE + "\n", 1)[1].split("      - name:", 1)[0]
    for name in ("dependency-caching", "trap-caching"):
        if not re.search(r"(?m)^          " + name + r": false$", init):
            raise ValueError("Fresh extraction requires " + name + ": false")


class ManualTracingBoundary(unittest.TestCase):
    def test_database_overlay_and_both_cache_types_are_explicitly_disabled(self):
        fresh_extraction(WORKFLOW.read_text())

    def test_implicit_or_enabled_reuse_is_rejected(self):
        text = WORKFLOW.read_text()
        for old, replacements in (
                ("CODEQL_OVERLAY_DATABASE_MODE: none", ("", "CODEQL_OVERLAY_DATABASE_MODE: overlay")),
                ("dependency-caching: false", ("", "dependency-caching: true")),
                ("trap-caching: false", ("", "trap-caching: true"))):
            for replacement in replacements:
                with self.subTest(setting=old, replacement=replacement):
                    with self.assertRaises(ValueError):
                        fresh_extraction(text.replace(old, replacement))

    def test_manager_context_is_compiled_in_source_profile_before_initialization(self):
        text = WORKFLOW.read_text()
        block = text.split("      - name: " + DM_CONTEXT + "\n", 1)[1].split("      - name:", 1)[0]
        self.assertIn("        if: matrix.profile == 'cpp-source'\n", block)
        self.assertLess(text.index(DM_CONTEXT), text.index(INITIALIZE))
        self.assertIn("g++ -std=c++20 -Wall -Wextra -Werror -fsyntax-only", block)
        self.assertIn(".github/codeql/dm_header_context.cpp", block)
        self.assertIn("tee codeql-evidence/dm-header-context.log", block)
        self.assertNotIn("-DODBCPP_TEST_VIA_DRIVER_MANAGER", text)

    def test_configure_is_untraced_but_actual_build_is_traced(self):
        tracing_boundary(WORKFLOW.read_text())

    def test_initializing_before_configure_is_rejected(self):
        text = WORKFLOW.read_text()
        start, middle, end = [text.index("      - name: " + name + "\n")
                              for name in (CONFIGURE, INITIALIZE, BUILD)]
        changed = text[:start] + text[middle:end] + text[start:middle] + text[end:]
        with self.assertRaisesRegex(ValueError, "Configure must precede"):
            tracing_boundary(changed)

    def test_skipping_manual_initialization_is_rejected(self):
        changed = WORKFLOW.read_text().replace(
            "      - name: " + INITIALIZE + "\n",
            "      - name: " + INITIALIZE + "\n        if: matrix.profile != 'cpp-redshift'\n")
        with self.assertRaisesRegex(ValueError, "Every matrix profile"):
            tracing_boundary(changed)

    def test_raw_logs_precede_verification_and_publication(self):
        text = WORKFLOW.read_text()
        retained = re.search(r"(?m)^      - name: Retain (?:C\+\+|compiled-language) "
                             r"extractor logs and diagnostics even after failure$", text)
        self.assertIsNotNone(retained)
        self.assertLess(retained.start(),
                        text.index("      - name: Independently verify actual extraction"))
        self.assertLess(text.index("      - name: Independently verify actual extraction"),
                        text.index("      - name: Publish only the verified SARIF"))

    def test_csharp_build_is_traced_fresh_and_compiler_servers_disabled(self):
        text = WORKFLOW.read_text()
        blocks = re.split(r"(?m)^      - name: ", text)[1:]
        steps = [block.splitlines()[0] for block in blocks]
        self.assertEqual(steps.count(CSHARP_BUILD), 1)
        self.assertLess(steps.index(INITIALIZE), steps.index(CSHARP_BUILD))
        self.assertLess(steps.index(CSHARP_BUILD), steps.index(ANALYZE))
        block = blocks[steps.index(CSHARP_BUILD)]
        self.assertIn("        if: matrix.profile == 'csharp'\n", block)
        self.assertIn("language: csharp\n            build-mode: manual", text)
        for required in ("dotnet build .github/codeql/standalone-csharp.csproj",
                         "--no-incremental", "--disable-build-servers",
                         "-p:UseSharedCompilation=false", "-maxcpucount:1",
                         "tee codeql-evidence/csharp-build/build.log"):
            self.assertIn(required, block)

    def test_library_project_compiles_all_tracked_cs_without_duplicate_source_or_main(self):
        root = WORKFLOW.parents[2]
        project = root / ".github/codeql/standalone-csharp.csproj"
        text = project.read_text()
        tracked = set(subprocess.check_output(["git", "ls-files", "*.cs"],
                                              cwd=root, text=True).splitlines())
        self.assertEqual(library_sources(text), tracked)
        with self.assertRaisesRegex(ValueError, "explicit library"):
            library_sources(text.replace("<OutputType>Library", "<OutputType>Exe"))


if __name__ == "__main__":
    unittest.main()
