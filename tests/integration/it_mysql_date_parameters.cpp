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
// ODBCPP_MYSQL_DATE_PARAMETER_FIXTURE_ADMITTED=pinned-8.4.11-temporary-date-parameters-valid.
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

// Retains only deadlines and bounded numeric metadata from complete validated
// PREPARE column frames; no packets, names, SQL, values or credentials are kept.
// Fragmented frames are not reconstructed or treated as evidence of absence.
class DeadlineTransport final:public rs::core::transport::TLSTransport {
 public:
  struct NumericColumn {
    std::uint64_t charset{},type{},width{},decimals{},normalized_width{};
    std::int64_t normalized_decimals{};
  };
  std::array<NumericColumn,3> prepare_columns{};
  std::size_t prepare_column_count{};
  bool observe_prepare{};
  std::vector<rs::util::Deadline> deadlines;
  std::size_t closes{};
  rs::util::Result<void> connect_plain(std::string_view host,std::uint16_t port,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::connect_plain(host,port,deadline);
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view host,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::upgrade_to_tls(host,deadline);
  }
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte> bytes,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);
    mysql::query_detail::Cursor cursor(bytes);
    std::uint64_t length{},sequence{},command{};
    if (cursor.integer(3,length) && cursor.integer(1,sequence) && sequence==0 &&
        length && length==cursor.remaining() && cursor.integer(1,command)) {
      observe_prepare=command==22; // Ends before EXECUTE/CLOSE; no payload retained.
    }
    return TLSTransport::send(bytes,deadline);
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte> bytes,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);
    auto result=TLSTransport::recv(bytes,deadline);
    if (observe_prepare && result && result->n==bytes.size() &&
        prepare_column_count<prepare_columns.size()) {
      try {
        const std::span<const std::byte> frame{bytes.data(),result->n};
        mysql::query_detail::Cursor cursor(frame);
        std::string_view field;
        bool fields=true;
        for (unsigned i=0;i<6;++i) {
          if (!cursor.text(field)) { fields=false;break; }
        }
        std::uint64_t fixed{},charset{};
        if (fields && cursor.length(fixed) && fixed==12 && cursor.integer(2,charset)) {
          mysql::query_detail::NativeParameterDescriptorObservation raw;
          auto column=mysql::query_detail::column(frame,ResultLimits{},nullptr,nullptr,nullptr,&raw);
          if (column && column->normalized_type) {
            prepare_columns[prepare_column_count++]={charset,raw.type,raw.width,raw.decimals,
              column->normalized_type->column_size,column->normalized_type->decimal_digits};
          }
        }
      } catch (...) {
        // Diagnostics cannot change the native read result or session policy.
      }
    }
    return result;
  }
  void close() noexcept override { ++closes;TLSTransport::close(); }
};

class MySqlDateParametersIntegrationTest:public ::testing::Test {
 protected:
  ConnectionSettings settings_;
  std::unique_ptr<MySqlSession> session_;
  DeadlineTransport* transport_{};
  std::size_t connected_closes_{};
  bool created_{};
  bool create_attempted_{};
  std::optional<std::string> modes_;
  bool cleanup_attempted_{};
  rs::util::Deadline deadline_{};
  static constexpr std::string_view select_sql=
      "SELECT d,neighbor FROM odbcpp_date_parameter_fixture ORDER BY id";
  static constexpr std::string_view parameter_sql=
      "SELECT d,neighbor FROM odbcpp_date_parameter_fixture WHERE d <=> CAST(? AS DATE) ORDER BY id";

