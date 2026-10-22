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
    def test_shared_two_case_profile_reuses_one_private_file_and_retires_once(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / 'private'
            cases = [dict(id='R'+str(i), image='owner', profile='shared',
                          gtest_filter='Relationship.Case'+str(i)) for i in range(2)]
            specs = [dict(ctest_id=row['id'],ctest_profile='shared',image='owner',
                          case=row['gtest_filter'],key=row['id'],profile='logical',
                          env=None,marker=None) for row in cases]
            profiles = {'logical':dict(private_input={'kind':'CORE_ENV'},
                connection_environment=['ODBCPP_TEST_DSN'],variables={'MARKER':'1'})}
            index, custody = inputs.stage_case_environments(directory, {'cases':cases},
                specs, profiles, {'logical':'invented-only'}, {'logical':{}})
            self.assertEqual(index['cases'],cases)
            self.assertEqual(len(custody),1)
            self.assertEqual(len(list(directory.iterdir())),1)
            path=Path(index['profiles']['shared']['environment_file'])
            self.assertIn('ODBCPP_TEST_DSN=invented-only',json.loads(path.read_text())['environment'])
            inputs.close_case_environments(custody,directory)
            self.assertFalse(directory.exists())
            # The same selector cannot silently replace different case inputs.
            specs[1].update(env='MARKER',marker='2')
            with self.assertRaisesRegex(ValueError,'different environment'):
                inputs.stage_case_environments(directory,{'cases':cases},specs,
                    profiles,{'logical':'invented-only'},{'logical':{}})
            self.assertFalse(directory.exists())

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

    def test_explicit_TLS_alias_and_catalog_fixture_inputs_follow_profiles(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "private"
            case = {"id": "C1", "image": "image", "profile": "p_C1",
                    "gtest_filter": "Suite.TLSAndCatalog"}
            spec = dict(ctest_id="C1", ctest_profile="p_C1", image="image",
                        case=case["gtest_filter"], key=case["gtest_filter"], profile="p",
                        env=None, marker=None, catalog="varbyte")
            table = "fullsuite_catalog_types_20261015_c01"
            profiles = {"p": {"private_input": {"kind": "CORE_ENV"},
                "connection_environment": ["ODBCPP_REDSHIFT_TEST_CONNECTION", "ODBCPP_TEST_DSN"],
                "variables": {"ODBCPP_REDSHIFT_VARBYTE_CATALOG_SCHEMA": "odbcpp_fixture",
                              "ODBCPP_REDSHIFT_VARBYTE_CATALOG_TABLE": table}}}
            connection = "SERVER=mock.invalid;UID=odbcpp_pilot_test;SSL=1;SSLCAFILE=/mock/ca;"
            index, custody = inputs.stage_case_environments(directory, {"cases": [case]},
                [spec], profiles, {"p": connection}, {"p": {}})
            path = Path(index["profiles"]["p_C1"]["environment_file"])
            env = dict(value.split("=", 1) for value in
                       json.loads(path.read_text())["environment"])
            self.assertEqual(env["ODBCPP_TEST_DSN"], connection)
            self.assertEqual(env["ODBCPP_REDSHIFT_TEST_CONNECTION"], connection)
            self.assertEqual(env["ODBCPP_REDSHIFT_VARBYTE_CATALOG_TABLE"], table)
            self.assertEqual(env["ODBCPP_REDSHIFT_VARBYTE_CATALOG_SCHEMA"], "odbcpp_fixture")
            inputs.close_case_environments(custody, directory)

    def test_owned_DSN_keeps_DSN_name_without_emitting_direct_connection(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "private"
            case = {"id": "C1", "image": "image", "profile": "p_C1", "gtest_filter": "Suite.DSN"}
            spec = dict(ctest_id="C1", ctest_profile="p_C1", image="image", case="Suite.DSN",
                        key="Suite.DSN", profile="p", env=None, marker=None, catalog=None)
            profiles = {"p": {"private_input": {"kind": "OWN_DSN"},
                "connection_environment": ["ODBCPP_REDSHIFT_TEST_CONNECTION"],
                "variables": {"ODBCPP_TEST_DSN": "DSN=RedshiftProd"}}}
            index, custody = inputs.stage_case_environments(directory, {"cases": [case]},
                [spec], profiles, {"p": "SSL=1;PWD=synthetic;"},
                {"p": {"ODBCINI": "/mock/private/odbc.ini"}})
            path = Path(index["profiles"]["p_C1"]["environment_file"])
            env = dict(value.split("=", 1) for value in json.loads(path.read_text())["environment"])
            self.assertEqual(env["ODBCPP_TEST_DSN"], "DSN=RedshiftProd")
            self.assertNotIn("ODBCPP_REDSHIFT_TEST_CONNECTION", env)
            self.assertEqual(env["ODBCINI"], "/mock/private/odbc.ini")
            inputs.close_case_environments(custody, directory)

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
        for kind in ("OWN_DM_FILE", "VENDOR_DM_FILE"):
            with self.subTest(kind=kind):
                connection_file = factory.build_profile_connection({}, {"private_input": {"kind": kind}})
                self.assertEqual("UID=test;PWD={dummy};SSL=1;", connection_file)
        dsn, registration = factory.dsn_documents()
        self.assertIn("LogQueries=false\n", dsn)
        self.assertEqual("[driver]\nDriver=example\n", registration)


if __name__ == "__main__":
    unittest.main()
