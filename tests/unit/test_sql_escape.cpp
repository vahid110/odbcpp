#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <tuple>

#include "core/database/postgres/pg_sql_dialect.h"
#include "core/database/postgres/pg_backend_provider.h"

using rs::core::database::SqlTranslationError;
using rs::core::database::postgres::translate_odbc_sql;

TEST(SqlConvertEscapeTest, NumericTemporalAndBooleanTargetsUseNativeCasts) {
  using rs::core::database::postgres::SqlDialectProfile;
  struct Target { const char* odbc; const char* pg; const char* redshift; };
  const std::array targets{
      Target{"SQL_SMALLINT", "SMALLINT", "SMALLINT"},
      Target{"SQL_INTEGER", "INTEGER", "INTEGER"},
      Target{"SQL_BIGINT", "BIGINT", "BIGINT"},
      Target{"SQL_REAL", "REAL", "REAL"},
      Target{"SQL_FLOAT", "DOUBLE PRECISION", "DOUBLE PRECISION"},
      Target{"SQL_DOUBLE", "DOUBLE PRECISION", "DOUBLE PRECISION"},
      Target{"SQL_NUMERIC", "NUMERIC", "DECIMAL(38,18)"},
      Target{"SQL_DECIMAL", "NUMERIC", "DECIMAL(38,18)"},
      Target{"SQL_BIT", "BOOLEAN", "BOOLEAN"},
      Target{"SQL_DATE", "DATE", "DATE"},
      Target{"SQL_TIME", "TIME", "TIME"},
      Target{"SQL_TIMESTAMP", "TIMESTAMP", "TIMESTAMP"}};
  for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
    for (const auto& target : targets) {
      const auto sql = "SELECT {fn CONVERT(?, " + std::string(target.odbc) + ")}";
      const auto result = translate_odbc_sql(sql, profile);
      ASSERT_TRUE(result) << result.message << sql;
      EXPECT_EQ("SELECT CAST(? AS " + std::string(
          profile == SqlDialectProfile::PostgreSql ? target.pg : target.redshift) + ")", result.sql);
      const auto null_result = translate_odbc_sql(
          "SELECT {fn CONVERT(NULL, " + std::string(target.odbc) + ")}", profile);
      ASSERT_TRUE(null_result) << null_result.message;
      EXPECT_EQ("SELECT CAST(NULL AS " + std::string(
          profile == SqlDialectProfile::PostgreSql ? target.pg : target.redshift) + ")", null_result.sql);
    }
  }
}

TEST(SqlConvertEscapeTest, NestedCallsAndLexicalBodiesPreserveEachExpressionOnce) {
  using rs::core::database::postgres::SqlDialectProfile;
  for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
    EXPECT_EQ("SELECT CAST(COALESCE(?, CAST(? AS INTEGER))-- value\n AS BIGINT)",
        translate_odbc_sql("SELECT {fn cOnVeRt /* call */ ({fn IFNULL(?, {fn CONVERT(?, SQL_INTEGER)})}-- value\n, /* type */ sql_bigint /* end */)}", profile).sql);
    EXPECT_EQ("SELECT CAST(COALESCE($tag$a,b){}$tag$, E'a\\'b,c', \"name,comma\") AS INTEGER)",
        translate_odbc_sql("SELECT {fn CONVERT(COALESCE($tag$a,b){}$tag$, E'a\\'b,c', \"name,comma\"), SQL_INTEGER)}", profile).sql);
    EXPECT_EQ("SELECT CAST(next_value() AS BIGINT)",
        translate_odbc_sql("SELECT {fn CONVERT(next_value(), SQL_BIGINT)}", profile).sql);
  }
}

TEST(SqlConvertEscapeTest, MalformedCallsAndUnsupportedTargetsRefuseBeforeDispatch) {
  using rs::core::database::postgres::SqlDialectProfile;
  for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
    for (const auto* sql : {"{fn CONVERT}", "{fn CONVERT()}", "{fn CONVERT(x)}",
         "{fn CONVERT(,SQL_INTEGER)}", "{fn CONVERT(x,/* empty */)}",
         "{fn CONVERT(x,SQL_INTEGER,1)}", "{fn CONVERT(x,SQL_INTEGER) + 1}",
         "{fn CONVERT(x,SQL_INTEGER); SELECT 1}", "{fn CONVERT(x,SQL_INTEGER(2))}",
         "{fn CONVERT(x,SQL_INTEGER -- no closer)}"}) {
      const auto result = translate_odbc_sql(sql, profile);
      EXPECT_EQ(SqlTranslationError::InvalidSyntax, result.error) << sql;
      EXPECT_TRUE(result.sql.empty()) << sql;
    }
    for (const auto* target : {"SQL_TINYINT", "SQL_UNKNOWN", "SQL_INTEGER_EXTRA"}) {
      const auto result = translate_odbc_sql("SELECT {fn CONVERT(NULL," + std::string(target) + ")}", profile);
      EXPECT_EQ(SqlTranslationError::Unsupported, result.error) << target;
      EXPECT_TRUE(result.sql.empty());
    }
    EXPECT_EQ("SELECT CAST(1 AS INTEGER)", translate_odbc_sql("SELECT {fn CONVERT(1,SQL_INTEGER)}", profile).sql);
    EXPECT_EQ("SELECT '{fn CONVERT(1,SQL_INTEGER)}', CONVERTX(1,SQL_INTEGER)",
        translate_odbc_sql("SELECT '{fn CONVERT(1,SQL_INTEGER)}', {fn CONVERTX(1,SQL_INTEGER)}", profile).sql);
  }
}

TEST(SqlConvertEscapeTest, CharacterBinaryAndUuidPoliciesAreExplicitPerProduct) {
  using rs::core::database::postgres::SqlDialectProfile;
  for (const auto* target : {"SQL_CHAR", "SQL_VARCHAR", "SQL_LONGVARCHAR",
                           "SQL_WCHAR", "SQL_WVARCHAR", "SQL_WLONGVARCHAR"}) {
    const auto sql = "SELECT {fn CONVERT('Grüße'," + std::string(target) + ")}";
    EXPECT_EQ("SELECT CAST('Grüße' AS TEXT)", translate_odbc_sql(sql).sql);
    EXPECT_EQ("SELECT CAST('Grüße' AS VARCHAR(65535))",
        translate_odbc_sql(sql, SqlDialectProfile::Redshift).sql);
  }
  for (const auto* target : {"SQL_BINARY", "SQL_VARBINARY", "SQL_LONGVARBINARY"}) {
    const auto sql = "SELECT {fn CONVERT(NULL," + std::string(target) + ")}";
    EXPECT_EQ("SELECT CAST(NULL AS BYTEA)", translate_odbc_sql(sql).sql);
    EXPECT_EQ("SELECT CAST(NULL AS VARBYTE)", translate_odbc_sql(sql, SqlDialectProfile::Redshift).sql);
  }
  EXPECT_EQ("SELECT CAST(NULL AS UUID)", translate_odbc_sql("SELECT {fn CONVERT(NULL,SQL_GUID)}").sql);
  EXPECT_EQ(SqlTranslationError::Unsupported,
      translate_odbc_sql("SELECT {fn CONVERT(NULL,SQL_GUID)}", SqlDialectProfile::Redshift).error);
  EXPECT_EQ("SELECT CAST((ARRAY[1,2])[1] AS INTEGER)",
      translate_odbc_sql("SELECT {fn CONVERT((ARRAY[1,2])[1],SQL_INTEGER)}").sql);
  for (const auto* target : {"SQL_INTERVAL_YEAR", "SQL_INTERVAL_MONTH", "SQL_INTERVAL_YEAR_TO_MONTH",
                           "SQL_INTERVAL_DAY", "SQL_INTERVAL_HOUR", "SQL_INTERVAL_MINUTE",
                           "SQL_INTERVAL_SECOND", "SQL_INTERVAL_DAY_TO_HOUR", "SQL_INTERVAL_DAY_TO_MINUTE",
                           "SQL_INTERVAL_DAY_TO_SECOND", "SQL_INTERVAL_HOUR_TO_MINUTE",
                           "SQL_INTERVAL_HOUR_TO_SECOND", "SQL_INTERVAL_MINUTE_TO_SECOND"}) {
    for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
      const auto result = translate_odbc_sql("SELECT {fn CONVERT(NULL," + std::string(target) + ")}", profile);
      EXPECT_EQ(SqlTranslationError::Unsupported, result.error) << target;
      EXPECT_TRUE(result.sql.empty());
    }
  }
}

