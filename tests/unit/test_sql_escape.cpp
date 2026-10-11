#include <gtest/gtest.h>

#include "core/database/postgres/pg_sql_dialect.h"
#include "core/database/postgres/pg_backend_provider.h"

using rs::core::database::SqlTranslationError;
using rs::core::database::postgres::translate_odbc_sql;

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
