#!/usr/bin/env python3
"""Focused offline contracts for the driver-manager test inventory."""
from pathlib import Path
import csv
import io
import tempfile
import unittest

import dm_test_inventory as inventory

HEADER = """#pragma once
namespace rs::odbc {
class HandleRegistry { public: static HandleRegistry& instance(); };
class ODBCConnection {};
class ODBCStatement {};
std::string utf8_to_wide(std::string_view text);
}
namespace rs::core::database {
class IBackendProvider {};
struct QueryResult { int rows; };
using Clock = int;
}
"""

PUBLIC = """#include <gtest/gtest.h>
// rs::odbc::ODBCStatement in a comment is not a use.
class ConnectTest : public ::testing::Test {
 protected:
  void SetUp() override { SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &env); }
  SQLHENV env{};
};
TEST_F(ConnectTest, Allocates) {
  const char* text = "rs::odbc::ODBCStatement in a string";
  EXPECT_EQ(SQL_SUCCESS, SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, nullptr, 0));
}
TEST(PlainTest, NoOdbc) { EXPECT_EQ(1, 1); }
"""

FAKE = """#include <gtest/gtest.h>
using namespace rs::core::database;
struct Observations { std::vector<QueryResult> results; };
class FakeProvider final : public IBackendProvider {
  std::shared_ptr<Observations> seen_;
};
class FakeTest : public ::testing::Test {
 protected:
  std::shared_ptr<Observations> seen = std::make_shared<Observations>();
  void SetUp() override {
    auto connection = std::make_unique<rs::odbc::ODBCConnection>(
        nullptr, std::make_shared<FakeProvider>(seen));
    rs::odbc::HandleRegistry::instance().register_handle(dbc, std::move(connection), env);
  }
  SQLHDBC dbc{}; SQLHENV env{};
};
TEST_F(FakeTest, PublicOverFake) {
  EXPECT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr, nullptr, 0, nullptr, 0, nullptr, 0));
}
TEST_F(FakeTest, ReadsFakeState) {
  SQLDisconnect(dbc);
  EXPECT_TRUE(seen->results.empty());
}
TEST_F(FakeTest, LooksUpHandle) {
  auto c = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  SQLDisconnect(dbc);
}
"""

INTERNAL = """#include <gtest/gtest.h>
using namespace rs::odbc;
std::string widen(const char* text) { return utf8_to_wide(text); }
TEST(InternalTest, HelperUsesInternal) { SQLFreeHandle(SQL_HANDLE_ENV, nullptr); widen("x"); }
TEST(InternalTest, BodyUsesInternal) { SQLFreeHandle(SQL_HANDLE_ENV, nullptr); ODBCStatement s; }
TEST(InternalTest, LocalAliasShadows) {
  using Clock = std::chrono::steady_clock;
  SQLFreeHandle(SQL_HANDLE_ENV, nullptr);
  Clock::now();
}
TEST(InternalTest, QualifiedAliasIsInternal) {
  SQLFreeHandle(SQL_HANDLE_ENV, nullptr);
  rs::core::database::Clock tick{};
}
TEST(InternalTest, UncalledFunctionNameIsNotEvidence) {
  int utf8_to_wide = 0;
  SQLFreeHandle(SQL_HANDLE_ENV, nullptr);
}
"""


class StripTests(unittest.TestCase):
    def test_comments_literals_and_preprocessor_are_blanked_with_lines_kept(self):
        text = '#include "a.h"\nint x; // rs::odbc::A\n/* B\n */ auto s = R"x(rs::C)x"; char c = \'"\';\n'
        stripped = inventory.strip_source(text)
        self.assertEqual(text.count("\n"), stripped.count("\n"))
        for hidden in ("rs::odbc::A", "rs::C", "include", "B"):
            self.assertNotIn(hidden, stripped)
        self.assertIn("int x;", stripped)

    def test_digit_separators_do_not_open_a_literal(self):
        stripped = inventory.strip_source("int n = 1'000; SQLFetch(s);")
        self.assertIn("SQLFetch(s)", stripped)