TEST(SqlTimestampEscapeTest, SupportedUnitsTranslateAndKeepMarkerAssociation) {
  using rs::core::database::postgres::SqlDialectProfile;
  rs::core::database::postgres::PgBackendProvider parser{
      {"postgresql", "PostgreSQL", "synthetic"}, {"synthetic.invalid", 5432, "synthetic", false}};
  for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
    for (const auto* unit : {"SECOND", "MINUTE", "HOUR", "DAY", "WEEK", "MONTH", "QUARTER", "YEAR"}) {
      const auto sql = "SELECT {fn TIMESTAMPADD(SQL_TSI_" + std::string(unit) + ", ?, ?)}";
      const auto result = translate_odbc_sql(sql, profile);
      ASSERT_TRUE(result) << result.message << sql;
      EXPECT_EQ(std::string::npos, result.sql.find("TIMESTAMPADD")) << result.sql;
      EXPECT_EQ(std::string::npos, result.sql.find("SQL_TSI_")) << result.sql;
      EXPECT_EQ(2u, parser.sql_dialect().count_parameter_markers(result.sql)) << result.sql;
    }
    for (const auto* unit : {"SECOND", "MINUTE", "HOUR", "DAY", "MONTH", "QUARTER", "YEAR"}) {
      const auto sql = "SELECT {fn TIMESTAMPDIFF(SQL_TSI_" + std::string(unit) + ", first_value(), second_value())}";
      const auto result = translate_odbc_sql(sql, profile);
      ASSERT_TRUE(result) << result.message << sql;
      EXPECT_EQ(std::string::npos, result.sql.find("TIMESTAMPDIFF")) << result.sql;
      EXPECT_EQ(std::string::npos, result.sql.find("SQL_TSI_")) << result.sql;
      const auto first = result.sql.find("first_value()");
      const auto second = result.sql.find("second_value()");
      ASSERT_NE(std::string::npos, first);
      ASSERT_NE(std::string::npos, second);
      EXPECT_LT(first, second) << result.sql;
      EXPECT_EQ(std::string::npos, result.sql.find("first_value()", first + 1));
      EXPECT_EQ(std::string::npos, result.sql.find("second_value()", second + 1));
    }
  }
}

TEST(SqlTimestampEscapeTest, FractionalAdditionAndAmbiguousWeekDifferenceRefuseExplicitly) {
  using rs::core::database::postgres::SqlDialectProfile;
  for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
    for (const auto* sql : {"{fn TIMESTAMPADD(SQL_TSI_FRAC_SECOND,1,NULL)}",
                           "{fn TIMESTAMPDIFF(SQL_TSI_FRAC_SECOND,NULL,NULL)}",
                           "{fn TIMESTAMPDIFF(SQL_TSI_WEEK,NULL,NULL)}",
                           "{fn TIMESTAMPADD(SQL_TSI_UNKNOWN,1,NULL)}",
                           "{fn TIMESTAMPDIFF(SQL_TSI_UNKNOWN,NULL,NULL)}"}) {
      const auto result = translate_odbc_sql(sql, profile);
      EXPECT_EQ(SqlTranslationError::Unsupported, result.error) << sql;
      EXPECT_TRUE(result.sql.empty());
    }
  }
}

