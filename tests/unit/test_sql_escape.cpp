#include <gtest/gtest.h>

#include "odbc/sql_escape.h"

using rs::odbc::SqlEscapeError;
using rs::odbc::translate_odbc_sql;

TEST(SqlEscapeTest, TranslatesDatetimeLiterals) {
  EXPECT_EQ("SELECT DATE '2024-02-29'",
            translate_odbc_sql("SELECT {d '2024-02-29'}").sql);
  EXPECT_EQ("SELECT TIME '23:59:59.123'",
            translate_odbc_sql("SELECT {t '23:59:59.123'}").sql);
  EXPECT_EQ("SELECT TIMESTAMP '2026-09-12 14:30:01'",
            translate_odbc_sql(
                "SELECT {ts '2026-09-12 14:30:01'}").sql);
}

TEST(SqlEscapeTest, RejectsInvalidDatetimeValues) {
  for (const auto* sql : {
           "SELECT {d '2023-02-29'}", "SELECT {d '2024-13-01'}",
           "SELECT {t '24:00:00'}", "SELECT {t '12:00:60'}",
           "SELECT {ts '2024-01-01T12:00:00'}"}) {
    const auto translated = translate_odbc_sql(sql);
    EXPECT_EQ(SqlEscapeError::InvalidDatetime, translated.error) << sql;
    EXPECT_FALSE(translated.message.empty()) << sql;
  }
}

TEST(SqlEscapeTest, TranslatesNestedScalarFunctions) {
  EXPECT_EQ("SELECT UPPER(LOWER(name))",
            translate_odbc_sql(
                "SELECT {fn UCASE({fn LCASE(name)})}").sql);
  EXPECT_EQ("SELECT COALESCE(value, 0), CURRENT_DATE, CURRENT_TIME, "
            "CURRENT_TIMESTAMP",
            translate_odbc_sql(
                "SELECT {fn IFNULL(value, 0)}, {fn CURDATE()}, "
                "{fn CURTIME()}, {fn NOW()}").sql);
}

TEST(SqlEscapeTest, TranslatesJoinLikeAndProcedureEscapes) {
  EXPECT_EQ("SELECT * FROM a LEFT OUTER JOIN b ON a.id=b.id",
            translate_odbc_sql(
                "SELECT * FROM {oj a LEFT OUTER JOIN b ON a.id=b.id}").sql);
  EXPECT_EQ("SELECT name FROM t WHERE name LIKE 'x!_%' ESCAPE '!'",
            translate_odbc_sql(
                "SELECT name FROM t WHERE name LIKE 'x!_%' {escape '!'}").sql);
  EXPECT_EQ("CALL refresh_cache(1)",
            translate_odbc_sql("{call refresh_cache(1)}").sql);
}

TEST(SqlEscapeTest, PreservesBracesOutsideEscapeClauses) {
  const char* sql =
      "SELECT '{d ''not-a-date''}', \"{column}\", $$ {fn UCASE(x)} $$ "
      "-- {t '99:99:99'}\n/* {oj ignored} */ {vendor native}";
  EXPECT_EQ(sql, translate_odbc_sql(sql).sql);
}

TEST(SqlEscapeTest, ReportsMalformedAndUnsupportedEscapes) {
  EXPECT_EQ(SqlEscapeError::InvalidSyntax,
            translate_odbc_sql("SELECT {d '2024-01-01'").error);
  EXPECT_EQ(SqlEscapeError::InvalidSyntax,
            translate_odbc_sql("SELECT 1 {escape '!!'}").error);
  EXPECT_EQ(SqlEscapeError::Unsupported,
            translate_odbc_sql("{?= call answer()}").error);
}
