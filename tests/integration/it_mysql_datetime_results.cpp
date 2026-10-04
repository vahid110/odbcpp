#include <gtest/gtest.h>
#include "core/database/mysql/mysql_session.h"
#include "core/transport/tls_transport.h"
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
// ODBCPP_MYSQL_DATETIME_FIXTURE_ADMITTED=pinned-8.4.11-temporary-datetime-valid.
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

class MySqlDatetimeResultsIntegrationTest:public ::testing::Test {
 protected:
  ConnectionSettings settings_;
  std::unique_ptr<MySqlSession> session_;
  DeadlineTransport* transport_{};
  std::size_t connected_closes_{};
  bool created_{};
  bool cleanup_attempted_{};
  rs::util::Deadline deadline_{};
  static constexpr std::string_view select_sql=
      "SELECT d0,d3,d6,neighbor FROM odbcpp_datetime_result_fixture ORDER BY id";

  ~MySqlDatetimeResultsIntegrationTest() override {
    rs::core::security::secure_cleanse({reinterpret_cast<unsigned char*>(settings_.password.data()),settings_.password.size()});
  }
  BackendResult<QueryResult> query(std::string_view sql) {
    transport_->deadlines.clear();
    auto result=session_->execute_query(sql,deadline_);
    EXPECT_FALSE(transport_->deadlines.empty());
    for (const auto deadline:transport_->deadlines) EXPECT_EQ(deadline_,deadline);
    return result;
  }
  void SetUp() override {
    try { set_up_fixture(); }
    catch (...) { FAIL()<<"datetime-live-setup-exception"; }
  }
  void set_up_fixture() {
    const EnvironmentValue admitted{"ODBCPP_MYSQL_DATETIME_FIXTURE_ADMITTED"},host{"ODBCPP_MYSQL_TEST_HOST"},
        port{"ODBCPP_MYSQL_TEST_PORT"},ca{"ODBCPP_MYSQL_TEST_CA_FILE"},
        user{"ODBCPP_MYSQL_TEST_USER"},password{"ODBCPP_MYSQL_TEST_PASSWORD"};
    ASSERT_TRUE(admitted.value && std::string_view(admitted.value)=="pinned-8.4.11-temporary-datetime-valid")<<"datetime-live-admission";
    ASSERT_TRUE(host.value && std::string_view(host.value)=="localhost")<<"datetime-live-host";
    ASSERT_TRUE(user.value && std::string_view(user.value)=="sdk")<<"datetime-live-user";
    ASSERT_TRUE(password.value && password.value[0])<<"datetime-live-password-config";
    ASSERT_TRUE(port.value && ca.value && ca.value[0])<<"datetime-live-endpoint-config";
    const std::string_view port_text(port.value);unsigned numeric_port{};
    const auto parsed=std::from_chars(port_text.data(),port_text.data()+port_text.size(),numeric_port);
    ASSERT_TRUE(parsed.ec==std::errc{} && parsed.ptr==port_text.data()+port_text.size() && numeric_port>0 && numeric_port<=65535)<<"datetime-live-port";
    std::error_code file_error;
    ASSERT_TRUE(std::filesystem::path(ca.value).is_absolute() && std::filesystem::is_regular_file(ca.value,file_error) && !file_error)<<"datetime-live-ca";
    settings_.host=host.value;settings_.port=static_cast<std::uint16_t>(numeric_port);
    settings_.user=user.value;settings_.password=password.value;settings_.ssl_ca_file=ca.value;
    settings_.database="odbcpp";settings_.use_ssl=true;settings_.timeout=std::chrono::seconds(10);
    auto transport=std::make_unique<DeadlineTransport>();transport_=transport.get();
    session_=std::make_unique<MySqlSession>(std::move(transport));
    auto connected=session_->connect(settings_);ASSERT_TRUE(static_cast<bool>(connected))<<"datetime-live-connect";
    ASSERT_TRUE(transport_->peer_identity_verified())<<"datetime-live-verified-peer";
    ASSERT_TRUE(session_->server_version()=="8.4.11")<<"datetime-live-version";
    ASSERT_EQ(SessionState::Idle,connected.session_snapshot().state);
    ASSERT_EQ(SessionDisposition::Reusable,connected.session_snapshot().disposition);
    // connect_plain resets the transport; count subsequent owned disconnects.
    connected_closes_=transport_->closes;
    ASSERT_FALSE(transport_->deadlines.empty());
    for (const auto deadline:transport_->deadlines) EXPECT_EQ(transport_->deadlines.front(),deadline);
    deadline_=rs::util::make_deadline(std::chrono::seconds(30));
    // Inventory only: ordinary canonical valid DATETIMEs need no SQL-mode change.
    // Do not log server strings or infer zero/invalid-date storage qualification.
    auto modes=query("SELECT @@SESSION.sql_mode AS sql_mode");
    ASSERT_TRUE(static_cast<bool>(modes))<<"datetime-live-mode-inventory";
    ASSERT_EQ(1u,modes->rows.size());ASSERT_EQ(1u,modes->rows[0].size());
    ASSERT_TRUE(modes->rows[0][0]);ASSERT_TRUE(modes->cell_errors.empty());
    ASSERT_FALSE(modes->error);ASSERT_TRUE(modes->additional_results.empty());
    // Refuse persistent-name collisions. A fresh physical session has no prior
    // temporary tables; CREATE without IF NOT EXISTS also refuses temp collision.
    auto collision=query("SELECT CAST(COUNT(*) AS SIGNED) AS collisions FROM information_schema.tables WHERE table_schema='odbcpp' AND table_name='odbcpp_datetime_result_fixture'");
    ASSERT_TRUE(static_cast<bool>(collision))<<"datetime-live-collision-query";
    ASSERT_EQ(1u,collision->rows.size());ASSERT_EQ(1u,collision->rows[0].size());
    ASSERT_TRUE(collision->rows[0][0]==std::optional<std::string>{"0"})<<"datetime-live-name-collision";
    // Even an uncertain CREATE reply is cleaned up by unconditional owned
    // physical disconnect in TearDown; no reconnect or persistent DROP.
    auto created=query("CREATE TEMPORARY TABLE odbcpp_datetime_result_fixture (id INT PRIMARY KEY,d0 DATETIME(0),d3 DATETIME(3),d6 DATETIME(6),neighbor BIGINT) ENGINE=InnoDB");
    ASSERT_TRUE(static_cast<bool>(created))<<"datetime-live-create";
    created_=true;
  }
  void cleanup_owned_table() {
    if (!created_ || cleanup_attempted_ || !session_ || !session_->is_connected()) return;
    cleanup_attempted_=true;
    deadline_=rs::util::make_deadline(std::chrono::seconds(5));
    auto dropped=query("DROP TEMPORARY TABLE odbcpp_datetime_result_fixture");
    EXPECT_TRUE(static_cast<bool>(dropped))<<"datetime-live-cleanup";
    if (dropped) created_=false;
  }
  void TearDown() override {
    try { cleanup_owned_table(); }
    catch (...) { ADD_FAILURE()<<"datetime-live-cleanup-exception"; }
    // Always close the owned physical session, even after assertion/exception,
    // failed cleanup or an uncertain CREATE outcome. Its temp table dies here.
    if (session_) session_->disconnect();
  }
  void check_zero_warnings() {
    // Diagnostic statement immediately follows INSERT or each SELECT; no
    // intervening query may erase that statement's warnings. Do not print them.
    auto warnings=query("SHOW COUNT(*) WARNINGS");
    ASSERT_TRUE(static_cast<bool>(warnings))<<"datetime-live-warning-inventory";
    ASSERT_EQ(1u,warnings->rows.size());ASSERT_EQ(1u,warnings->rows[0].size());
    ASSERT_TRUE(warnings->rows[0][0]==std::optional<std::string>{"0"})<<"datetime-live-warning-count";
    EXPECT_TRUE(warnings->cell_errors.empty());EXPECT_FALSE(warnings->error);
    EXPECT_TRUE(warnings->additional_results.empty());
  }
  void check_result(const QueryResult& result,const ResultRows& expected) {
    ASSERT_EQ(4u,result.columns.size());
    const std::array<std::string_view,4> names{"d0","d3","d6","neighbor"};
    const std::array<ScalarType,4> types{ScalarType::Timestamp,ScalarType::Timestamp,ScalarType::Timestamp,ScalarType::BigInt};
    const std::array<std::uint64_t,4> widths{19,23,26,19};
    const std::array<std::int16_t,4> precisions{0,3,6,0};
    for (std::size_t i=0;i<4;++i) {
      ASSERT_TRUE(result.columns[i].normalized_type);
      const auto& info=*result.columns[i].normalized_type;
      EXPECT_TRUE(info.known);EXPECT_EQ(types[i],info.type);
      EXPECT_EQ(widths[i],info.column_size);EXPECT_EQ(precisions[i],info.decimal_digits);
      EXPECT_TRUE(names[i]==result.columns[i].name)<<"datetime-live-column-name";
    }
    EXPECT_TRUE(expected==result.rows)<<"datetime-live-exact-rows";
    EXPECT_TRUE(result.cell_errors.empty());EXPECT_FALSE(result.error);
    EXPECT_TRUE(result.additional_results.empty());EXPECT_TRUE(result.normalized_parameter_types.empty());
    EXPECT_EQ(StatementKind::SelectCursor,result.statement_kind);
  }
};

