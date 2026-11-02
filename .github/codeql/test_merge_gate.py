"""Offline regression for the CodeQL execution gate, not scanner results."""

import os
from pathlib import Path
import re
import subprocess
import textwrap
import unittest


WORKFLOW = Path(__file__).resolve().parents[1] / "workflows/codeql.yml"
PROFILES = {"cpp-redshift", "cpp-source", "python", "csharp", "actions"}


def job(text, name):
    match = re.search(r"(?ms)^  " + re.escape(name) + r":\n(.*?)(?=^  \S|\Z)", text)
    if match is None:
        raise ValueError("Missing CodeQL job: " + name)
    return match.group(1)


class MergeGate(unittest.TestCase):
    def test_analysis_keeps_all_required_profiles_without_failure_masking(self):
        analysis = job(WORKFLOW.read_text(), "analyze")
        profiles = re.findall(r"(?m)^          - profile: (\S+)$", analysis)
        self.assertEqual(set(profiles), PROFILES)
        self.assertEqual(len(profiles), len(PROFILES))
        self.assertIn("    name: CodeQL (${{ matrix.profile }})\n", analysis)
        self.assertIn("      fail-fast: false\n", analysis)
        self.assertNotRegex(analysis, r"(?m)^    if:")
        self.assertNotIn("continue-on-error:", analysis)

    def test_gate_is_unconditional_and_depends_on_the_analysis_matrix(self):
        gate = job(WORKFLOW.read_text(), "execution-gate")
        self.assertIn("    name: CodeQL execution gate\n", gate)
        self.assertIn("    needs: analyze\n", gate)
        self.assertIn("    if: ${{ always() }}\n", gate)
        self.assertIn("          ANALYSIS_RESULT: ${{ needs.analyze.result }}\n", gate)
        self.assertNotRegex(gate, r"(?m)^        if:")
        self.assertNotIn("continue-on-error:", gate)
        self.assertNotIn("uses:", gate)  # No checkout, credentials or artifact execution.

    def run_gate(self, result):
        gate = job(WORKFLOW.read_text(), "execution-gate")
        script = textwrap.dedent(gate.split("        run: |\n", 1)[1])
        environment = {"PATH": os.defpath}
        if result is not None:
            environment["ANALYSIS_RESULT"] = result
        return subprocess.run(
            ["bash", "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", script],
            env=environment, text=True, capture_output=True, check=False,
        )

    def test_only_success_passes(self):
        result = self.run_gate("success")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_skipped_cancelled_missing_and_unfinished_analysis_fail(self):
        for status in ("failure", "skipped", "cancelled", "neutral", "", None,
                       "queued", "in_progress", "timed_out", "unknown"):
            with self.subTest(status=status):
                result = self.run_gate(status)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
