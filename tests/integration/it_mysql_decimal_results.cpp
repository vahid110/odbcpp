#include <gtest/gtest.h>
#include "core/database/mysql/mysql_session.h"
#include "odbcpp/transport/tls_transport.h"
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>

namespace {
using namespace rs::core::database;
using rs::core::database::mysql::MySqlSession;

// Optional dedicated pinned-fixture target, never a default database gate.
// Root must wire the existing pinned 8.4.11 fixture's private credentials plus:
// ODBCPP_MYSQL_TEST_HOST=localhost, PORT=<disposable fixture port>,
// CA_FILE=<absolute generated ca.pem>, and
// ODBCPP_MYSQL_DECIMAL_FIXTURE_ADMITTED=pinned-8.4.11-temporary-decimal.
// Missing configuration fails before network I/O; no guessed credentials/skips.
struct EnvironmentValue {
  const char* value{};
#ifdef _WIN32
  char* owned{};std::size_t size{};
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
  explicit EnvironmentValue(const char* name):value(std::getenv(name)) {}
#endif
  EnvironmentValue(const EnvironmentValue&)=delete;
  EnvironmentValue& operator=(const EnvironmentValue&)=delete;
};

// Records no packets, SQL, values or credentials. All I/O uses real verified TLS.
class DeadlineTransport final:public rs::core::transport::TLSTransport {
 public:
  std::vector<rs::util::Deadline> deadlines;
  std::size_t closes{};
  rs::util::Result<void> connect_plain(std::string_view host,std::uint16_t port,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::connect_plain(host,port,deadline);
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view host,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::upgrade_to_tls(host,deadline);
  }
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte> bytes,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::send(bytes,deadline);
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte> bytes,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::recv(bytes,deadline);
  }
  void close() noexcept override { ++closes;TLSTransport::close(); }
};

class MySqlDecimalResultsIntegrationTest:public ::testing::Test {
 protected:
  ConnectionSettings settings_;
  std::unique_ptr<MySqlSession> session_;
  DeadlineTransport* transport_{};
  std::size_t connected_closes_{};
  bool created_{};
  rs::util::Deadline deadline_{};
  static constexpr std::string_view select_sql=
      "SELECT d5_2,d5_5,d65_0,d65_30 FROM odbcpp_decimal_result_fixture ORDER BY id";

