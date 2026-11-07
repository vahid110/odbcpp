import unittest
from provider_activation import latest_pass


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
