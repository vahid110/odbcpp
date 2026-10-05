#include "core/database/mysql/mysql_session.h"
#include "odbcpp/transport/tls_transport.h"
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
      settings.user=username.value;settings.password=password.value;settings.ssl_ca_file=argv[3];settings.database="odbcpp";
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
      if (!query("CREATE TEMPORARY TABLE sdk_session (id BIGINT, value VARCHAR(16)) ENGINE=InnoDB")) return fail(4);
      auto inserted=query("INSERT INTO sdk_session VALUES (1,'fixture'),(2,NULL)");
      if (!inserted || inserted->affected_rows!=2) return fail(5);
      auto rows=query("SELECT id,value FROM sdk_session ORDER BY id");
      if (!rows || rows->rows.size()!=2 || rows->rows[0][0]!=std::optional<std::string>{"1"} ||
          rows->rows[0][1]!=std::optional<std::string>{"fixture"} || rows->rows[1][1]) return fail(6);
      const auto prepared=[&](std::string_view sql,std::span<const rs::core::database::QueryParameter> parameters) {
        auto value=session.execute_prepared(sql,parameters,rs::util::make_deadline(std::chrono::seconds(10)));
        if (!value) std::cerr<<"FAIL session-query-"<<++query_id<<" code "<<value.error().value()<<'\n';
        return value;
      };
      using Parameter=rs::core::database::QueryParameter;
      using Type=rs::core::database::QueryParameterType;
      const std::array bound{Parameter{"-42",Type::Int32},Parameter{std::string("a'\0é",5),Type::Text},
          Parameter{std::string("\0\xff",2),Type::Binary},Parameter{std::nullopt,Type::Int64}};
      auto selected=prepared("SELECT CAST(? AS SIGNED) AS n, CAST(? AS CHAR CHARACTER SET utf8mb4) AS txt, CAST(? AS BINARY) AS bin, CAST(? AS SIGNED) AS missing",bound);
      if (!selected || selected->rows.size()!=1 || selected->columns.size()!=4 || !selected->cell_errors.empty() ||
          selected->rows[0][0]!=bound[0].value || selected->rows[0][1]!=bound[1].value ||
          selected->rows[0][2]!=bound[2].value || selected->rows[0][3]) return fail(13);
      const std::array inserted_parameters{Parameter{"3",Type::Int16},Parameter{"",Type::Text}};
      auto prepared_insert=prepared("INSERT INTO sdk_session VALUES (?,?)",inserted_parameters);
      if (!prepared_insert || prepared_insert->affected_rows!=1) return fail(14);
      auto mismatch=prepared("SELECT CAST(? AS SIGNED)",{});
      if (mismatch || mismatch.error()!=DbErrorCode::InvalidParameter || !session.is_connected() || !query("SELECT 1")) return fail(15);
      auto no_parameters=prepared("SELECT CAST(7 AS SIGNED) AS n",{});
      if (!no_parameters || no_parameters->rows.size()!=1 || no_parameters->rows[0][0]!=std::optional<std::string>{"7"}) return fail(16);
      auto* transactions=session.transaction_session();
      if (!transactions || !transactions->transaction_capabilities().supported ||
          transactions->transaction_capabilities().transactional_ddl) return fail(17);
      const auto control=[&](rs::core::database::TransactionAction action) {
        return transactions->transaction(action,rs::util::make_deadline(std::chrono::seconds(10)));
      };
      using Action=rs::core::database::TransactionAction;
      auto begun=control(Action::Begin);
      if (!begun || begun.session_snapshot().state!=rs::core::database::SessionState::Transaction ||
          !query("INSERT INTO sdk_session VALUES (4,'committed')")) return fail(18);
      if (!query("SET SESSION completion_type=1")) return fail(26);
      auto committed=control(Action::Commit);
      if (!committed || committed.session_snapshot().state!=rs::core::database::SessionState::Idle) return fail(19);
      if (!control(Action::Begin) || !query("INSERT INTO sdk_session VALUES (5,'rolled_back')")) return fail(20);
      auto nested=control(Action::Begin);
      if (nested || nested.error()!=DbErrorCode::InvalidParameter || !session.is_connected() ||
          session.session_state()!=rs::core::database::SessionState::Transaction) return fail(21);
      if (!query("SET SESSION completion_type=2")) return fail(27);
      auto rolled_back=control(Action::Rollback);
      if (!rolled_back || rolled_back.session_snapshot().state!=rs::core::database::SessionState::Idle) return fail(22);
      if (!query("SET SESSION completion_type=0")) return fail(28);
      auto count_committed=query("SELECT CAST(COUNT(*) AS SIGNED) AS n FROM sdk_session WHERE id=4");
      auto count_rolled_back=query("SELECT CAST(COUNT(*) AS SIGNED) AS n FROM sdk_session WHERE id=5");
      if (!count_committed || !count_rolled_back || count_committed->rows.size()!=1 || count_rolled_back->rows.size()!=1 ||
          count_committed->rows[0][0]!=std::optional<std::string>{"1"} ||
          count_rolled_back->rows[0][0]!=std::optional<std::string>{"0"}) return fail(23);
      // Compare on the server and return the already-qualified signed integer
      // type; system-variable string metadata is outside this bounded profile.
      const std::array isolation_checks{
          "SELECT CAST(@@SESSION.transaction_isolation = 'READ-UNCOMMITTED' AS SIGNED) AS isolation_matches",
          "SELECT CAST(@@SESSION.transaction_isolation = 'READ-COMMITTED' AS SIGNED) AS isolation_matches",
          "SELECT CAST(@@SESSION.transaction_isolation = 'REPEATABLE-READ' AS SIGNED) AS isolation_matches",
          "SELECT CAST(@@SESSION.transaction_isolation = 'SERIALIZABLE' AS SIGNED) AS isolation_matches"};
      for (std::size_t i=0;i<isolation_checks.size();++i) {
        auto changed=transactions->set_transaction_isolation(rs::core::database::transaction_isolations[i],
            rs::util::make_deadline(std::chrono::seconds(10)));
        auto isolation=query(isolation_checks[i]);
        if (!changed || !isolation || isolation->rows.size()!=1 ||
            isolation->rows[0][0]!=std::optional<std::string>{"1"}) return fail(24);
      }
      if (!transactions->set_transaction_isolation(rs::core::database::TransactionIsolation::RepeatableRead,
          rs::util::make_deadline(std::chrono::seconds(10)))) return fail(25);
      auto empty=query("SELECT id FROM sdk_session WHERE id=0");
      if (!empty || empty->columns.size()!=1 || !empty->rows.empty()) return fail(7);
      auto invalid=query("SELECT * FROM sdk_missing_table");
      if (invalid || invalid.error()!=DbErrorCode::QueryFailed || session.is_connected()) return fail(8);
      session.disconnect();
      if (result->rows[0][0]!=std::optional<std::string>{"42"} || rows->rows[0][1]!=std::optional<std::string>{"fixture"}) return fail(9);
      if (!session.connect(settings) || !query("SELECT 1")) return fail(10);
      session.disconnect();settings.database="odbcpp_missing_database";
      auto missing=session.connect(settings);
      if (missing || missing.error()!=DbErrorCode::QueryFailed || session.is_connected() ||
          !session.server_version().empty()) return fail(11);
      settings.database="odbcpp";
      if (!session.connect(settings) || !query("SELECT 1")) return fail(12);
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
