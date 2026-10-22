// Redshift SUPER and spatial result/parameter metadata through the actual
// driver API, a real Redshift-profile PostgreSQL-family session and literal
// protocol frames from a local in-process wire. No database service is used.
// Native references: AWS ODBC 868e9b0d035f5e219890c2de424b9d29d130f300
// rsodbc.h:176-182 (SUPER 4000, GEOMETRY 3000, GEOGRAPHY 3001,
// GEOMETRYHEX 3999). See docs/redshift-special-types.md.
#include "tests/support/redshift_scripted_wire.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace rs::core::database;
using namespace rs::tests::redshift_wire;

class RedshiftSpecialTypesTest : public RedshiftScriptedSessionTest {};

// Server-serialized SUPER JSON text; SQL NULL differs from JSON null.
const std::vector<std::optional<std::string>> kSuperValues{
    std::string(R"({"a":1,"b":[true,null],"c":{"d":"x"}})"),
    std::string(R"([1,"abc",null])"),
    std::string(R"("text scalar")"),
    std::string("null"),
    std::nullopt,
    std::string("\"\xe2\x82\xac\xf0\x9f\x98\x80\\u0000\""),
    std::string("123.45"),
    std::string("\"\""),
};

std::string super_response(const std::vector<std::optional<std::string>>& values) {
  std::string response = row_description({{"payload", 4000}});
  for (const auto& value : values) response += data_row({value});
  return response + complete("SELECT " + std::to_string(values.size()));
}