TEST(SqlTimestampEscapeTest, InvalidArityUnitExpressionAndTrailingSqlNeverPassThrough) {
  using rs::core::database::postgres::SqlDialectProfile;
  for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
    for (const auto* sql : {"{fn TIMESTAMPADD}", "{fn TIMESTAMPADD()}", "{fn TIMESTAMPADD(SQL_TSI_DAY,1)}",
                           "{fn TIMESTAMPADD(SQL_TSI_DAY,,x)}", "{fn TIMESTAMPADD(SQL_TSI_DAY,1,x,2)}",
                           "{fn TIMESTAMPDIFF(SQL_TSI_DAY,x)}", "{fn TIMESTAMPDIFF(SQL_TSI_DAY,x,/* empty */)}",
                           "{fn TIMESTAMPDIFF(SQL_TSI_DAY,x,y) + 1}", "{fn TIMESTAMPADD(?,1,x)}",
                           "{fn TIMESTAMPDIFF('SQL_TSI_DAY',x,y)}", "{fn TIMESTAMPADD(SQL_TSI_DAY+1,1,x)}"}) {
      const auto result = translate_odbc_sql(sql, profile);
      EXPECT_EQ(SqlTranslationError::InvalidSyntax, result.error) << sql;
      EXPECT_TRUE(result.sql.empty());
    }
  }
}

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
           "SELECT {t '-1:00:00'}", "SELECT {t '12:-1:00'}",
           "SELECT {t '12:00:-1'}", "SELECT {t '-0:00:00'}",
           "SELECT {ts '2024-01-01 12:-1:00'}",
           "SELECT {ts '2024-01-01T12:00:00'}"}) {
    const auto translated = translate_odbc_sql(sql);
    EXPECT_EQ(SqlTranslationError::InvalidDatetime, translated.error) << sql;
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

TEST(SqlEscapeTest, LikeEscapeAcceptsOneUnicodeOrQuotedCharacter) {
  EXPECT_EQ("SELECT ESCAPE '\xC3\xA9'",
            translate_odbc_sql("SELECT {escape '\xC3\xA9'}").sql);
  EXPECT_EQ("SELECT ESCAPE '\xF0\x9F\x98\x80'",
            translate_odbc_sql("SELECT {escape '\xF0\x9F\x98\x80'}").sql);
  EXPECT_EQ("SELECT ESCAPE ''''",
            translate_odbc_sql("SELECT {escape ''''}").sql);
  EXPECT_EQ(SqlTranslationError::InvalidSyntax,
            translate_odbc_sql("SELECT {escape 'e\xCC\x81'}").error);
  EXPECT_EQ(SqlTranslationError::InvalidSyntax,
            translate_odbc_sql("SELECT {escape '\xC3'}").error);
}

TEST(SqlEscapeTest, PreservesBracesOutsideEscapeClauses) {
  const char* sql =
      "SELECT '{d ''not-a-date''}', \"{column}\", $$ {fn UCASE(x)} $$ "
      "-- {t '99:99:99'}\n/* {oj ignored} */ {vendor native}";
  EXPECT_EQ(sql, translate_odbc_sql(sql).sql);
}

TEST(SqlEscapeTest, PreservesNestedBlockComments) {
  const char* sql =
      "SELECT 1 /* outer /* inner */ {fn UCASE(ignored)} */ "
      "WHERE 1 = {fn IFNULL(NULL, 1)}";
  EXPECT_EQ("SELECT 1 /* outer /* inner */ {fn UCASE(ignored)} */ "
            "WHERE 1 = COALESCE(NULL, 1)",
            translate_odbc_sql(sql).sql);
  EXPECT_EQ("SELECT UPPER(1 /* outer /* inner */ } still commented */)",
            translate_odbc_sql(
                "SELECT {fn UCASE(1 /* outer /* inner */ } still commented */)}")
                .sql);
}

TEST(SqlEscapeTest, EndsLineCommentsOnCarriageReturn) {
  EXPECT_EQ("SELECT 1 -- {fn UCASE(ignored)}\r, UPPER('yes')",
            translate_odbc_sql(
                "SELECT 1 -- {fn UCASE(ignored)}\r, {fn UCASE('yes')}")
                .sql);
}

TEST(SqlEscapeTest, PreservesBackslashQuotedEscapeStrings) {
  EXPECT_EQ("SELECT E'it\\'s {fn UCASE(ignored)}', UPPER('outside')",
            translate_odbc_sql(
                "SELECT E'it\\'s {fn UCASE(ignored)}', "
                "{fn UCASE('outside')}")
                .sql);
}

TEST(SqlEscapeTest, DollarSignsInIdentifiersDoNotHideEscapes) {
  constexpr const char* sql =
      "SELECT 1 AS foo$tag$bar, {fn UCASE('ok')}, 2 AS baz$tag$qux";
  EXPECT_EQ("SELECT 1 AS foo$tag$bar, UPPER('ok'), 2 AS baz$tag$qux",
            translate_odbc_sql(sql).sql);
  EXPECT_EQ("SELECT $tag${fn UCASE('ignored')}$tag$, UPPER('ok')",
            translate_odbc_sql(
                "SELECT $tag${fn UCASE('ignored')}$tag$, {fn UCASE('ok')}")
                .sql);
}

TEST(SqlEscapeTest, ReportsMalformedAndUnsupportedEscapes) {
  EXPECT_EQ(SqlTranslationError::InvalidSyntax,
            translate_odbc_sql("SELECT {d '2024-01-01'").error);
  EXPECT_EQ(SqlTranslationError::InvalidSyntax,
            translate_odbc_sql("SELECT 1 {escape '!!'}").error);
  EXPECT_EQ(SqlTranslationError::Unsupported,
            translate_odbc_sql("{?= call answer()}").error);
}


TEST(RedshiftTemporalEscapeTest, ExactNativeProfileNestedExpressionsAndPostgresRemainDistinct) {
  using rs::core::database::postgres::SqlDialectProfile;
  const auto sql = "SELECT {fn NOW()}, {fn CURTIME()}, {fn CURDATE()}, {fn IFNULL({fn NOW()}, {fn NOW()})}";
  const auto native = translate_odbc_sql(sql, SqlDialectProfile::Redshift);
  ASSERT_TRUE(native);
  EXPECT_EQ("SELECT GETDATE(), CAST(GETDATE() AS TIME), CURRENT_DATE, COALESCE(GETDATE(), GETDATE())", native.sql);
  EXPECT_EQ("SELECT CURRENT_TIMESTAMP, CURRENT_TIME, CURRENT_DATE, COALESCE(CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)",
      translate_odbc_sql(sql).sql);
  EXPECT_EQ("SELECT GETDATE(), CAST(GETDATE() AS TIME)",
      translate_odbc_sql("SELECT {fn nOw /* name */ ( /* outer /* inner */ ok */ ) /* end */}, {fn CuRtImE( -- empty\n )}",
          SqlDialectProfile::Redshift).sql);
}

TEST(RedshiftTemporalEscapeTest, QuotedCommentsPrefixesAndPrecisionSpellingsAreNotRewritten) {
  using rs::core::database::postgres::SqlDialectProfile;
  const auto sql = "SELECT '{fn NOW()}', \"{fn CURTIME()}\", $$ {fn NOW()} $$, {fn NOWHERE()}, {fn CURTIMEX()}, "
      "{fn CURRENT_TIME(3)}, {fn CURRENT_TIMESTAMP(6)} /* {fn NOW()} */ -- {fn CURTIME()}\n";
  const auto native = translate_odbc_sql(sql, SqlDialectProfile::Redshift);
  ASSERT_TRUE(native);
  EXPECT_EQ("SELECT '{fn NOW()}', \"{fn CURTIME()}\", $$ {fn NOW()} $$, NOWHERE(), CURTIMEX(), "
      "CURRENT_TIME(3), CURRENT_TIMESTAMP(6) /* {fn NOW()} */ -- {fn CURTIME()}\n", native.sql);
  EXPECT_EQ("SELECT E'it\\'s {fn NOW()}', GETDATE()",
      translate_odbc_sql("SELECT E'it\\'s {fn NOW()}', {fn NOW()}", SqlDialectProfile::Redshift).sql);
}

TEST(RedshiftTemporalEscapeTest, RedshiftRequiresOneWholeZeroArgumentCallAndRecovers) {
  using rs::core::database::postgres::SqlDialectProfile;
  for (const auto* sql : {"SELECT {fn NOW(1)}", "SELECT {fn CURTIME(NULL)}", "SELECT {fn NOW() + 1}",
                         "SELECT {fn NOW(())}", "SELECT {fn CURTIME}", "SELECT {fn NOW()()}",
                         "SELECT {fn NOW(/* unterminated)}"}) {
    const auto rejected = translate_odbc_sql(sql, SqlDialectProfile::Redshift);
    EXPECT_EQ(SqlTranslationError::InvalidSyntax, rejected.error);
    EXPECT_TRUE(rejected.sql.empty());
  }
  EXPECT_EQ("SELECT CURRENT_TIMESTAMP(1)", translate_odbc_sql("SELECT {fn NOW(1)}").sql);
  EXPECT_EQ("SELECT GETDATE()", translate_odbc_sql("SELECT {fn NOW()}", SqlDialectProfile::Redshift).sql);
}

TEST(RedshiftTemporalEscapeTest, InterleavedProviderProfilesIgnoreMisleadingDisplayNames) {
  using namespace rs::core::database;
  postgres::PgBackendProvider rs{BackendIdentity{"redshift", "PostgreSQL", "synthetic"},
      BackendConnectionDefaults{"synthetic.invalid", 5439, "synthetic", false}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider pg{BackendIdentity{"postgresql", "Amazon Redshift", "synthetic"},
      BackendConnectionDefaults{"synthetic.invalid", 5432, "synthetic", false}};
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ("SELECT GETDATE()", rs.sql_dialect().translate_sql("SELECT {fn NOW()}").sql);
    EXPECT_EQ("SELECT CURRENT_TIMESTAMP", pg.sql_dialect().translate_sql("SELECT {fn NOW()}").sql);
    EXPECT_EQ(1u, rs.sql_dialect().count_parameter_markers("SELECT {fn NOW()}, ?"));
  }
}


TEST(RedshiftLocateEscapeTest, PositionKeepsOriginalParameterOrderAndNestedExpressionsExactlyOnce) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT POSITION('fish' IN 'dogfish')",translate_odbc_sql("SELECT {fn LOCATE('fish','dogfish')}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT POSITION(? IN ?)",translate_odbc_sql("SELECT {fn LOCATE(?,?)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT POSITION(COALESCE(?, 'a,b') IN LOWER(?))",
      translate_odbc_sql("SELECT {fn LOCATE({fn IFNULL(?, 'a,b')},{fn LCASE(?)})}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LOCATE(?,?)",translate_odbc_sql("SELECT {fn LOCATE(?,?)}").sql);
  EXPECT_EQ("SELECT GETDATE(), POSITION(NULL IN ''), CURRENT_DATE",
      translate_odbc_sql("SELECT {fn NOW()}, {fn LOCATE(NULL,'')}, {fn CURDATE()}",SqlDialectProfile::Redshift).sql);
}

TEST(RedshiftLocateEscapeTest, LexicalCommasAndTrailingLineCommentNewlinesNeverEatGeneratedSyntax) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT POSITION('a,b'-- needle\n IN 'x,a,b'-- hay\n)",
      translate_odbc_sql("SELECT {fn LOCATE('a,b'-- needle\n,'x,a,b'-- hay\n)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT POSITION($tag$a,b$tag$ IN (lower('A,B') /* outer /* , */ kept */))",
      translate_odbc_sql("SELECT {fn LOCATE($tag$a,b$tag$,(lower('A,B') /* outer /* , */ kept */))}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT POSITION(E'a\\'b,c' IN \"identifier,comma\")",
      translate_odbc_sql("SELECT {fn LOCATE(E'a\\'b,c',\"identifier,comma\")}",SqlDialectProfile::Redshift).sql);
  const auto untouched="SELECT '{fn LOCATE(a,b)}', $$ {fn LOCATE(a,b)} $$ /* {fn LOCATE(a,b)} */, {fn LOCATEX(a,b)}";
  EXPECT_EQ("SELECT '{fn LOCATE(a,b)}', $$ {fn LOCATE(a,b)} $$ /* {fn LOCATE(a,b)} */, LOCATEX(a,b)",
      translate_odbc_sql(untouched,SqlDialectProfile::Redshift).sql);
}

TEST(RedshiftLocateEscapeTest, ClosedArityErrorsPrecedeNativeDispatchAndValidCallsRecover) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ(SqlTranslationError::Unsupported,translate_odbc_sql("SELECT {fn LOCATE('a','abc',2)}",SqlDialectProfile::Redshift).error);
  for(const auto* sql:{"SELECT {fn LOCATE}","SELECT {fn LOCATE()}","SELECT {fn LOCATE(a)}",
      "SELECT {fn LOCATE(,b)}","SELECT {fn LOCATE(a,/* none */)}","SELECT {fn LOCATE(a,b,c,d)}",
      "SELECT {fn LOCATE(a,b,) }","SELECT {fn LOCATE((a,b)}","SELECT {fn LOCATE(a,b) + 1}",
      "SELECT {fn LOCATE(a,'unclosed)}","SELECT {fn LOCATE(a,$tag$unclosed)}"}) {
    const auto result=translate_odbc_sql(sql,SqlDialectProfile::Redshift);
    EXPECT_EQ(SqlTranslationError::InvalidSyntax,result.error); EXPECT_TRUE(result.sql.empty());
  }
  EXPECT_EQ("SELECT POSITION('' IN NULL)",translate_odbc_sql("SELECT {fn LOCATE('',NULL)}",SqlDialectProfile::Redshift).sql);
}