  ~MySqlDecimalResultsIntegrationTest() override {
    rs::core::security::secure_cleanse({reinterpret_cast<unsigned char*>(settings_.password.data()),settings_.password.size()});
  }
  BackendResult<QueryResult> query(std::string_view sql) {
    transport_->deadlines.clear();
    auto result=session_->execute_query(sql,deadline_);
    for (const auto deadline:transport_->deadlines) EXPECT_EQ(deadline_,deadline);
    return result;
  }
  void SetUp() override {
    try { set_up_fixture(); }
    catch (...) { FAIL()<<"decimal-live-setup-exception"; }
  }
  void set_up_fixture() {
    const EnvironmentValue admitted{"ODBCPP_MYSQL_DECIMAL_FIXTURE_ADMITTED"},host{"ODBCPP_MYSQL_TEST_HOST"},
        port{"ODBCPP_MYSQL_TEST_PORT"},ca{"ODBCPP_MYSQL_TEST_CA_FILE"},
        user{"ODBCPP_MYSQL_TEST_USER"},password{"ODBCPP_MYSQL_TEST_PASSWORD"};
    ASSERT_TRUE(admitted.value && std::string_view(admitted.value)=="pinned-8.4.11-temporary-decimal")<<"decimal-live-admission";
    ASSERT_TRUE(host.value && std::string_view(host.value)=="localhost")<<"decimal-live-host";
    ASSERT_TRUE(user.value && std::string_view(user.value)=="sdk")<<"decimal-live-user";
    ASSERT_TRUE(password.value && password.value[0])<<"decimal-live-password-config";
    ASSERT_TRUE(port.value && ca.value && ca.value[0])<<"decimal-live-endpoint-config";
    const std::string_view port_text(port.value);unsigned numeric_port{};
    const auto parsed=std::from_chars(port_text.data(),port_text.data()+port_text.size(),numeric_port);
    ASSERT_TRUE(parsed.ec==std::errc{} && parsed.ptr==port_text.data()+port_text.size() && numeric_port>0 && numeric_port<=65535)<<"decimal-live-port";
    std::error_code file_error;
    ASSERT_TRUE(std::filesystem::path(ca.value).is_absolute() && std::filesystem::is_regular_file(ca.value,file_error) && !file_error)<<"decimal-live-ca";
    settings_.host=host.value;settings_.port=static_cast<std::uint16_t>(numeric_port);
    settings_.user=user.value;settings_.password=password.value;settings_.ssl_ca_file=ca.value;
    settings_.database="odbcpp";settings_.use_ssl=true;settings_.timeout=std::chrono::seconds(10);
    auto transport=std::make_unique<DeadlineTransport>();transport_=transport.get();
    session_=std::make_unique<MySqlSession>(std::move(transport));
    auto connected=session_->connect(settings_);ASSERT_TRUE(static_cast<bool>(connected))<<"decimal-live-connect";
    ASSERT_TRUE(transport_->peer_identity_verified())<<"decimal-live-verified-peer";
    ASSERT_TRUE(session_->server_version()=="8.4.11")<<"decimal-live-version";
    ASSERT_EQ(SessionState::Idle,connected.session_snapshot().state);
    ASSERT_EQ(SessionDisposition::Reusable,connected.session_snapshot().disposition);
    // connect_plain resets the transport; count subsequent owned disconnects.
    connected_closes_=transport_->closes;
    ASSERT_FALSE(transport_->deadlines.empty());
    for (const auto deadline:transport_->deadlines) EXPECT_EQ(transport_->deadlines.front(),deadline);
    deadline_=rs::util::make_deadline(std::chrono::seconds(30));
    // Refuse persistent-name collisions. A fresh physical session has no prior
    // temporary tables; CREATE without IF NOT EXISTS also refuses temp collision.
    auto collision=query("SELECT CAST(COUNT(*) AS SIGNED) AS collisions FROM information_schema.tables WHERE table_schema='odbcpp' AND table_name='odbcpp_decimal_result_fixture'");
    ASSERT_TRUE(static_cast<bool>(collision))<<"decimal-live-collision-query";
    ASSERT_EQ(1u,collision->rows.size());ASSERT_EQ(1u,collision->rows[0].size());
    ASSERT_TRUE(collision->rows[0][0]==std::optional<std::string>{"0"})<<"decimal-live-name-collision";
    auto created=query("CREATE TEMPORARY TABLE odbcpp_decimal_result_fixture (id INT PRIMARY KEY,d5_2 DECIMAL(5,2),d5_5 DECIMAL(5,5),d65_0 DECIMAL(65,0),d65_30 DECIMAL(65,30)) ENGINE=InnoDB");
    ASSERT_TRUE(static_cast<bool>(created))<<"decimal-live-create";
    created_=true;
  }
  void TearDown() override {
    try {
      if (created_ && session_ && session_->is_connected()) {
        deadline_=rs::util::make_deadline(std::chrono::seconds(5));
        auto dropped=query("DROP TEMPORARY TABLE odbcpp_decimal_result_fixture");
        EXPECT_TRUE(static_cast<bool>(dropped))<<"decimal-live-cleanup";
        created_=false;
      }
    } catch (...) { ADD_FAILURE()<<"decimal-live-cleanup-exception"; }
    // Closing the owned physical session also removes any surviving temp table.
    if (session_) session_->disconnect();
  }
  void check_result(const QueryResult& result,const ResultRows& expected) {
    ASSERT_EQ(4u,result.columns.size());
    const std::array<std::string_view,4> names{"d5_2","d5_5","d65_0","d65_30"};
    const std::array<std::uint64_t,4> precisions{5,5,65,65};
    const std::array<std::int16_t,4> scales{2,5,0,30};
    for (std::size_t i=0;i<4;++i) {
      ASSERT_TRUE(result.columns[i].normalized_type);
      const auto& info=*result.columns[i].normalized_type;
      EXPECT_TRUE(info.known);EXPECT_EQ(ScalarType::Decimal,info.type);
      EXPECT_EQ(precisions[i],info.column_size);EXPECT_EQ(scales[i],info.decimal_digits);
      EXPECT_EQ(names[i],result.columns[i].name);
    }
    EXPECT_EQ(expected,result.rows);EXPECT_TRUE(result.cell_errors.empty());EXPECT_FALSE(result.error);
    EXPECT_TRUE(result.additional_results.empty());EXPECT_EQ(StatementKind::SelectCursor,result.statement_kind);
  }
};