TEST_F(RedshiftSpecialTypesTest, SuperResultMetadataIsLongVarcharNamedSuper) {
  script->responses["SELECT payload FROM super_fixture"] = super_response(kSuperValues);
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT payload FROM super_fixture"));
  SQLSMALLINT type = -1, digits = -1, nullable = -1;
  SQLULEN size = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 1, nullptr, 0, nullptr, &type, &size, &digits, &nullable));
  EXPECT_EQ(SQL_LONGVARCHAR, type);
  EXPECT_EQ(0u, size); // The 16 MB object bound is not a per-column length.
  EXPECT_EQ(0, digits);
  EXPECT_EQ("super", column_text(1, SQL_DESC_TYPE_NAME));
  EXPECT_EQ("super", column_text(1, SQL_DESC_LOCAL_TYPE_NAME));
  EXPECT_EQ(SQL_LONGVARCHAR, column_number(1, SQL_DESC_CONCISE_TYPE));
  EXPECT_EQ(SQL_NO_TOTAL, column_number(1, SQL_DESC_OCTET_LENGTH));
  EXPECT_EQ(SQL_NO_TOTAL, column_number(1, SQL_DESC_DISPLAY_SIZE));
  EXPECT_EQ(SQL_TRUE, column_number(1, SQL_DESC_CASE_SENSITIVE));
  // Name-derived fields follow the advertised "super" row, not the "text" family.
  EXPECT_EQ("", column_text(1, SQL_DESC_LITERAL_PREFIX));
  EXPECT_EQ("", column_text(1, SQL_DESC_LITERAL_SUFFIX));
  SQLHDESC ird = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_IMP_ROW_DESC, &ird, 0, nullptr));
  char name[16]{};
  SQLINTEGER name_length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ird, 1, SQL_DESC_TYPE_NAME, name, sizeof(name), &name_length));
  EXPECT_STREQ("super", name);
  EXPECT_EQ(5, name_length);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(RedshiftSpecialTypesTest, SuperValuesRoundTripExactlyThroughCharWideAndDefault) {
  script->responses["SELECT payload FROM super_fixture"] = super_response(kSuperValues);
  for (const SQLSMALLINT target : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_DEFAULT}, SQLSMALLINT{SQL_C_WCHAR}}) {
    SCOPED_TRACE(target);
    ASSERT_EQ(SQL_SUCCESS, execute("SELECT payload FROM super_fixture"));
    for (const auto& expected : kSuperValues) {
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      alignas(SQLWCHAR) unsigned char buffer[256];
      std::fill(std::begin(buffer), std::end(buffer), 0x5a);
      SQLLEN length = -7;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, target, buffer, sizeof(buffer), &length));
      if (!expected) {
        EXPECT_EQ(SQL_NULL_DATA, length);
        continue;
      }
      if (target == SQL_C_WCHAR) {
        const auto wide = rs::odbc::utf8_to_wide(*expected);
        ASSERT_TRUE(wide);
        ASSERT_EQ(static_cast<SQLLEN>(wide->size() * sizeof(SQLWCHAR)), length);
        EXPECT_EQ(0, std::memcmp(buffer, wide->data(), static_cast<std::size_t>(length)));
      } else {
        ASSERT_EQ(static_cast<SQLLEN>(expected->size()), length);
        EXPECT_EQ(0, std::memcmp(buffer, expected->data(), expected->size()));
        EXPECT_EQ(0, buffer[expected->size()]);
      }
      EXPECT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, target, buffer, sizeof(buffer), &length));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(RedshiftSpecialTypesTest, LargeSuperChunksAcrossSQLGetDataAndBoundTruncation) {
  std::string large = "{\"k\":\"";
  for (std::size_t i = 0; large.size() < 70000; ++i) {
    large += (i % 7 == 0) ? "\xc3\xa9" : "x"; // Multibyte characters straddle chunks.
  }
  large += "\"}";
  script->responses["SELECT payload FROM super_large"] =
      super_response({large, std::string(R"({"next":true})")});
  for (const SQLSMALLINT target : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
    SCOPED_TRACE(target);
    ASSERT_EQ(SQL_SUCCESS, execute("SELECT payload FROM super_large"));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    const auto bytes = chunked(1, target, target == SQL_C_WCHAR ? 4096 * sizeof(SQLWCHAR) : 4096);
    if (target == SQL_C_WCHAR) {
      const auto wide = rs::odbc::utf8_to_wide(large);
      ASSERT_TRUE(wide);
      ASSERT_EQ(wide->size() * sizeof(SQLWCHAR), bytes.size());
      EXPECT_EQ(0, std::memcmp(bytes.data(), wide->data(), bytes.size()));
    } else {
      EXPECT_EQ(large, bytes);
    }
    // The next row remains intact after chunked consumption.
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    char next[32]{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, next, sizeof(next), &length));
    EXPECT_STREQ(R"({"next":true})", next);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  // Bound fetch reports the full length, truncates and continues to the next row.
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT payload FROM super_large"));
  char bound[16]{};
  SQLLEN indicator = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_CHAR, bound, sizeof(bound), &indicator));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  EXPECT_EQ("01004", state());
  EXPECT_EQ(static_cast<SQLLEN>(large.size()), indicator);
  EXPECT_EQ(0, std::memcmp(bound, large.data(), sizeof(bound) - 1));
  EXPECT_EQ('\0', bound[sizeof(bound) - 1]);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(13, indicator);
  EXPECT_STREQ(R"({"next":true})", bound);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(RedshiftSpecialTypesTest, MalformedSuperTextIsADeferredDataErrorWithRecovery) {
  script->responses["SELECT payload FROM super_bad"] =
      super_response({std::string("\"\xff\""), std::string("[2]")});
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT payload FROM super_bad"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  char buffer[16];
  std::fill(std::begin(buffer), std::end(buffer), 'Z');
  SQLLEN length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &length));
  EXPECT_EQ("22018", state());
  EXPECT_EQ(73, length);
  EXPECT_EQ('Z', buffer[0]);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &length));
  EXPECT_STREQ("[2]", buffer);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(RedshiftSpecialTypesTest, SuperResultsNeverIssueTypeDiscoverySql) {
  script->responses["SELECT payload FROM super_fixture"] = super_response(kSuperValues);
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT payload FROM super_fixture"));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  EXPECT_EQ((std::vector<std::string>{"SELECT payload FROM super_fixture"}), script->received_sql);
}

