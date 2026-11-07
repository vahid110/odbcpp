import json
from pathlib import Path
import unittest
from unittest.mock import patch
from provider_activation import latest_pass, is_proven


class ProviderActivationTests(unittest.TestCase):
    def test_absent_and_failed_proofs_do_not_enable(self):
        self.assertFalse(latest_pass([]))
        self.assertFalse(latest_pass([{"external_id": "15:1", "conclusion": "failure"}]))

    def test_newer_dispatch_outranks_rerun_of_old_dispatch(self):
        runs = [{"external_id": "24:1", "conclusion": "failure"},
                {"external_id": "9:8", "conclusion": "success"}]
        self.assertFalse(latest_pass(runs))
        runs.append({"external_id": "24:2", "conclusion": "success"})
        self.assertTrue(latest_pass(runs))

    def test_queued_review_observes_new_pending_proof_and_ignores_foreign_checks(self):
        directory = Path(__file__).resolve().parent
        anchor = (directory / "run-anchor").read_text().strip()
        app = int((directory / "reviewer-app-id").read_text())
        proof = {"external_id": "17:1", "app": {"id": app}, "head_sha": anchor,
                 "name": "Provider canary openai example", "conclusion": "success"}
        pending = dict(proof, external_id="18:1", conclusion=None)
        foreign = dict(proof, external_id="19:1", app={"id": app + 1})
        other = dict(proof, external_id="20:1", name="Provider canary claude example")
        stale = dict(proof, external_id="21:1", name="Provider canary openai obsolete")
        def pages(rows):
            return json.dumps([{"check_runs": rows}]).encode()

        with patch.dict("os.environ", GITHUB_REPOSITORY="example/project"), patch(
            "subprocess.check_output",
            side_effect=[pages([proof]), pages([proof, pending, foreign, other, stale])],
        ):
            self.assertTrue(is_proven("openai", "example"))
            self.assertFalse(is_proven("openai", "example"))
