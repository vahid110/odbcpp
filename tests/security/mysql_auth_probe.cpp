#include "core/database/mysql/mysql_session.h"
#include "core/transport/tls_transport.h"
#include <cstdlib>
#include <iostream>

namespace {
struct EnvironmentValue {
  const char* value{};
#ifdef _WIN32
  char* owned{};
  std::size_t size{};
  explicit EnvironmentValue(const char* name) {
    if (_dupenv_s(&owned,&size,name)==0) value=owned;
  }
  ~EnvironmentValue() {
    if (owned) {
      rs::core::security::secure_cleanse({reinterpret_cast<unsigned char*>(owned),size});
      std::free(owned);
    }
  }
#else
  explicit EnvironmentValue(const char* name) : value(std::getenv(name)) {}
#endif
  EnvironmentValue(const EnvironmentValue&)=delete;
  EnvironmentValue& operator=(const EnvironmentValue&)=delete;
};
}

int main(int argc, char** argv) {
  using namespace rs::core::database::mysql;
  using rs::util::DbErrorCode;
  if (argc != 5) return 2; // host, port, CA, expected result
  const EnvironmentValue username{"ODBCPP_MYSQL_TEST_USER"};
  const EnvironmentValue password{"ODBCPP_MYSQL_TEST_PASSWORD"};
  if (!username.value || !password.value) return 2;
  try {
    const auto port=std::stoul(argv[2]);
    if (port==0 || port>65535) return 2;
    const std::string_view expected=argv[4];
    if (expected=="session") {
      const auto fail=[](unsigned id) { std::cerr<<"FAIL session-check-"<<id<<'\n';return 1; };
      rs::core::database::ConnectionSettings settings;
      settings.host=argv[1];settings.port=static_cast<std::uint16_t>(port);
      settings.user=username.value;settings.password=password.value;settings.ssl_ca_file=argv[3];
      struct CleanPassword {
        std::string& value;
        ~CleanPassword() { rs::core::security::secure_cleanse({reinterpret_cast<unsigned char*>(value.data()),value.size()}); }
      } clean{settings.password};
      MySqlSession session(std::make_unique<rs::core::transport::TLSTransport>());
      auto connected=session.connect(settings);
      if (!connected || connected.session_snapshot().state!=rs::core::database::SessionState::Idle ||
          connected.session_snapshot().disposition!=rs::core::database::SessionDisposition::Reusable) return fail(1);
      unsigned query_id{};
      const auto query=[&](std::string_view sql) {
        ++query_id;
        auto result=session.execute_query(sql,rs::util::make_deadline(std::chrono::seconds(10)));
        if (!result) std::cerr<<"FAIL session-query-"<<query_id<<" code "<<result.error().value()<<'\n';
        return result;
      };
      auto result=query("SELECT CAST(42 AS SIGNED) AS id, _utf8mb4'a' AS txt, NULL AS missing, _utf8mb4'' AS empty_value, UNHEX('00FF') AS bin");
      if (!result || result->columns.size()!=5 || result->rows.size()!=1 || !result->cell_errors.empty()) return fail(2);
      const auto& row=result->rows[0];
      if (row[0]!=std::optional<std::string>{"42"} || row[1]!=std::optional<std::string>{"a"} || row[2] ||
          !row[3] || !row[3]->empty() || row[4]!=std::optional<std::string>{std::string("\0\xff",2)}) return fail(3);
      if (!query("CREATE TEMPORARY TABLE odbcpp.sdk_session (id BIGINT, value VARCHAR(16))")) return fail(4);
      auto inserted=query("INSERT INTO odbcpp.sdk_session VALUES (1,'fixture'),(2,NULL)");
      if (!inserted || inserted->affected_rows!=2) return fail(5);
      auto rows=query("SELECT id,value FROM odbcpp.sdk_session ORDER BY id");
      if (!rows || rows->rows.size()!=2 || rows->rows[0][0]!=std::optional<std::string>{"1"} ||
          rows->rows[0][1]!=std::optional<std::string>{"fixture"} || rows->rows[1][1]) return fail(6);
      auto empty=query("SELECT id FROM odbcpp.sdk_session WHERE id=0");
      if (!empty || empty->columns.size()!=1 || !empty->rows.empty()) return fail(7);
      auto invalid=query("SELECT * FROM odbcpp.sdk_missing_table");
      if (invalid || invalid.error()!=DbErrorCode::QueryFailed || session.is_connected()) return fail(8);
      session.disconnect();
      if (result->rows[0][0]!=std::optional<std::string>{"42"} || rows->rows[0][1]!=std::optional<std::string>{"fixture"}) return fail(9);
      if (!session.connect(settings) || !query("SELECT 1")) return fail(10);
      session.disconnect();std::cout<<"PASS session\n";return 0;
    }
    rs::core::transport::TLSTransport transport;
    transport.set_ca_locations(argv[3],"");
    auto result=authenticate_verified_tls(transport,argv[1],static_cast<std::uint16_t>(port),
        username.value,password.value,rs::util::make_deadline(std::chrono::seconds(15)));
    if (expected=="reject-auth" || expected=="reject-tls") {
      if (result) { transport.close(); return 1; }
      const auto error=expected=="reject-auth"?DbErrorCode::AuthenticationFailed:DbErrorCode::TLSError;
      if (result.error()!=error) return 1;
      std::cout << "PASS " << expected << '\n'; return 0;
    }
    if (!result) { std::cerr << "Authentication rejected, code " << result.error().value() << '\n'; return 1; }
    const auto path=expected=="full"?AuthenticationPath::FullOverTls:AuthenticationPath::Cached;
    if (result->path!=path || result->verified.greeting.server_version!="8.4.11" ||
        !transport.peer_identity_verified()) { transport.close(); return 1; }
    transport.close();
    std::cout << "PASS " << expected << '\n';
    return 0;
  } catch (...) { std::cerr << "MySQL probe failed\n"; return 1; }
}
