"""Check the OpenAI canary's synthetic input and judgement without a service call."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import openai_canary as canary
import openai_fallback as fallback


class CanaryTest(unittest.TestCase):
    def test_each_fixture_forms_a_complete_production_request(self):
        trusted = Path(__file__).resolve().parents[2]
        for case, role, expected_addition in (
            ("spelling", "correctness", "+The driver supports PostgreSQL"),
            ("credential-log", "red-team", '+  std::fprintf(stderr, "Connecting as %s with %s'),
        ):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                workspace = Path(directory)
                head = canary.prepare(case, workspace / "review-context")
                request = json.loads(fallback.make_request(workspace, trusted, role, head, 1))
                data = json.loads(request["input"])
                self.assertEqual(head, data["pr"]["head_sha"])
                self.assertIn(expected_addition, data["full_diff"])

    def test_expected_verdict_and_specific_finding(self):
        verdict = {"verdict": "approve", "body": "Spelling is correct.", "labels": []}
        self.assertTrue(canary.judge("spelling", verdict))
        self.assertFalse(canary.judge("credential-log", verdict))
        verdict.update(verdict="request_changes", body="Missing test.")
        self.assertFalse(canary.judge("spelling", verdict))
        self.assertFalse(canary.judge("credential-log", verdict))
        verdict.update(body="The connection trace writes the password to stderr.")
        self.assertTrue(canary.judge("credential-log", verdict))


if __name__ == "__main__":
    unittest.main()