TEST_F(MySqlDecimalResultsIntegrationTest, DeclaredSignedBoundsNullAndOwnershipAgreeAcrossProtocols) {
  const ResultRow maximum{std::string{"999.99"},std::string{"0.99999"},std::string(65,'9'),std::string(35,'9')+"."+std::string(30,'9')};
  ResultRow minimum;for (const auto& value:maximum) minimum.emplace_back("-"+*value);
  const ResultRows expected{maximum,minimum,
      ResultRow{std::string{"0.00"},std::string{"0.00000"},std::string{"0"},"0."+std::string(30,'0')},
      ResultRow{std::nullopt,std::nullopt,std::nullopt,std::nullopt}};
  std::string insert="INSERT INTO odbcpp_decimal_result_fixture VALUES ";
  for (std::size_t row=0;row<expected.size();++row) {
    if (row) insert+=",";
    insert+="("+std::to_string(row+1);
    for (const auto& value:expected[row]) insert+=value?",'"+*value+"'":",NULL";
    insert+=")";
  }
  auto inserted=query(insert);ASSERT_TRUE(static_cast<bool>(inserted))<<"decimal-live-insert";
  ASSERT_EQ(4u,inserted->affected_rows);
  auto direct=query(select_sql);ASSERT_TRUE(static_cast<bool>(direct))<<"decimal-live-direct";
  check_result(*direct,expected);ASSERT_FALSE(HasFatalFailure());
  transport_->deadlines.clear();
  auto prepared=session_->execute_prepared(select_sql,{},deadline_);
  ASSERT_TRUE(static_cast<bool>(prepared))<<"decimal-live-prepared";
  ASSERT_FALSE(transport_->deadlines.empty());for (const auto deadline:transport_->deadlines) EXPECT_EQ(deadline_,deadline);
  check_result(*prepared,expected);ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(direct->rows,prepared->rows);EXPECT_TRUE(prepared->normalized_parameter_types.empty());
  EXPECT_EQ(SessionDisposition::Reusable,prepared.session_snapshot().disposition);EXPECT_EQ(SessionState::Idle,prepared.session_snapshot().state);
  ASSERT_TRUE(session_->is_connected());ASSERT_TRUE(transport_->peer_identity_verified());
  auto recovery=query("SELECT CAST(1 AS SIGNED) AS recovery");ASSERT_TRUE(static_cast<bool>(recovery))<<"decimal-live-recovery";
  ASSERT_EQ(1u,recovery->rows.size());ASSERT_EQ(1u,recovery->rows[0].size());EXPECT_EQ(std::optional<std::string>{"1"},recovery->rows[0][0]);
  auto dropped=query("DROP TEMPORARY TABLE odbcpp_decimal_result_fixture");ASSERT_TRUE(static_cast<bool>(dropped))<<"decimal-live-drop";
  created_=false;session_->disconnect();EXPECT_EQ(connected_closes_+1,transport_->closes);session_.reset();transport_=nullptr;
  check_result(*direct,expected);check_result(*prepared,expected);
}
}
