"""Offline controls for the manual build's tracing boundary."""

from pathlib import Path
import re
import unittest


WORKFLOW = Path(__file__).resolve().parents[1] / "workflows/codeql.yml"
CONFIGURE = "Configure the actual REDSHIFT C++20 profile"
INITIALIZE = "Initialize CodeQL"
BUILD = "Compile product, SDK header units and existing test sources without running tests"
ANALYZE = "Analyze and retain full diagnostics before publication"


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


class ManualTracingBoundary(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