TEST_F(RedshiftSpecialTypesTest, SuperParameterDescriptionIsNamedWithoutSuperBinding) {
  // ParameterDescription OID 4000 is server metadata, not a SUPER bind type.
  script->responses["INSERT INTO super_fixture VALUES ($1)"] =
      frame('1', "") + frame('t', be16(1) + be32(4000)) + frame('n', "");
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>("INSERT INTO super_fixture VALUES (?)")), SQL_NTS));
  SQLSMALLINT type = -1, digits = -1, nullable = -1;
  SQLULEN size = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(SQL_LONGVARCHAR, type);
  EXPECT_EQ(0u, size);
  SQLHDESC ipd = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_IMP_PARAM_DESC, &ipd, 0, nullptr));
  char name[16]{};
  SQLINTEGER name_length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ipd, 1, SQL_DESC_TYPE_NAME, name, sizeof(name), &name_length));
  EXPECT_STREQ("super", name);
  for (const auto& sql : script->received_sql) {
    EXPECT_EQ(std::string::npos, sql.find("pg_type")) << sql; // Known OID: no discovery.
  }
}

TEST_F(RedshiftSpecialTypesTest, TypeInfoAdvertisesSuperAsLongVarchar) {
  ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(stmt, SQL_LONGVARCHAR));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  char name[32]{};
  SQLLEN length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, name, sizeof(name), &length));
  EXPECT_STREQ("super", name);
  SQLINTEGER size = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_LONG, &size, sizeof(size), &length));
  EXPECT_EQ(16777216, size); // Documented SUPER object limit.
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 4, SQL_C_CHAR, name, sizeof(name), &length));
  EXPECT_EQ(SQL_NULL_DATA, length); // No SUPER literal syntax is advertised.
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  EXPECT_TRUE(script->received_sql.empty());
}


// EWKB for POINT(1 2) without SRID and GEOGRAPHY POINT(1 1) SRID 4326; the
// first contains zero bytes. Hex forms from AWS Python integration data.
const std::string kPointEwkb = rs::util::decode_hex("0101000000000000000000F03F0000000000000040").value();
const std::string kGeographyEwkb = rs::util::decode_hex("0101000020E6100000000000000000F03F000000000000F03F").value();

std::string spatial_response() {
  // OID 3000 carries raw EWKB; 3999 and 3001 carry hexadecimal EWKB text.
  std::string response = row_description({{"geom", 3000}, {"geomhex", 3999}, {"geog", 3001}});
  response += data_row({kPointEwkb, std::string("0101000000000000000000f03f0000000000000040"),
                        std::string("0101000020E6100000000000000000F03F000000000000F03F")});
  response += data_row({std::nullopt, std::nullopt, std::nullopt});
  response += data_row({std::string(), std::string(), std::string()});
  return response + complete("SELECT 3");
}

TEST_F(RedshiftSpecialTypesTest, SpatialResultMetadataIsUnsizedLongVarbinaryWithNativeNames) {
  script->responses["SELECT spatial"] = spatial_response();
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT spatial"));
  const char* names[]{"geometry", "geometry", "geography"};
  for (SQLUSMALLINT column = 1; column <= 3; ++column) {
    SCOPED_TRACE(column);
    SQLSMALLINT type = -1, digits = -1;
    SQLULEN size = 99;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, column, nullptr, 0, nullptr, &type, &size, &digits, nullptr));
    EXPECT_EQ(SQL_LONGVARBINARY, type);
    EXPECT_EQ(0u, size); // No fixed maximum is advertised per column.
    EXPECT_EQ(0, digits);
    EXPECT_EQ(names[column - 1], column_text(column, SQL_DESC_TYPE_NAME));
    EXPECT_EQ(names[column - 1], column_text(column, SQL_DESC_LOCAL_TYPE_NAME));
    EXPECT_EQ(SQL_NO_TOTAL, column_number(column, SQL_DESC_OCTET_LENGTH));
    EXPECT_EQ(SQL_NO_TOTAL, column_number(column, SQL_DESC_DISPLAY_SIZE));
  }
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  EXPECT_EQ((std::vector<std::string>{"SELECT spatial"}), script->received_sql);
}