class ParseTests(unittest.TestCase):
    def test_cases_definitions_and_lines(self):
        parsed = inventory.parse(inventory.strip_source(PUBLIC))
        self.assertEqual([("TEST_F", "ConnectTest", "Allocates", 8), ("TEST", "PlainTest", "NoOdbc", 12)],
                         [(c.macro, c.suite, c.name, c.line) for c in parsed.cases])
        self.assertIn("ConnectTest", parsed.definitions)

    def test_namespace_scope_functions_skip_members_and_types(self):
        names = inventory.namespace_scope_functions(inventory.strip_source(HEADER))
        self.assertEqual({"utf8_to_wide"}, names)


class ClassifyTests(unittest.TestCase):
    def run_inventory(self, files):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / "odbc").mkdir()
            (repo / "odbc" / "internal.h").write_text(HEADER)
            (repo / "tests" / "unit").mkdir(parents=True)
            for name, text in files.items():
                (repo / "tests" / "unit" / name).write_text(text)
            rows = inventory.inventory(repo)
        return {r["case"]: r for r in rows}

    def test_public_and_no_odbc_entry(self):
        rows = self.run_inventory({"test_public.cpp": PUBLIC})
        self.assertEqual("public-only", rows["Allocates"]["class"])
        self.assertEqual("", rows["Allocates"]["internals"])
        self.assertEqual("no-odbc-entry", rows["NoOdbc"]["class"])

    def test_fake_glue_state_and_registry_lookup(self):
        rows = self.run_inventory({"test_fake.cpp": FAKE})
        self.assertEqual("fake-backend", rows["PublicOverFake"]["class"])
        self.assertEqual("", rows["PublicOverFake"]["fake_state"])
        self.assertEqual("SQLDriverConnect+registered-handle", rows["PublicOverFake"]["connect"])
        self.assertEqual("fake-backend", rows["ReadsFakeState"]["class"])
        self.assertEqual("yes", rows["ReadsFakeState"]["fake_state"])
        self.assertEqual("needs-internals", rows["LooksUpHandle"]["class"])
        self.assertIn("HandleRegistry::get_handle_as", rows["LooksUpHandle"]["internals"])
        self.assertEqual("body", rows["LooksUpHandle"]["internals_in"])

    def test_internal_evidence_scope_shadowing_and_calls(self):
        rows = self.run_inventory({"test_internal.cpp": INTERNAL})
        self.assertEqual(("needs-internals", "helpers", "utf8_to_wide"),
                         tuple(rows["HelperUsesInternal"][k] for k in ("class", "internals_in", "internals")))
        self.assertEqual(("needs-internals", "body", "ODBCStatement"),
                         tuple(rows["BodyUsesInternal"][k] for k in ("class", "internals_in", "internals")))
        self.assertEqual("public-only", rows["LocalAliasShadows"]["class"])
        self.assertEqual("needs-internals", rows["QualifiedAliasIsInternal"]["class"])
        self.assertEqual("public-only", rows["UncalledFunctionNameIsNotEvidence"]["class"])

    def test_check_mode_detects_stale_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / "odbc").mkdir()
            (repo / "odbc" / "internal.h").write_text(HEADER)
            (repo / "tests" / "unit").mkdir(parents=True)
            (repo / "tests" / "unit" / "test_public.cpp").write_text(PUBLIC)
            out_csv, out_md = repo / "inv.csv", repo / "inv.md"
            args = ["--repo", str(repo), "--csv", str(out_csv), "--summary", str(out_md)]
            self.assertEqual(0, inventory.main(args))
            self.assertEqual(0, inventory.main(args + ["--check"]))
            rows = list(csv.DictReader(io.StringIO(out_csv.read_text())))
            self.assertEqual(2, len(rows))
            out_md.write_text("stale")
            self.assertEqual(1, inventory.main(args + ["--check"]))


if __name__ == "__main__":
    unittest.main()
