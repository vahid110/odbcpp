#include <gtest/gtest.h>
#include "odbc/setup_model.h"
using namespace rs::odbc::setup;
namespace {
Fields valid() { Fields f; f.dsn=L"Test"; f.server=L"localhost"; f.database=L"postgres"; return f; }
TEST(SetupModel, PersistsSettingsWithoutCredentials) {
  auto f=valid(); f.password=L"secret"; f.user=L"alice";
  const auto a=persisted(f);
  EXPECT_EQ(a.size(),5u); EXPECT_EQ(a.at(L"UID"),L"alice"); EXPECT_EQ(a.at(L"SSL"),L"1");
  EXPECT_EQ(a.count(L"PWD"),0u); EXPECT_EQ(a.count(L"PASSWORD"),0u);
}
TEST(SetupModel, EscapesTestCredentialsAndUnicode) {
  auto f=valid(); f.password=L"caf\u00e9;}tail";
  EXPECT_NE(connection_string(f).find(L"PWD={caf\u00e9;}}tail};"),std::wstring::npos);
  EXPECT_EQ(braced(L""),L"{}");
}
TEST(SetupModel, RejectsInvalidNames) {
  for(const auto name:{L"",L"bad;name",L"bad\\name",L"ODBC Data Sources",L"odbc data sources"}) {
    auto f=valid(); f.dsn=name; EXPECT_THROW(validate(f),std::invalid_argument);
  }
  auto f=valid(); f.dsn=std::wstring(33,L'x'); EXPECT_THROW(validate(f),std::invalid_argument);
}
TEST(SetupModel, ValidatesPortBounds) {
  for(const auto port:{L"",L"0",L"65536",L"-1",L"5432x",L"999999"}) {
    auto f=valid(); f.port=port; EXPECT_THROW(validate(f),std::invalid_argument);
  }
  for(const auto port:{L"1",L"65535"}) { auto f=valid(); f.port=port; EXPECT_NO_THROW(validate(f)); }
}
TEST(SetupModel, RejectsMissingAndControlFields) {
  auto f=valid(); f.server=L""; EXPECT_THROW(validate(f),std::invalid_argument);
  f=valid(); f.database=L""; EXPECT_THROW(validate(f),std::invalid_argument);
  f=valid(); f.user=L"line\nbreak"; EXPECT_THROW(validate(f),std::invalid_argument);
  f=valid(); f.password=std::wstring(1025,L'x'); EXPECT_THROW(validate(f),std::invalid_argument);
}
TEST(SetupModel, AcceptsUnicodeAndOptionalUser) {
  auto f=valid(); f.dsn=L"\u6d4b\u8bd5"; f.database=L"caf\u00e9"; f.ssl=false;
  EXPECT_NO_THROW(validate(f)); EXPECT_EQ(persisted(f).at(L"SSL"),L"0");
}
}