  ~MySqlDateParametersIntegrationTest() override {
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
    catch (...) { FAIL()<<"date-parameter-live-setup-exception"; }
  }
  void set_up_fixture() {
    const EnvironmentValue admitted{"ODBCPP_MYSQL_DATE_PARAMETER_FIXTURE_ADMITTED"},host{"ODBCPP_MYSQL_TEST_HOST"},
        port{"ODBCPP_MYSQL_TEST_PORT"},ca{"ODBCPP_MYSQL_TEST_CA_FILE"},
        user{"ODBCPP_MYSQL_TEST_USER"},password{"ODBCPP_MYSQL_TEST_PASSWORD"};
    ASSERT_TRUE(admitted.value && std::string_view(admitted.value)=="pinned-8.4.11-temporary-date-parameters-valid")<<"date-parameter-live-admission";
    ASSERT_TRUE(host.value && std::string_view(host.value)=="localhost")<<"date-parameter-live-host";
    ASSERT_TRUE(user.value && std::string_view(user.value)=="sdk")<<"date-parameter-live-user";
    ASSERT_TRUE(password.value && password.value[0])<<"date-parameter-live-password-config";
    ASSERT_TRUE(port.value && ca.value && ca.value[0])<<"date-parameter-live-endpoint-config";
    const std::string_view port_text(port.value);unsigned numeric_port{};
    const auto parsed=std::from_chars(port_text.data(),port_text.data()+port_text.size(),numeric_port);
    ASSERT_TRUE(parsed.ec==std::errc{} && parsed.ptr==port_text.data()+port_text.size() && numeric_port>0 && numeric_port<=65535)<<"date-parameter-live-port";
    std::error_code file_error;
    ASSERT_TRUE(std::filesystem::path(ca.value).is_absolute() && std::filesystem::is_regular_file(ca.value,file_error) && !file_error)<<"date-parameter-live-ca";
    settings_.host=host.value;settings_.port=static_cast<std::uint16_t>(numeric_port);
    settings_.user=user.value;settings_.password=password.value;settings_.ssl_ca_file=ca.value;
    settings_.database="odbcpp";settings_.use_ssl=true;settings_.timeout=std::chrono::seconds(10);
    auto transport=std::make_unique<DeadlineTransport>();transport_=transport.get();
    session_=std::make_unique<MySqlSession>(std::move(transport));
    auto connected=session_->connect(settings_);ASSERT_TRUE(static_cast<bool>(connected))<<"date-parameter-live-connect";
    ASSERT_TRUE(transport_->peer_identity_verified())<<"date-parameter-live-verified-peer";
    ASSERT_TRUE(session_->server_version()=="8.4.11")<<"date-parameter-live-version";
    ASSERT_EQ(SessionState::Idle,connected.session_snapshot().state);
    ASSERT_EQ(SessionDisposition::Reusable,connected.session_snapshot().disposition);
    // connect_plain resets the transport; count subsequent owned disconnects.
    connected_closes_=transport_->closes;
    ASSERT_FALSE(transport_->deadlines.empty());
    for (const auto deadline:transport_->deadlines) EXPECT_EQ(transport_->deadlines.front(),deadline);
    deadline_=rs::util::make_deadline(std::chrono::seconds(30));
    // Inventory only: ordinary canonical valid DATEs need no SQL-mode change.
    // Do not log server strings or infer zero/invalid-date storage qualification.
    auto modes=query("SELECT @@SESSION.sql_mode AS sql_mode");
    ASSERT_TRUE(static_cast<bool>(modes))<<"date-parameter-live-mode-inventory";
    ASSERT_EQ(1u,modes->rows.size());ASSERT_EQ(1u,modes->rows[0].size());
    ASSERT_TRUE(modes->rows[0][0]);ASSERT_TRUE(modes->cell_errors.empty());
    ASSERT_FALSE(modes->error);ASSERT_TRUE(modes->additional_results.empty());
    modes_=modes->rows[0][0];
    // Refuse persistent-name collisions. A fresh physical session has no prior
    // temporary tables; CREATE without IF NOT EXISTS also refuses temp collision.
    auto collision=query("SELECT CAST(COUNT(*) AS SIGNED) AS collisions FROM information_schema.tables WHERE table_schema='odbcpp' AND table_name='odbcpp_date_parameter_fixture'");
    ASSERT_TRUE(static_cast<bool>(collision))<<"date-parameter-live-collision-query";
    ASSERT_EQ(1u,collision->rows.size());ASSERT_EQ(1u,collision->rows[0].size());
    ASSERT_TRUE(collision->rows[0][0]==std::optional<std::string>{"0"})<<"date-parameter-live-name-collision";
    // Even an uncertain CREATE reply is cleaned up by unconditional owned
    // physical disconnect in TearDown; no reconnect or persistent DROP.
    create_attempted_=true;
    auto created=query("CREATE TEMPORARY TABLE odbcpp_date_parameter_fixture (id INT PRIMARY KEY,d DATE,neighbor BIGINT) ENGINE=InnoDB");
    ASSERT_TRUE(static_cast<bool>(created))<<"date-parameter-live-create";
    created_=true;
  }
  void cleanup_owned_table() {
    if (!created_ || cleanup_attempted_ || !session_ || !session_->is_connected()) return;
    cleanup_attempted_=true;
    deadline_=rs::util::make_deadline(std::chrono::seconds(5));
    auto dropped=query("DROP TEMPORARY TABLE odbcpp_date_parameter_fixture");
    EXPECT_TRUE(static_cast<bool>(dropped))<<"date-parameter-live-cleanup";
    if (dropped) created_=false;
  }
  void TearDown() override {
    try { cleanup_owned_table(); }
    catch (...) { ADD_FAILURE()<<"date-parameter-live-cleanup-exception"; }
    // Always close the owned physical session, even after assertion/exception,
    // failed cleanup or an uncertain CREATE outcome. Its temp table dies here.
    if (session_) {
      session_->disconnect();
      if (create_attempted_) created_=false;
    }
  }
  void check_zero_warnings() {
    // Diagnostic statement immediately follows INSERT or each SELECT; no
    // intervening query may erase that statement's warnings. Do not print them.
    auto warnings=query("SHOW COUNT(*) WARNINGS");
    ASSERT_TRUE(static_cast<bool>(warnings))<<"date-parameter-live-warning-inventory";
    ASSERT_EQ(1u,warnings->rows.size());ASSERT_EQ(1u,warnings->rows[0].size());
    ASSERT_TRUE(warnings->rows[0][0]==std::optional<std::string>{"0"})<<"date-parameter-live-warning-count";
    EXPECT_TRUE(warnings->cell_errors.empty());EXPECT_FALSE(warnings->error);
    EXPECT_TRUE(warnings->additional_results.empty());
  }
  BackendResult<QueryResult> prepared(const QueryParameter& parameter) {
    transport_->deadlines.clear();
    transport_->prepare_column_count=0;transport_->observe_prepare=false;
    const std::array parameters{parameter};
    auto result=session_->execute_prepared(parameter_sql,parameters,deadline_);
    transport_->observe_prepare=false;
    for (const auto deadline:transport_->deadlines) EXPECT_EQ(deadline_,deadline);
    return result;
  }
  void check_result(const QueryResult& result,const ResultRows& expected,bool parameter_receipt) {
    ASSERT_EQ(2u,result.columns.size());
    const std::array<std::string_view,2> names{"d","neighbor"};
    const std::array<ScalarType,2> types{ScalarType::Date,ScalarType::BigInt};
    const std::array<std::uint64_t,2> widths{10,19};
    for (std::size_t i=0;i<2;++i) {
      ASSERT_TRUE(result.columns[i].normalized_type);
      const auto& info=*result.columns[i].normalized_type;
      EXPECT_TRUE(info.known);EXPECT_EQ(types[i],info.type);
      EXPECT_EQ(widths[i],info.column_size);EXPECT_EQ(0,info.decimal_digits);
      EXPECT_TRUE(names[i]==result.columns[i].name)<<"date-parameter-live-column-name";
    }
    EXPECT_TRUE(expected==result.rows)<<"date-parameter-live-exact-rows";
    EXPECT_TRUE(result.cell_errors.empty());EXPECT_FALSE(result.error);
    EXPECT_TRUE(result.additional_results.empty());EXPECT_EQ(StatementKind::SelectCursor,result.statement_kind);
    ASSERT_EQ(parameter_receipt?1u:0u,result.normalized_parameter_types.size());
    if (parameter_receipt) {
      // Actual owning receipt; the separately reviewed runtime guard checks
      // native10/decimals0 and charset-bound raw byte width (63/10 or45/40)
      // before publishing normalized Date10/0 metadata.
      const auto& receipt=result.normalized_parameter_types[0];
      EXPECT_TRUE(receipt.known);EXPECT_EQ(ScalarType::Date,receipt.type);
      EXPECT_EQ(10u,receipt.column_size);EXPECT_EQ(0,receipt.decimal_digits);
    }
  }
};