TEST_F(RedshiftSpecialTypesTest, SpatialValuesAreEwkbBytesForBinaryAndDefaultAndHexForCharacter) {
  script->responses["SELECT spatial"] = spatial_response();
  const std::string expected[]{kPointEwkb, kPointEwkb, kGeographyEwkb};
  for (const SQLSMALLINT target : {SQLSMALLINT{SQL_C_BINARY}, SQLSMALLINT{SQL_C_DEFAULT},
                                   SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
    SCOPED_TRACE(target);
    ASSERT_EQ(SQL_SUCCESS, execute("SELECT spatial"));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    for (SQLUSMALLINT column = 1; column <= 3; ++column) {
      SCOPED_TRACE(column);
      alignas(SQLWCHAR) unsigned char buffer[512];
      std::fill(std::begin(buffer), std::end(buffer), 0x5a);
      SQLLEN length = -7;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, column, target, buffer, sizeof(buffer), &length));
      const auto& bytes = expected[column - 1];
      if (target == SQL_C_BINARY || target == SQL_C_DEFAULT) {
        ASSERT_EQ(static_cast<SQLLEN>(bytes.size()), length);
        EXPECT_EQ(0, std::memcmp(buffer, bytes.data(), bytes.size()));
        EXPECT_EQ(0x5a, buffer[bytes.size()]);
        continue;
      }
      // The existing generic binary-to-character conversion (also VARBYTE).
      const auto hex_text = rs::util::encode_hex(bytes);
      if (target == SQL_C_WCHAR) {
        const auto wide = rs::odbc::utf8_to_wide(hex_text);
        ASSERT_TRUE(wide);
        ASSERT_EQ(static_cast<SQLLEN>(wide->size() * sizeof(SQLWCHAR)), length);
        EXPECT_EQ(0, std::memcmp(buffer, wide->data(), static_cast<std::size_t>(length)));
      } else {
        ASSERT_EQ(static_cast<SQLLEN>(hex_text.size()), length);
        EXPECT_EQ(hex_text, std::string(reinterpret_cast<char*>(buffer), hex_text.size()));
      }
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    for (SQLUSMALLINT column = 1; column <= 3; ++column) {
      unsigned char buffer[8]{};
      SQLLEN length = -7;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, column, target, buffer, sizeof(buffer), &length));
      EXPECT_EQ(SQL_NULL_DATA, length);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    for (SQLUSMALLINT column = 1; column <= 3; ++column) {
      alignas(SQLWCHAR) unsigned char buffer[8]{};
      SQLLEN length = -7;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, column, target, buffer, sizeof(buffer), &length));
      EXPECT_EQ(0, length); // Empty payload is distinct from SQL NULL.
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(RedshiftSpecialTypesTest, LargeSpatialPayloadChunksAndBoundTruncationReportFullLength) {
  // A synthetic multi-chunk EWKB-shaped payload: byte-order flag then all octets.
  std::string large(1, '\1');
  for (std::size_t i = 0; large.size() < 70001; ++i) large.push_back(static_cast<char>(i & 0xff));
  const auto large_hex = rs::util::encode_hex(large);
  script->responses["SELECT large_spatial"] =
      row_description({{"geom", 3000}, {"geog", 3001}}) +
      data_row({large, large_hex}) + data_row({kPointEwkb, std::nullopt}) + complete("SELECT 2");
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT large_spatial"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(large, chunked(1, SQL_C_BINARY, 4096));
  EXPECT_EQ(large, chunked(2, SQL_C_BINARY, 4093)); // Odd chunk sizes keep byte offsets.
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT large_spatial"));
  unsigned char bound[16];
  SQLLEN indicator = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_BINARY, bound, sizeof(bound), &indicator));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  EXPECT_EQ("01004", state());
  EXPECT_EQ(static_cast<SQLLEN>(large.size()), indicator);
  EXPECT_EQ(0, std::memcmp(bound, large.data(), sizeof(bound)));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt)); // 21-byte EWKB into 16 bytes.
  EXPECT_EQ(static_cast<SQLLEN>(kPointEwkb.size()), indicator);
  EXPECT_EQ(0, std::memcmp(bound, kPointEwkb.data(), sizeof(bound)));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(RedshiftSpecialTypesTest, InconsistentSpatialPayloadsAreDeferredDataErrorsNotCorruption) {
  // Hex text under raw OID 3000, odd/invalid hex under 3999/3001: never
  // silently returned as ASCII or partially decoded bytes.
  script->responses["SELECT bad_spatial"] =
      row_description({{"geom", 3000}, {"geomhex", 3999}, {"geog", 3001}}) +
      data_row({std::string("0101000000000000000000F03F0000000000000040"),
                std::string("0101zz"), std::string("010")}) +
      data_row({kPointEwkb, std::string("01"), std::string("01")}) + complete("SELECT 2");
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT bad_spatial"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); // Unrequested malformed cells are deferred.
  for (SQLUSMALLINT column = 1; column <= 3; ++column) {
    SCOPED_TRACE(column);
    for (const SQLSMALLINT target : {SQLSMALLINT{SQL_C_BINARY}, SQLSMALLINT{SQL_C_CHAR}}) {
      unsigned char buffer[64];
      std::fill(std::begin(buffer), std::end(buffer), 0x5a);
      SQLLEN length = 73;
      EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, column, target, buffer, sizeof(buffer), &length));
      EXPECT_EQ("22018", state());
      EXPECT_EQ(73, length);
      EXPECT_EQ(0x5a, buffer[0]);
    }
  }
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  unsigned char buffer[64]{};
  SQLLEN length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY, buffer, sizeof(buffer), &length));
  EXPECT_EQ(static_cast<SQLLEN>(kPointEwkb.size()), length);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_BINARY, buffer, sizeof(buffer), &length));
  EXPECT_EQ(1, length);
  EXPECT_EQ(0x01, buffer[0]);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(RedshiftSpecialTypesTest, TypeInfoAdvertisesSpatialAfterVarbyteWithDocumentedLimit) {
  ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(stmt, SQL_LONGVARBINARY));
  const std::pair<const char*, SQLINTEGER> expected[]{
      {"varbyte", 16777216}, {"geometry", 1048447}, {"geography", 1048447}};
  for (const auto& [name, size] : expected) {
    SCOPED_TRACE(name);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    char actual[32]{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, actual, sizeof(actual), &length));
    EXPECT_STREQ(name, actual);
    SQLINTEGER column_size = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_LONG, &column_size, sizeof(column_size), &length));
    EXPECT_EQ(size, column_size);
  }
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(RedshiftSpecialTypesTest, SpatialParameterDescriptionKeepsBaselineUnknownFamily) {
  // Review finding: describing a spatial parameter as LONGVARBINARY would steer
  // applications to a VARBYTE bind that Redshift does not convert implicitly.
  // Parameter metadata keeps the baseline unknown fallback and its discovery.
  script->responses["INSERT INTO spatial_fixture VALUES ($1, $2)"] =
      frame('1', "") + frame('t', be16(2) + be32(3000) + be32(3001)) + frame('n', "");
  script->prefix_responses.emplace_back("WITH RECURSIVE type_chain",
      row_description({{"original_oid", 25}, {"type_oid", 25}, {"type_modifier", 25}}) +
      data_row({std::string("3000"), std::string("3000"), std::string("-1")}) +
      data_row({std::string("3001"), std::string("3001"), std::string("-1")}) + complete("SELECT 2"));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>("INSERT INTO spatial_fixture VALUES (?, ?)")), SQL_NTS));
  SQLHDESC ipd = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_IMP_PARAM_DESC, &ipd, 0, nullptr));
  for (SQLUSMALLINT parameter = 1; parameter <= 2; ++parameter) {
    SCOPED_TRACE(parameter);
    SQLSMALLINT type = -1;
    SQLULEN size = 99;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, parameter, &type, &size, nullptr, nullptr));
    EXPECT_EQ(SQL_VARCHAR, type);
    EXPECT_NE(SQL_LONGVARBINARY, type);
    char name[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ipd, parameter, SQL_DESC_TYPE_NAME, name, sizeof(name), nullptr));
    EXPECT_STRNE("geometry", name);
    EXPECT_STRNE("geography", name);
  }
  const auto discovery = std::count_if(script->received_sql.begin(), script->received_sql.end(),
      [](const std::string& sql) { return sql.starts_with("WITH RECURSIVE type_chain"); });
  EXPECT_EQ(1, discovery); // Unchanged baseline resolver behaviour for unknown OIDs.
}

} // namespace