TEST(RedshiftLocateEscapeTest, ExactProfileInterleavingPreservesDefaultAndActualMarkerCount) {
  using namespace rs::core::database;
  postgres::PgBackendProvider rs{BackendIdentity{"redshift","PostgreSQL","synthetic"},
      BackendConnectionDefaults{"synthetic.invalid",5439,"synthetic",false},std::nullopt,postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider pg{BackendIdentity{"postgresql","Amazon Redshift","synthetic"},
      BackendConnectionDefaults{"synthetic.invalid",5432,"synthetic",false}};
  for(int i=0;i<3;++i) {
    const auto actual=rs.sql_dialect().translate_sql("SELECT {fn LOCATE(?,?)}"); ASSERT_TRUE(actual);
    EXPECT_EQ("SELECT POSITION(? IN ?)",actual.sql); EXPECT_EQ(2u,rs.sql_dialect().count_parameter_markers(actual.sql));
    EXPECT_EQ("SELECT LOCATE(?,?)",pg.sql_dialect().translate_sql("SELECT {fn LOCATE(?,?)}").sql);
  }
}


TEST(RedshiftLengthEscapeTest, UnaryStringCompositionKeepsExpressionAndMarkersOnce) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT LENGTH(RTRIM(?, ' '))",translate_odbc_sql("SELECT {fn LENGTH(?)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LENGTH(RTRIM(COALESCE(?, 'cat   '), ' '))",translate_odbc_sql("SELECT {fn LENGTH({fn IFNULL(?, 'cat   ')})}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LENGTH(RTRIM(POSITION(? IN ?), ' '))",translate_odbc_sql("SELECT {fn LENGTH({fn LOCATE(?,?)})}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LENGTH(RTRIM(NULL, ' ')), LENGTH(RTRIM('', ' ')), LENGTH(RTRIM('   ', ' '))",translate_odbc_sql("SELECT {fn LENGTH(NULL)}, {fn LENGTH('')}, {fn LENGTH('   ')}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftLengthEscapeTest, ExplicitSpaceRetainsTabLiteralAndEvaluatesRawExpressionOnce) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT LENGTH(RTRIM('a\t ', ' '))",translate_odbc_sql("SELECT {fn LENGTH('a\t ')}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LENGTH(RTRIM(nextval('unexecuted_length_sequence'), ' '))",translate_odbc_sql("SELECT {fn LENGTH(nextval('unexecuted_length_sequence'))}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftLengthEscapeTest, LexicalBodiesAndLineCommentNewlineSurviveBothClosers) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT LENGTH(RTRIM('a,b'-- kept\n, ' '))",translate_odbc_sql("SELECT {fn LENGTH('a,b'-- kept\n)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LENGTH(RTRIM($tag$a,b$tag$, ' '))",translate_odbc_sql("SELECT {fn LENGTH($tag$a,b$tag$)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LENGTH(RTRIM((lower('A,B') /* outer /* , */ kept */), ' '))",translate_odbc_sql("SELECT {fn LENGTH((lower('A,B') /* outer /* , */ kept */))}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT LENGTH(RTRIM(E'a\\'b,c', ' '))",translate_odbc_sql("SELECT {fn LENGTH(E'a\\'b,c')}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftLengthEscapeTest, ClosedUnaryArityRefusesMalformedCallsAndThenRecovers) {
  using rs::core::database::postgres::SqlDialectProfile;
  for(const auto* text:{"SELECT {fn LENGTH}","SELECT {fn LENGTH()}","SELECT {fn LENGTH(/*empty*/)}","SELECT {fn LENGTH(a,b)}","SELECT {fn LENGTH(a,)}","SELECT {fn LENGTH((a)}","SELECT {fn LENGTH(a) + 1}","SELECT {fn LENGTH('unclosed)}"}) {
    const auto r=translate_odbc_sql(text,SqlDialectProfile::Redshift);
    EXPECT_EQ(SqlTranslationError::InvalidSyntax,r.error);EXPECT_TRUE(r.sql.empty());
  }
  EXPECT_EQ("SELECT LENGTH(RTRIM('cat   ', ' '))",translate_odbc_sql("SELECT {fn LENGTH('cat   ')}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftLengthEscapeTest, DefaultNativeOtherFunctionsAndExactPrefixesRemainUnchanged) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT LENGTH(?)",translate_odbc_sql("SELECT {fn LENGTH(?)}").sql);
  EXPECT_EQ("SELECT LENGTH(?), CHAR_LENGTH(?), OCTET_LENGTH(?), LENGTHY(?)",translate_odbc_sql("SELECT LENGTH(?), {fn CHAR_LENGTH(?)}, {fn OCTET_LENGTH(?)}, {fn LENGTHY(?)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT '{fn LENGTH(a,b)}', $$ {fn LENGTH(a,b)} $$",translate_odbc_sql("SELECT '{fn LENGTH(a,b)}', $$ {fn LENGTH(a,b)} $$",SqlDialectProfile::Redshift).sql);
}


TEST(RedshiftCalendarEscapeTest, TypedLiteralComponentsNestedEscapesAndOriginalMarkersOnce) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT CAST(DATE_PART(year, DATE '2024-02-29') AS INTEGER), CAST(DATE_PART(month, TIMESTAMP '2023-12-31 00:00:01.000001') AS INTEGER), CAST(DATE_PART(day, ?) AS INTEGER)",
      translate_odbc_sql("SELECT {fn YEAR({d '2024-02-29'})}, {fn MONTH({ts '2023-12-31 00:00:01.000001'})}, {fn DAYOFMONTH(?)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(DATE_PART(year, COALESCE(?,DATE '2000-01-01')) AS INTEGER)",translate_odbc_sql("SELECT {fn YEAR({fn IFNULL(?,{d '2000-01-01'})})}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(DATE_PART(year, ?) AS INTEGER), CAST(DATE_PART(month, ?) AS INTEGER), CAST(DATE_PART(day, ?) AS INTEGER)",translate_odbc_sql("SELECT {fn YEAR(?)}, {fn MONTH(?)}, {fn DAYOFMONTH(?)}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftCalendarEscapeTest, RawLineCommentEndsBeforeCastAndLexicalCommasStayInsideArgument) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT CAST(DATE_PART(day, DATE '2024-02-29'-- kept\n) AS INTEGER)",translate_odbc_sql("SELECT {fn DAYOFMONTH({d '2024-02-29'}-- kept\n)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(DATE_PART(month, $tag$a,b$tag$) AS INTEGER)",translate_odbc_sql("SELECT {fn MONTH($tag$a,b$tag$)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(DATE_PART(year, (coalesce(?, DATE '2000-01-01') /* outer /* , */ kept */)) AS INTEGER)",translate_odbc_sql("SELECT {fn YEAR((coalesce(?, DATE '2000-01-01') /* outer /* , */ kept */))}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(DATE_PART(day, E'a\\'b,c') AS INTEGER)",translate_odbc_sql("SELECT {fn DAYOFMONTH(E'a\\'b,c')}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftCalendarEscapeTest, ThreeClosedUnaryNamesRejectMalformedCallsBeforeValidRecovery) {
  using rs::core::database::postgres::SqlDialectProfile;
  for(const auto* name:{"YEAR","MONTH","DAYOFMONTH"}) {
    for(const auto* args:{"","()","(/*empty*/)","(a,b)","(a,)","((a)","(a) + 1","('unclosed)"}) {
      const auto r=translate_odbc_sql(std::string("SELECT {fn ")+name+args+"}",SqlDialectProfile::Redshift);
      EXPECT_EQ(SqlTranslationError::InvalidSyntax,r.error);EXPECT_TRUE(r.sql.empty());
    }
  }
  EXPECT_EQ("SELECT CAST(DATE_PART(month, NULL) AS INTEGER)",translate_odbc_sql("SELECT {fn MONTH(NULL)}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftCalendarEscapeTest, ProfileNativePrefixesAndPreviousEscapesStayIsolated) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT YEAR(?), MONTH(?), DAYOFMONTH(?)",translate_odbc_sql("SELECT {fn YEAR(?)}, {fn MONTH(?)}, {fn DAYOFMONTH(?)}").sql);
  EXPECT_EQ("SELECT YEAR(?), DATE_PART(month, ?), YEARX(?), MONTHLY(?), DAYOFMONTHX(?)",translate_odbc_sql("SELECT YEAR(?), DATE_PART(month, ?), {fn YEARX(?)}, {fn MONTHLY(?)}, {fn DAYOFMONTHX(?)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT '{fn MONTH(a,b)}', $$ {fn YEAR(a,b)} $$, GETDATE(), LENGTH(RTRIM(?, ' ')), POSITION(? IN ?)",translate_odbc_sql("SELECT '{fn MONTH(a,b)}', $$ {fn YEAR(a,b)} $$, {fn NOW()}, {fn LENGTH(?)}, {fn LOCATE(?,?)}",SqlDialectProfile::Redshift).sql);
}


TEST(RedshiftClockEscapeTest, TimeTimestampPartsKeepFractionAndOriginalMarkersExactlyOnce) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(hour FROM TIME '23:59:59')) AS INTEGER), CAST(FLOOR(EXTRACT(minute FROM TIMESTAMP '2024-02-29 23:59:59.999999')) AS INTEGER), CAST(FLOOR(EXTRACT(second FROM ?)) AS INTEGER)",translate_odbc_sql("SELECT {fn HOUR({t '23:59:59'})}, {fn MINUTE({ts '2024-02-29 23:59:59.999999'})}, {fn SECOND(?)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(second FROM COALESCE(?,TIMESTAMP '2024-02-29 23:59:59.999999'))) AS INTEGER)",translate_odbc_sql("SELECT {fn SECOND({fn IFNULL(?,{ts '2024-02-29 23:59:59.999999'})})}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(hour FROM ?)) AS INTEGER), CAST(FLOOR(EXTRACT(minute FROM ?)) AS INTEGER), CAST(FLOOR(EXTRACT(second FROM ?)) AS INTEGER)",translate_odbc_sql("SELECT {fn HOUR(?)}, {fn MINUTE(?)}, {fn SECOND(?)}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftClockEscapeTest, RawLineCommentAndLexicalCommasDoNotSwallowExtractCloser) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(second FROM TIMESTAMP '2024-02-29 23:59:59.999999'-- kept\n)) AS INTEGER)",translate_odbc_sql("SELECT {fn SECOND({ts '2024-02-29 23:59:59.999999'}-- kept\n)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(minute FROM $tag$a,b$tag$)) AS INTEGER)",translate_odbc_sql("SELECT {fn MINUTE($tag$a,b$tag$)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(hour FROM (coalesce(?, TIME '12:08:43') /* outer /* , */ kept */))) AS INTEGER)",translate_odbc_sql("SELECT {fn HOUR((coalesce(?, TIME '12:08:43') /* outer /* , */ kept */))}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(second FROM E'a\\'b,c')) AS INTEGER)",translate_odbc_sql("SELECT {fn SECOND(E'a\\'b,c')}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftClockEscapeTest, ThreeClosedUnaryPartsRejectMalformedCallsAndRecover) {
  using rs::core::database::postgres::SqlDialectProfile;
  for(const auto* name:{"HOUR","MINUTE","SECOND"}) {
    for(const auto* args:{"","()","(/*empty*/)","(a,b)","(a,)","((a)","(a) + 1","('unclosed)"}) {
      const auto r=translate_odbc_sql(std::string("SELECT {fn ")+name+args+"}",SqlDialectProfile::Redshift);
      EXPECT_EQ(SqlTranslationError::InvalidSyntax,r.error);EXPECT_TRUE(r.sql.empty());
    }
  }
  EXPECT_EQ("SELECT CAST(FLOOR(EXTRACT(second FROM NULL)) AS INTEGER)",translate_odbc_sql("SELECT {fn SECOND(NULL)}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftClockEscapeTest, ProfilesNativePrefixesAndPreviousPackagesRemainUnchanged) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT HOUR(?), MINUTE(?), SECOND(?)",translate_odbc_sql("SELECT {fn HOUR(?)}, {fn MINUTE(?)}, {fn SECOND(?)}").sql);
  EXPECT_EQ("SELECT HOUR(?), EXTRACT(second FROM ?), HOURX(?), MINUTES(?), SECONDLY(?)",translate_odbc_sql("SELECT HOUR(?), EXTRACT(second FROM ?), {fn HOURX(?)}, {fn MINUTES(?)}, {fn SECONDLY(?)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("SELECT '{fn SECOND(a,b)}', $$ {fn HOUR(a,b)} $$, CAST(DATE_PART(year, ?) AS INTEGER), GETDATE(), LENGTH(RTRIM(?, ' ')), POSITION(? IN ?)",translate_odbc_sql("SELECT '{fn SECOND(a,b)}', $$ {fn HOUR(a,b)} $$, {fn YEAR(?)}, {fn NOW()}, {fn LENGTH(?)}, {fn LOCATE(?,?)}",SqlDialectProfile::Redshift).sql);
}

TEST(RedshiftDayOrdinalEscapeTest, SundayBasedWeekdayAndYearOrdinalUseExpressionOnce) {
  using rs::core::database::postgres::SqlDialectProfile;
  const auto weekday=rs::core::database::postgres::translate_odbc_sql("SELECT {fn DAYOFWEEK(?)}",SqlDialectProfile::Redshift);
  ASSERT_TRUE(weekday);EXPECT_EQ("SELECT (EXTRACT(dow FROM ?)+1)",weekday.sql);
  EXPECT_EQ(1,std::count(weekday.sql.begin(),weekday.sql.end(),'?'));
  const auto ordinal=rs::core::database::postgres::translate_odbc_sql("{fn dayofyear({d '2024-02-29'})}",SqlDialectProfile::Redshift);
  ASSERT_TRUE(ordinal);EXPECT_EQ("EXTRACT(doy FROM DATE '2024-02-29')",ordinal.sql);
  const auto nested=rs::core::database::postgres::translate_odbc_sql("{fn DAYOFWEEK({fn IFNULL(?,{ts '2024-01-07 12:34:56.123456'})})}",SqlDialectProfile::Redshift);
  ASSERT_TRUE(nested);EXPECT_EQ("(EXTRACT(dow FROM COALESCE(?,TIMESTAMP '2024-01-07 12:34:56.123456'))+1)",nested.sql);
  EXPECT_EQ(1,std::count(nested.sql.begin(),nested.sql.end(),'?'));
}
TEST(RedshiftDayOrdinalEscapeTest, RawCommentNewlineQuotedCommaAndDollarBodiesRemainIntact) {
  using rs::core::database::postgres::SqlDialectProfile;
  for(const auto& item:std::array<std::pair<const char*,const char*>,3>{{
      {"{fn DAYOFWEEK(? -- end\n)}","(EXTRACT(dow FROM ? -- end\n)+1)"},
      {"{fn DAYOFYEAR(/* outer /* nested */ end */ COALESCE(?,DATE '2024-01-01'))}","EXTRACT(doy FROM /* outer /* nested */ end */ COALESCE(?,DATE '2024-01-01'))"},
      {"{fn DAYOFYEAR(COALESCE($q$a,b$q$,?))}","EXTRACT(doy FROM COALESCE($q$a,b$q$,?))"}}}){
    const auto result=rs::core::database::postgres::translate_odbc_sql(item.first,SqlDialectProfile::Redshift);
    ASSERT_TRUE(result);EXPECT_EQ(item.second,result.sql);
  }
}
TEST(RedshiftDayOrdinalEscapeTest, ExactUnaryMalformedCallsRefuseThenValidRecovery) {
  using rs::core::database::postgres::SqlDialectProfile;using rs::core::database::SqlTranslationError;
  for(const auto* sql:{"{fn DAYOFWEEK}","{fn DAYOFWEEK()}","{fn DAYOFWEEK( ,?)}","{fn DAYOFYEAR(?,?)}",
      "{fn DAYOFYEAR(?) trailing}","{fn DAYOFWEEK(/* comment */)}","{fn DAYOFYEAR('unclosed)}"}){
    const auto result=rs::core::database::postgres::translate_odbc_sql(sql,SqlDialectProfile::Redshift);
    EXPECT_FALSE(result);EXPECT_EQ(SqlTranslationError::InvalidSyntax,result.error);EXPECT_TRUE(result.sql.empty());
  }
  const auto result=rs::core::database::postgres::translate_odbc_sql("{fn DAYOFYEAR(NULL)}",SqlDialectProfile::Redshift);
  ASSERT_TRUE(result);EXPECT_EQ("EXTRACT(doy FROM NULL)",result.sql);
}
TEST(RedshiftDayOrdinalEscapeTest, PostgresUnknownPrefixesAndNativeSqlAreUnchanged) {
  using rs::core::database::postgres::SqlDialectProfile;
  const auto pg=rs::core::database::postgres::translate_odbc_sql("{fn DAYOFWEEK(?)}");ASSERT_TRUE(pg);EXPECT_EQ("DAYOFWEEK(?)",pg.sql);
  for(const auto* sql:{"SELECT EXTRACT(doy FROM d)","SELECT 'DAYOFWEEK(?)'","{fn DAYOFWEEK_EXTRA(?)}"}){
    const auto result=rs::core::database::postgres::translate_odbc_sql(sql,SqlDialectProfile::Redshift);ASSERT_TRUE(result);
    EXPECT_EQ(std::string(sql)=="{fn DAYOFWEEK_EXTRA(?)}"?"DAYOFWEEK_EXTRA(?)":sql,result.sql);
  }
  const auto prior=rs::core::database::postgres::translate_odbc_sql("{fn SECOND(?)}",SqlDialectProfile::Redshift);
  ASSERT_TRUE(prior);EXPECT_EQ("CAST(FLOOR(EXTRACT(second FROM ?)) AS INTEGER)",prior.sql);
}

TEST(RedshiftLogarithmEscapeTest, NaturalAndBaseTenMapOnceWithoutCastOrBaseArithmetic) {
  using rs::core::database::postgres::SqlDialectProfile;
  const auto result=translate_odbc_sql("SELECT {fn LOG(?)},{fn LOG10(?)}",SqlDialectProfile::Redshift);
  ASSERT_TRUE(result);EXPECT_EQ("SELECT LN(?),LOG(?)",result.sql);EXPECT_EQ(2,std::count(result.sql.begin(),result.sql.end(),'?'));
  EXPECT_EQ("LN(COALESCE(?,100))",translate_odbc_sql("{fn lOg({fn IFNULL(?,100)})}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("LOG(100)",translate_odbc_sql("{fn log10(100)}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftLogarithmEscapeTest, RawLineCommentsQuotesNestedCommentsAndDollarBodiesAreRetained) {
  using rs::core::database::postgres::SqlDialectProfile;
  for(const auto& item:std::array<std::pair<const char*,const char*>,4>{{
      {"{fn LOG(? -- end\n)}","LN(? -- end\n)"},
      {"{fn LOG10(/* a /* b */ c */?)}","LOG(/* a /* b */ c */?)"},
      {"{fn LOG(COALESCE($q$a,b$q$,?))}","LN(COALESCE($q$a,b$q$,?))"},
      {"{fn LOG10(COALESCE('a,b',?))}","LOG(COALESCE('a,b',?))"}}}){
    const auto result=translate_odbc_sql(item.first,SqlDialectProfile::Redshift);ASSERT_TRUE(result);EXPECT_EQ(item.second,result.sql);
  }
}
TEST(RedshiftLogarithmEscapeTest, MalformedUnaryCallsRefuseAndPositiveSmallRecoveryRemainsNative) {
  using rs::core::database::postgres::SqlDialectProfile;
  for(const auto* sql:{"{fn LOG}","{fn LOG()}","{fn LOG(?,?)}","{fn LOG10(,?)}","{fn LOG10(?) extra}","{fn LOG(/*empty*/)}","{fn LOG('unclosed)}"}){
    const auto result=translate_odbc_sql(sql,SqlDialectProfile::Redshift);EXPECT_EQ(SqlTranslationError::InvalidSyntax,result.error);EXPECT_TRUE(result.sql.empty());
  }
  EXPECT_EQ("LN(1e-6)",translate_odbc_sql("{fn LOG(1e-6)}",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("LOG(NULL)",translate_odbc_sql("{fn LOG10(NULL)}",SqlDialectProfile::Redshift).sql);
}
TEST(RedshiftLogarithmEscapeTest, PostgresNativeLogAndUnknownPrefixesAndPriorFamiliesAreUnchanged) {
  using rs::core::database::postgres::SqlDialectProfile;
  EXPECT_EQ("SELECT LOG(?),LOG10(?)",translate_odbc_sql("SELECT {fn LOG(?)},{fn LOG10(?)}").sql);
  EXPECT_EQ("SELECT LOG(2,100),LN(x),LOG_EXTRA(?),'LOG10(?)'",translate_odbc_sql("SELECT LOG(2,100),LN(x),{fn LOG_EXTRA(?)},'LOG10(?)'",SqlDialectProfile::Redshift).sql);
  EXPECT_EQ("(EXTRACT(dow FROM ?)+1),EXTRACT(doy FROM ?),CAST(FLOOR(EXTRACT(second FROM ?)) AS INTEGER)",translate_odbc_sql("{fn DAYOFWEEK(?)},{fn DAYOFYEAR(?)},{fn SECOND(?)}",SqlDialectProfile::Redshift).sql);
}


TEST(SqlCapabilityEscapeProof, ScalarTranslationsRemainAvailableIndependentlyOfQualifiedClaims) {
  using namespace rs::core::database;
  using postgres::SqlDialectProfile;
  struct Case {
    bool SqlCapabilities::*field;
    const char* escaped;
    const char* postgres_sql;
    const char* redshift_sql;
  };
  const Case cases[]{
      {&SqlCapabilities::scalar_upper, "{fn UCASE('Ab')}", "UPPER('Ab')", "UPPER('Ab')"},
      {&SqlCapabilities::scalar_lower, "{fn LCASE('Ab')}", "LOWER('Ab')", "LOWER('Ab')"},
      {&SqlCapabilities::scalar_if_null, "{fn IFNULL(NULL,7)}", "COALESCE(NULL,7)", "COALESCE(NULL,7)"},
      {&SqlCapabilities::scalar_current_date, "{fn CURDATE()}", "CURRENT_DATE", "CURRENT_DATE"},
      {&SqlCapabilities::scalar_current_time, "{fn CURTIME()}", "CURRENT_TIME", "CAST(GETDATE() AS TIME)"},
      {&SqlCapabilities::scalar_current_timestamp, "{fn NOW()}", "CURRENT_TIMESTAMP", "GETDATE()"},
      {&SqlCapabilities::scalar_trimmed_length, "{fn LENGTH('cat   ')}", nullptr, "LENGTH(RTRIM('cat   ', ' '))"},
      {&SqlCapabilities::scalar_locate_two_arguments, "{fn LOCATE('at','cat')}", nullptr, "POSITION('at' IN 'cat')"},
      {&SqlCapabilities::scalar_natural_log, "{fn LOG(100)}", nullptr, "LN(100)"},
      {&SqlCapabilities::scalar_log10, "{fn LOG10(100)}", nullptr, "LOG(100)"},
      {&SqlCapabilities::scalar_year, "{fn YEAR(?)}", nullptr, "CAST(DATE_PART(year, ?) AS INTEGER)"},
      {&SqlCapabilities::scalar_month, "{fn MONTH(?)}", nullptr, "CAST(DATE_PART(month, ?) AS INTEGER)"},
      {&SqlCapabilities::scalar_day, "{fn DAYOFMONTH(?)}", nullptr, "CAST(DATE_PART(day, ?) AS INTEGER)"},
      {&SqlCapabilities::scalar_weekday, "{fn DAYOFWEEK(?)}", nullptr, "(EXTRACT(dow FROM ?)+1)"},
      {&SqlCapabilities::scalar_yearday, "{fn DAYOFYEAR(?)}", nullptr, "EXTRACT(doy FROM ?)"},
      {&SqlCapabilities::scalar_hour, "{fn HOUR(?)}", nullptr, "CAST(FLOOR(EXTRACT(hour FROM ?)) AS INTEGER)"},
      {&SqlCapabilities::scalar_minute, "{fn MINUTE(?)}", nullptr, "CAST(FLOOR(EXTRACT(minute FROM ?)) AS INTEGER)"},
      {&SqlCapabilities::scalar_second, "{fn SECOND(?)}", nullptr, "CAST(FLOOR(EXTRACT(second FROM ?)) AS INTEGER)"},
  };
  for (const bool redshift : {false, true}) {
    const auto capabilities = redshift ? postgres::redshift_backend_capabilities("misleading")
                                      : postgres::pg_backend_capabilities("misleading");
    const auto profile = redshift ? SqlDialectProfile::Redshift : SqlDialectProfile::PostgreSql;
    for (const auto& item : cases) {
      SCOPED_TRACE(item.escaped);
      SCOPED_TRACE(redshift);
      const auto* expected = redshift ? item.redshift_sql : item.postgres_sql;
      // A translation can remain usable while its SQLGetInfo claim awaits live proof.
      const bool qualified = !redshift && expected != nullptr;
      EXPECT_EQ(qualified, capabilities.sql.*(item.field));
      if (expected != nullptr) {
        const auto result = translate_odbc_sql(item.escaped, profile);
        ASSERT_TRUE(result) << result.message;
        EXPECT_EQ(expected, result.sql);
      }
    }
  }
}

TEST(SqlCapabilityEscapeProof, JoinTranslationFormsStripOnlyTheEscapeWrapper) {
  using rs::core::database::postgres::SqlDialectProfile;
  for (const auto profile : {SqlDialectProfile::PostgreSql, SqlDialectProfile::Redshift}) {
    for (const auto* native : {
        "a LEFT OUTER JOIN b ON a.x=b.y",
        "a LEFT JOIN b ON a.x=b.y",
        "a RIGHT OUTER JOIN b ON a.x=b.y",
        "a FULL OUTER JOIN b ON a.x=b.y",
        "a LEFT OUTER JOIN b ON b.y=a.x",
        "a LEFT OUTER JOIN (b LEFT OUTER JOIN c ON b.y=c.z) ON a.x=b.y",
        "a LEFT OUTER JOIN (b INNER JOIN c ON b.y=c.z) ON a.x=b.y"}) {
      SCOPED_TRACE(native);
      const auto result = translate_odbc_sql("{oj " + std::string(native) + "}", profile);
      ASSERT_TRUE(result) << result.message;
      EXPECT_EQ(native, result.sql);
    }
  }
}

// Golden SQL for one second-based and one calendar unit per product (#144
// review), so a wrong unit scale or operand order fails here and not only in
// a live run. PostgreSQL counts unit boundaries as f(b) - f(a) over values
// truncated to the unit; month and quarter use AGE from 2000-01-01, whose
// total months times 12 (or 4) are read back exactly as the YEAR field.
TEST(SqlTimestampEscapeTest, GoldenSqlPerProduct) {
  using rs::core::database::postgres::SqlDialectProfile;
  const auto pg_ts = [](const std::string& x) {
    return "CAST(REGEXP_REPLACE(BTRIM(CAST(( " + x + ") AS TEXT)) COLLATE \"C\", '^(?=[0-9]{2}:)', "
           "CAST(CURRENT_DATE AS TEXT) || ' ') AS TIMESTAMP)";
  };
  const auto rs_ts = [](const std::string& x) {
    return "CAST(REGEXP_REPLACE(BTRIM(CAST(( " + x + ") AS VARCHAR(MAX))), '^(?=[0-9]{2}:)', "
           "CAST(CURRENT_DATE AS VARCHAR(10)) || ' ', 1, 'p') AS TIMESTAMP)";
  };
  const auto pg_months = [&](const std::string& unit, const std::string& x, const char* factor) {
    return "EXTRACT(YEAR FROM (AGE(DATE_TRUNC('" + unit + "', " + pg_ts(x) + "), TIMESTAMP '2000-01-01') * " +
           factor + "))";
  };
  const std::tuple<SqlDialectProfile, const char*, std::string> cases[]{
      {SqlDialectProfile::PostgreSql, "SELECT {fn TIMESTAMPDIFF(SQL_TSI_SECOND, a, b)}",
       "SELECT CAST(-((EXTRACT(EPOCH FROM DATE_TRUNC('second', " + pg_ts("a") + ")) / 1) - (EXTRACT(EPOCH FROM "
       "DATE_TRUNC('second', " + pg_ts("b") + ")) / 1)) AS BIGINT)"},
      {SqlDialectProfile::PostgreSql, "SELECT {fn TIMESTAMPDIFF(SQL_TSI_MONTH, a, b)}",
       "SELECT CAST(-(" + pg_months("month", "a", "12") + " - " + pg_months("month", "b", "12") + ") AS BIGINT)"},
      {SqlDialectProfile::PostgreSql, "SELECT {fn TIMESTAMPDIFF(SQL_TSI_QUARTER, a, b)}",
       "SELECT CAST(-(" + pg_months("quarter", "a", "4") + " - " + pg_months("quarter", "b", "4") + ") AS BIGINT)"},
      {SqlDialectProfile::PostgreSql, "SELECT {fn TIMESTAMPADD(SQL_TSI_SECOND, n, t)}",
       "SELECT (CAST(( n) AS BIGINT) * INTERVAL '1 second' + " + pg_ts("t") + ")"},
      {SqlDialectProfile::PostgreSql, "SELECT {fn TIMESTAMPADD(SQL_TSI_QUARTER, n, t)}",
       "SELECT (CAST(( n) AS BIGINT) * INTERVAL '3 months' + " + pg_ts("t") + ")"},
      {SqlDialectProfile::Redshift, "SELECT {fn TIMESTAMPDIFF(SQL_TSI_SECOND, a, b)}",
       "SELECT DATEDIFF(second, " + rs_ts("a") + ", " + rs_ts("b") + ")"},
      {SqlDialectProfile::Redshift, "SELECT {fn TIMESTAMPDIFF(SQL_TSI_QUARTER, a, b)}",
       "SELECT DATEDIFF(quarter, " + rs_ts("a") + ", " + rs_ts("b") + ")"},
      {SqlDialectProfile::Redshift, "SELECT {fn TIMESTAMPADD(SQL_TSI_SECOND, n, t)}",
       "SELECT DATEADD(second, CAST(( n) AS INTEGER), " + rs_ts("t") + ")"},
      {SqlDialectProfile::Redshift, "SELECT {fn TIMESTAMPADD(SQL_TSI_QUARTER, n, t)}",
       "SELECT DATEADD(quarter, CAST(( n) AS INTEGER), " + rs_ts("t") + ")"},
  };
  for (const auto& [profile, sql, expected] : cases) {
    const auto result = translate_odbc_sql(sql, profile);
    ASSERT_TRUE(result) << result.message << sql;
    EXPECT_EQ(expected, result.sql) << sql;
  }
}