TEST_F(MySqlDateParametersIntegrationTest, PhysicalDateComparisonBoundsNullAndOwningReceipts) {
  const ResultRows expected{
      ResultRow{std::string{"1000-01-01"},std::string{"42"}},
      ResultRow{std::string{"9999-12-31"},std::string{"42"}},
      ResultRow{std::string{"2000-02-29"},std::string{"42"}},
      ResultRow{std::string{"1900-02-28"},std::string{"42"}},
      ResultRow{std::string{"2024-02-29"},std::string{"42"}},
      ResultRow{std::nullopt,std::string{"42"}}};
  // Physical DATE filtering with explicit cast context. Bare SELECT ? does
  // not prove DATE inference. No date insertion parameters or mode mutation.
  auto inserted=query("INSERT INTO odbcpp_date_parameter_fixture VALUES "
      "(1,'1000-01-01',42),(2,'9999-12-31',42),(3,'2000-02-29',42),"
      "(4,'1900-02-28',42),(5,'2024-02-29',42),(6,NULL,42)");
  ASSERT_TRUE(static_cast<bool>(inserted))<<"date-parameter-live-insert";ASSERT_EQ(6u,inserted->affected_rows);
  check_zero_warnings();ASSERT_FALSE(HasFailure());
  auto direct=query(select_sql);ASSERT_TRUE(static_cast<bool>(direct))<<"date-parameter-live-direct";
  check_zero_warnings();ASSERT_FALSE(HasFailure());
  check_result(*direct,expected,false);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(SessionDisposition::Reusable,direct.session_snapshot().disposition);
  EXPECT_EQ(SessionState::Idle,direct.session_snapshot().state);
  std::vector<QueryResult> owning_results;
  for (std::size_t i=0;i<expected.size();++i) {
    SCOPED_TRACE(i);
    QueryParameter parameter{expected[i][0],QueryParameterType::Date};
    auto filtered=prepared(parameter);
    std::string numeric_metadata=" complete-prepare-column-observations="+
      std::to_string(transport_->prepare_column_count);
    if (!filtered) {
      for (std::size_t n=0;n<transport_->prepare_column_count;++n) {
        const auto& column=transport_->prepare_columns[n];
        numeric_metadata+=" ["+std::to_string(n)+":"+std::to_string(column.charset)+","+
          std::to_string(column.type)+","+std::to_string(column.width)+","+
          std::to_string(column.decimals)+","+std::to_string(column.normalized_width)+","+
          std::to_string(column.normalized_decimals)+"]";
      }
    }
    ASSERT_TRUE(static_cast<bool>(filtered))<<"date-parameter-live-filter "
      <<(filtered ? std::string_view{} : filtered.backend_error().safe_summary())
      <<" code="<<(filtered ? 0 : filtered.error().value())
      <<" operation="<<(filtered ? 0 : static_cast<int>(filtered.backend_error().operation))
      <<" state="<<static_cast<int>(filtered.session_snapshot().state)
      <<" disposition="<<static_cast<int>(filtered.session_snapshot().disposition)<<numeric_metadata;
    ASSERT_FALSE(transport_->deadlines.empty());
    // Must immediately follow the successful prepared statement, before any
    // recovery/inventory query can replace its warning area.
    check_zero_warnings();ASSERT_FALSE(HasFailure());
    check_result(*filtered,ResultRows{expected[i]},true);ASSERT_FALSE(HasFailure());
    EXPECT_EQ(SessionDisposition::Reusable,filtered.session_snapshot().disposition);
    EXPECT_EQ(SessionState::Idle,filtered.session_snapshot().state);
    EXPECT_TRUE(parameter.value==expected[i][0])<<"date-parameter-live-caller-ownership";
    parameter.value="changed";owning_results.push_back(std::move(*filtered));
  }
  // Rejected caller values are local policy checks, not server-invalid-date
  // qualification. Offline fake-wire tests own the precise zero-I/O proof.
  const std::array invalid{std::string{},std::string{"0000-00-00"},std::string{"2024-00-01"},
      std::string{"2023-02-29"},std::string{"1900-02-29"},std::string{"0999-12-31"},
      std::string{"10000-01-01"},std::string{"2024-2-29"},std::string("2000-02-29\0",11)};
  const auto closes=transport_->closes;
  for (const auto& value:invalid) {
    QueryParameter parameter{value,QueryParameterType::Date};auto rejected=prepared(parameter);
    ASSERT_FALSE(static_cast<bool>(rejected))<<"date-parameter-live-invalid-admission";
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),rejected.error());
    EXPECT_EQ(BackendOperation::ExecutePrepared,rejected.backend_error().operation);
    EXPECT_TRUE(rejected.error_message()=="MySQL session operation failed")<<"date-parameter-live-safe-diagnostic";
    EXPECT_EQ(SessionState::Idle,rejected.session_snapshot().state);
    EXPECT_EQ(SessionDisposition::Reusable,rejected.session_snapshot().disposition);
    EXPECT_TRUE(parameter.value==std::optional<std::string>{value})<<"date-parameter-live-invalid-caller-ownership";
    ASSERT_TRUE(session_->is_connected());ASSERT_TRUE(transport_->peer_identity_verified());
    EXPECT_EQ(closes,transport_->closes);
  }
  auto recovered=prepared(QueryParameter{std::string{"2024-02-29"},QueryParameterType::Date});
  ASSERT_TRUE(static_cast<bool>(recovered))<<"date-parameter-live-prepared-recovery";
  check_zero_warnings();ASSERT_FALSE(HasFailure());
  check_result(*recovered,ResultRows{expected[4]},true);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(SessionDisposition::Reusable,recovered.session_snapshot().disposition);
  EXPECT_EQ(SessionState::Idle,recovered.session_snapshot().state);
  auto recovery=query("SELECT CAST(1 AS SIGNED) AS recovery");
  ASSERT_TRUE(static_cast<bool>(recovery))<<"date-parameter-live-direct-recovery";
  ASSERT_EQ(1u,recovery->rows.size());ASSERT_EQ(1u,recovery->rows[0].size());
  EXPECT_TRUE(recovery->rows[0][0]==std::optional<std::string>{"1"})<<"date-parameter-live-recovery-value";
  EXPECT_TRUE(recovery->cell_errors.empty());EXPECT_FALSE(recovery->error);EXPECT_TRUE(recovery->additional_results.empty());
  EXPECT_EQ(SessionDisposition::Reusable,recovery.session_snapshot().disposition);
  EXPECT_EQ(SessionState::Idle,recovery.session_snapshot().state);
  auto modes=query("SELECT @@SESSION.sql_mode AS sql_mode");
  ASSERT_TRUE(static_cast<bool>(modes))<<"date-parameter-live-mode-recheck";
  ASSERT_EQ(1u,modes->rows.size());ASSERT_EQ(1u,modes->rows[0].size());
  EXPECT_TRUE(modes->rows[0][0]==modes_)<<"date-parameter-live-mode-preserved";
  EXPECT_TRUE(modes->cell_errors.empty());EXPECT_FALSE(modes->error);EXPECT_TRUE(modes->additional_results.empty());
  cleanup_owned_table();ASSERT_FALSE(HasFailure());ASSERT_FALSE(created_);
  session_->disconnect();EXPECT_EQ(connected_closes_+1,transport_->closes);
  session_.reset();transport_=nullptr;
  check_result(*direct,expected,false);check_result(*recovered,ResultRows{expected[4]},true);
  ASSERT_EQ(expected.size(),owning_results.size());
  for (std::size_t i=0;i<owning_results.size();++i) check_result(owning_results[i],ResultRows{expected[i]},true);
}
}
