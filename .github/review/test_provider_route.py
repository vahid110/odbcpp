import unittest
from provider_route import route


class ProviderRouteTests(unittest.TestCase):
    def test_provider_failure_without_output(self):
        self.assertEqual(route("failure", ""), "openai")

    def test_keep_any_result_for_existing_validation(self):
        for outcome in ("success", "failure"):
            for result in ('{"verdict":"request_changes"}', '{"verdict":"approve"}', "{invalid"):
                with self.subTest(outcome=outcome, result=result):
                    self.assertEqual(route(outcome, result), "claude")

    def test_success_without_output_or_action_not_run(self):
        for outcome in ("success", "skipped", "cancelled"):
            with self.assertRaises(ValueError):
                route(outcome, "")