TEST_F(MySqlDatetimeResultsIntegrationTest, CalendarPrecisionNullAndOwnershipAgreeAcrossProtocols) {
  const ResultRows expected{
    ResultRow{std::string{"1000-01-01 00:00:00"},std::string{"1000-01-01 00:00:00.000"},std::string{"1000-01-01 00:00:00.000000"},std::string{"42"}},
    ResultRow{std::string{"9999-12-31 23:59:59"},std::string{"9999-12-31 23:59:59.000"},std::string{"9999-12-31 23:59:59.000000"},std::string{"42"}},
    ResultRow{std::string{"2000-02-29 00:00:00"},std::string{"2000-02-29 00:00:00.000"},std::string{"2000-02-29 00:00:00.000000"},std::string{"42"}},
    ResultRow{std::string{"1900-02-28 12:34:56"},std::string{"1900-02-28 12:34:56.123"},std::string{"1900-02-28 12:34:56.123456"},std::string{"42"}},
    ResultRow{std::string{"2024-02-29 01:02:03"},std::string{"2024-02-29 01:02:03.001"},std::string{"2024-02-29 01:02:03.000001"},std::string{"42"}},
    ResultRow{std::nullopt,std::nullopt,std::nullopt,std::string{"42"}}};
  // Physical DATETIME(0/3/6) descriptors, calendar fields and exact interior
  // fractions. No fractional storage-endpoint, epoch/zone or parameter claim.
  auto inserted=query("INSERT INTO odbcpp_datetime_result_fixture VALUES "
      "(1,'1000-01-01 00:00:00','1000-01-01 00:00:00.000','1000-01-01 00:00:00.000000',42),"
      "(2,'9999-12-31 23:59:59','9999-12-31 23:59:59.000','9999-12-31 23:59:59.000000',42),"
      "(3,'2000-02-29 00:00:00','2000-02-29 00:00:00.000','2000-02-29 00:00:00.000000',42),"
      "(4,'1900-02-28 12:34:56','1900-02-28 12:34:56.123','1900-02-28 12:34:56.123456',42),"
      "(5,'2024-02-29 01:02:03','2024-02-29 01:02:03.001','2024-02-29 01:02:03.000001',42),"
      "(6,NULL,NULL,NULL,42)");
  ASSERT_TRUE(static_cast<bool>(inserted))<<"datetime-live-insert";ASSERT_EQ(6u,inserted->affected_rows);
  check_zero_warnings();ASSERT_FALSE(HasFailure());
  auto direct=query(select_sql);ASSERT_TRUE(static_cast<bool>(direct))<<"datetime-live-direct";
  check_zero_warnings();ASSERT_FALSE(HasFailure());
  check_result(*direct,expected);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(SessionDisposition::Reusable,direct.session_snapshot().disposition);
  EXPECT_EQ(SessionState::Idle,direct.session_snapshot().state);
  transport_->deadlines.clear();
  auto prepared=session_->execute_prepared(select_sql,{},deadline_);
  ASSERT_TRUE(static_cast<bool>(prepared))<<"datetime-live-prepared";
  ASSERT_FALSE(transport_->deadlines.empty());
  for (const auto deadline:transport_->deadlines) EXPECT_EQ(deadline_,deadline);
  check_zero_warnings();ASSERT_FALSE(HasFailure());
  check_result(*prepared,expected);ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(direct->rows==prepared->rows)<<"datetime-live-protocol-agreement";
  EXPECT_EQ(SessionDisposition::Reusable,prepared.session_snapshot().disposition);
  EXPECT_EQ(SessionState::Idle,prepared.session_snapshot().state);
  ASSERT_TRUE(session_->is_connected());ASSERT_TRUE(transport_->peer_identity_verified());
  // Native reuse proves response draining without a packet trace or replay.
  auto recovery=query("SELECT CAST(1 AS SIGNED) AS recovery");
  ASSERT_TRUE(static_cast<bool>(recovery))<<"datetime-live-recovery";
  ASSERT_EQ(1u,recovery->rows.size());ASSERT_EQ(1u,recovery->rows[0].size());
  EXPECT_TRUE(recovery->rows[0][0]==std::optional<std::string>{"1"})<<"datetime-live-recovery-value";
  EXPECT_TRUE(recovery->cell_errors.empty());EXPECT_FALSE(recovery->error);
  EXPECT_TRUE(recovery->additional_results.empty());
  EXPECT_EQ(SessionDisposition::Reusable,recovery.session_snapshot().disposition);
  EXPECT_EQ(SessionState::Idle,recovery.session_snapshot().state);
  cleanup_owned_table();ASSERT_FALSE(HasFailure());
  session_->disconnect();EXPECT_EQ(connected_closes_+1,transport_->closes);
  session_.reset();transport_=nullptr;
  check_result(*direct,expected);check_result(*prepared,expected);
}
}
