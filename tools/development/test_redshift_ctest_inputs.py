import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("inputs", Path(__file__).with_name("redshift_ctest_inputs.py"))
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)


class InputTests(unittest.TestCase):
    def test_profile_secrets_stay_in_private_files_and_markers_remain_distinct(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "private"
            cases = [{"id": "C1", "image": "image", "profile": "p_C1", "gtest_filter": "Suite.One"},
                     {"id": "C2", "image": "image", "profile": "p_C2", "gtest_filter": "Suite.Two"}]
            specs = [dict(ctest_id=c["id"], ctest_profile=c["profile"], image="image",
                          case=c["gtest_filter"], key=c["gtest_filter"], profile="p",
                          env="ODBCPP_TEST_MARKER", marker=c["id"], catalog=None) for c in cases]
            profiles = {"p": {"private_input": {"kind": "CORE_ENV"},
                              "connection_environment": ["ODBCPP_REDSHIFT_TEST_CONNECTION"],
                              "variables": {"LIMIT": "9"}}}
            secret = "UID=test;PWD={synthetic;a}}b};MAXROWS=2;"
            index, custody = inputs.stage_case_environments(directory, {"cases": cases}, specs,
                profiles, {"p": secret}, {"p": {"ODBCINI": "/private/test/odbc.ini"}})
            self.assertNotIn(secret, json.dumps(index))
            for case in cases:
                path = Path(index["profiles"][case["profile"]]["environment_file"])
                self.assertEqual(0o600, path.stat().st_mode & 0o777)
                values = dict(s.split("=", 1) for s in json.loads(path.read_text())["environment"])
                self.assertEqual(secret, values["ODBCPP_REDSHIFT_TEST_CONNECTION"])
                self.assertEqual(case["id"], values["ODBCPP_TEST_MARKER"])
                self.assertEqual("9", values["LIMIT"])
            inputs.close_case_environments(custody, directory)
            self.assertFalse(directory.exists())

    def test_partial_staging_failure_cleans_created_files(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "private"
            with self.assertRaises(ValueError):
                inputs.stage_case_environments(path, {"cases": [{"id": "wrong"}]},
                                               [{"ctest_id": "C1"}], {}, {}, {})
            self.assertFalse(path.exists())

    def test_generated_metadata_retired_only_with_held_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "CTestTestfile.cmake"
            path.write_text("synthetic private connection")
            path.chmod(0o600)
            held = inputs.generated_environment_custody(temp)
            path.write_text("changed synthetic connection")
            with self.assertRaises(RuntimeError):
                inputs.close_generated_environment(held)
            held = inputs.generated_environment_custody(temp)
            inputs.close_generated_environment(held)
            self.assertFalse(path.exists())

    def test_logging_preserves_connection_and_vendor_profile(self):
        class Factory:
            def build_profile_connection(self, config, profile, **kwargs):
                return "UID=test;PWD={dummy};SSL=1;"

            def dsn_documents(self, *args, **kwargs):
                return "[RedshiftProd]\nUID=test\n", "[driver]\nDriver=example\n"

        factory = inputs.LoggingFactory(Factory())
        owned = factory.build_profile_connection({}, {"private_input": {"kind": "CORE_ENV"}})
        self.assertTrue(owned.startswith("UID=test;PWD={dummy};SSL=1;"))
        self.assertIn("LogLevel=Debug;", owned)
        self.assertIn("LogQueries=false;", owned)
        self.assertIn("LogSink=Stderr;", owned)
        vendor = factory.build_profile_connection({}, {"private_input": {"kind": "VENDOR_DM_FILE"}})
        self.assertEqual("UID=test;PWD={dummy};SSL=1;", vendor)
        dsn, registration = factory.dsn_documents()
        self.assertIn("LogQueries=false\n", dsn)
        self.assertEqual("[driver]\nDriver=example\n", registration)


if __name__ == "__main__":
    unittest.main()
