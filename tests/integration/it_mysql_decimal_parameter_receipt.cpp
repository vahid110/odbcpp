#include <gtest/gtest.h>
#include "core/database/mysql/mysql_session.h"
#include "core/transport/tls_transport.h"
#include "tests/integration/mysql_prepare_numeric_observation.h"
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
using namespace rs::core::database;
namespace mysql=rs::core::database::mysql;
using Observer=mysql::integration_detail::PrepareNumericObservation;
using rs::util::DbErrorCode;
// Future dedicated default-OFF diagnostic only, never native parameter support.
// Private admission: ODBCPP_MYSQL_DECIMAL_RECEIPT_FIXTURE_ADMITTED=
// pinned-8.4.11-temporary-decimal-receipt-observation. Root excludes this source
// from default discovery before copying and separately admits its pinned runner.
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

// Owning diagnostic facts only; neither the policy label nor properties
// qualify native Numeric parameter support or complete the PREPARE exchange.
struct DecimalReceiptEvidence {
  Observer::RawColumn raw;
  std::uint16_t parameter_count{},result_count{};
  std::size_t observed_count{},prepare_commands{},execute_commands{},close_commands{},physical_close_delta{};
  bool records_truncated{},exchange_complete{},transport_complete_calls{},runtime_retired{};
};
unsigned decimal_receipt_policy_code(const Observer::RawColumn& raw) {
  const auto profile=mysql::decimal_detail::metadata(raw.width,raw.decimals,raw.unsigned_value());
  if (profile) { return 3; } // Existing result-only parameter refusal.
  if (profile.error()==rs::util::make_error_code(DbErrorCode::UnsupportedFeature)) { return 2; }
  return 1; // Existing structural width/scale failure.
}
void record_decimal_receipt_properties(const DecimalReceiptEvidence& evidence) {
  const std::array<std::pair<const char*,std::uint64_t>,20> fields{{
      {"decimal_receipt_schema",1},
      {"prepare_parameter_count",evidence.parameter_count},{"prepare_result_count",evidence.result_count},
      {"observed_parameter_count",evidence.observed_count},{"records_truncated",evidence.records_truncated},
      {"exchange_complete",evidence.exchange_complete},{"transport_complete_calls",evidence.transport_complete_calls},
      {"prepare_command_count",evidence.prepare_commands},{"execute_command_count",evidence.execute_commands},
      {"close_command_count",evidence.close_commands},{"raw_parameter_0_native_type",evidence.raw.type},
      {"raw_parameter_0_charset",evidence.raw.charset},{"raw_parameter_0_byte_width",evidence.raw.width},
      {"raw_parameter_0_decimals",evidence.raw.decimals},{"raw_parameter_0_flags",evidence.raw.flags},
      {"raw_parameter_0_unsigned",evidence.raw.unsigned_value()},{"runtime_retired",evidence.runtime_retired},
      {"physical_close_delta",evidence.physical_close_delta},{"refusal_policy_code",decimal_receipt_policy_code(evidence.raw)},
      {"parameter_support_claimed",0}}};
  for (const auto& [key,value]:fields) { ::testing::Test::RecordProperty(key,std::to_string(value)); }
}

// Observes only complete calls; stores numeric header fields, never a packet.
// Fragmentation fails this diagnostic proof without changing real I/O behavior.
class ReceiptTransport final:public rs::core::transport::TLSTransport {
 public:
  Observer observer;
  std::vector<rs::util::Deadline> deadlines;
  std::size_t closes{},prepare_commands{},execute_commands{},close_commands{};
  bool proof_complete{};
  void arm() {
    observer=Observer{};prepare_commands=0;execute_commands=0;close_commands=0;
    armed_=true;prepare_delivered_=false;pending_body_=false;proof_complete=true;
    deadlines.clear();
  }
  void finish() { armed_=false;observer.missing(); }
  rs::util::Result<void> connect_plain(std::string_view host,std::uint16_t port,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::connect_plain(host,port,deadline);
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view host,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);return TLSTransport::upgrade_to_tls(host,deadline);
  }
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte> bytes,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);
    auto result=TLSTransport::send(bytes,deadline);
    if (!armed_) return result;
    mysql::query_detail::Cursor cursor(bytes);std::uint64_t length{},sequence{},command{};
    if (!result || result->n!=bytes.size() || result->eof || !cursor.integer(3,length) ||
        !cursor.integer(1,sequence) || sequence || !length || length!=cursor.remaining() ||
        !cursor.integer(1,command)) { inconclusive();return result; }
    if (command==22) {
      ++prepare_commands;
      if (prepare_commands!=1 || !proof_complete) { inconclusive();return result; }
      prepare_delivered_=true;observer.begin();
    } else if (command==23) { ++execute_commands; }
    else if (command==25) { ++close_commands; }
    else { inconclusive(); }
    return result;
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte> bytes,rs::util::Deadline deadline) override {
    deadlines.push_back(deadline);
    auto result=TLSTransport::recv(bytes,deadline);
    if (!armed_ || !prepare_delivered_ || !proof_complete) return result;
    if (!result || result->n!=bytes.size() || result->eof) { inconclusive();return result; }
    if (!pending_body_) {
      if (bytes.size()!=4) { inconclusive();return result; }
      mysql::query_detail::Cursor header(bytes);std::uint64_t length{},sequence{};
      header.integer(3,length);header.integer(1,sequence);
      pending_length_=static_cast<std::uint32_t>(length);pending_sequence_=static_cast<std::uint8_t>(sequence);
      pending_body_=true;
    } else {
      if (bytes.size()!=pending_length_) { inconclusive();return result; }
      const auto progress=observer.observe_complete_payload(pending_length_,pending_sequence_,bytes);
      pending_body_=false;
      if (progress!=Observer::Progress::Collecting && progress!=Observer::Progress::Complete) { inconclusive(); }
    }
    return result;
  }
  void close() noexcept override { ++closes;TLSTransport::close(); }
 private:
  void inconclusive() { proof_complete=false;observer.missing(); }
  bool armed_{},prepare_delivered_{},pending_body_{};
  std::uint32_t pending_length_{};std::uint8_t pending_sequence_{};
};

class MySqlDecimalReceiptObservationIntegrationTest:public ::testing::Test {
 protected:
  ConnectionSettings settings_;
  std::unique_ptr<mysql::MySqlSession> session_;
  ReceiptTransport* transport_{};
  rs::util::Deadline deadline_{};
  bool create_attempted_{},created_{},cleanup_attempted_{};
  ~MySqlDecimalReceiptObservationIntegrationTest() override {
    rs::core::security::secure_cleanse({reinterpret_cast<unsigned char*>(settings_.password.data()),settings_.password.size()});
  }
  BackendResult<QueryResult> query(std::string_view sql) {
    transport_->deadlines.clear();auto result=session_->execute_query(sql,deadline_);
    EXPECT_FALSE(transport_->deadlines.empty());
    for (const auto deadline:transport_->deadlines) { EXPECT_EQ(deadline_,deadline); }
    return result;
  }
  void SetUp() override {
    try { setup(); }
    catch (...) { FAIL()<<"decimal-receipt-setup-exception"; }
  }
  void setup() {
    const EnvironmentValue marker{"ODBCPP_MYSQL_DECIMAL_RECEIPT_FIXTURE_ADMITTED"},host{"ODBCPP_MYSQL_TEST_HOST"},
        port{"ODBCPP_MYSQL_TEST_PORT"},ca{"ODBCPP_MYSQL_TEST_CA_FILE"},
        user{"ODBCPP_MYSQL_TEST_USER"},password{"ODBCPP_MYSQL_TEST_PASSWORD"};
    ASSERT_TRUE(marker.value && std::string_view(marker.value)=="pinned-8.4.11-temporary-decimal-receipt-observation")<<"decimal-receipt-admission";
    ASSERT_TRUE(host.value && std::string_view(host.value)=="localhost")<<"decimal-receipt-host";
    ASSERT_TRUE(user.value && std::string_view(user.value)=="sdk")<<"decimal-receipt-user";
    ASSERT_TRUE(password.value && password.value[0])<<"decimal-receipt-password-config";
    ASSERT_TRUE(port.value && ca.value && ca.value[0])<<"decimal-receipt-endpoint-config";
    unsigned numeric_port{};const std::string_view text(port.value);
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),numeric_port);
    ASSERT_TRUE(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && numeric_port && numeric_port<=65535)<<"decimal-receipt-port";
    std::error_code file_error;
    ASSERT_TRUE(std::filesystem::path(ca.value).is_absolute() && std::filesystem::is_regular_file(ca.value,file_error) && !file_error)<<"decimal-receipt-ca";
    settings_.host=host.value;settings_.port=static_cast<std::uint16_t>(numeric_port);settings_.user=user.value;
    settings_.password=password.value;settings_.ssl_ca_file=ca.value;settings_.database="odbcpp";
    settings_.use_ssl=true;settings_.timeout=std::chrono::seconds(10);
    auto transport=std::make_unique<ReceiptTransport>();transport_=transport.get();
    session_=std::make_unique<mysql::MySqlSession>(std::move(transport));
    auto connected=session_->connect(settings_);ASSERT_TRUE(static_cast<bool>(connected))<<"decimal-receipt-connect";
    ASSERT_TRUE(transport_->peer_identity_verified())<<"decimal-receipt-verified-peer";
    ASSERT_TRUE(session_->server_version()=="8.4.11")<<"decimal-receipt-version";
    ASSERT_EQ(SessionState::Idle,connected.session_snapshot().state);ASSERT_EQ(SessionDisposition::Reusable,connected.session_snapshot().disposition);
    ASSERT_FALSE(transport_->deadlines.empty());
    for (const auto deadline:transport_->deadlines) { EXPECT_EQ(transport_->deadlines.front(),deadline); }
    deadline_=rs::util::make_deadline(std::chrono::seconds(30));
    auto modes=query("SELECT @@SESSION.sql_mode AS sql_mode");ASSERT_TRUE(static_cast<bool>(modes))<<"decimal-receipt-mode-inventory";
    ASSERT_EQ(1u,modes->rows.size());ASSERT_EQ(1u,modes->rows[0].size());ASSERT_TRUE(modes->rows[0][0]);
    ASSERT_TRUE(modes->cell_errors.empty());ASSERT_FALSE(modes->error);ASSERT_TRUE(modes->additional_results.empty());
    auto collision=query("SELECT CAST(COUNT(*) AS SIGNED) AS collisions FROM information_schema.tables WHERE table_schema='odbcpp' AND table_name='odbcpp_decimal_receipt_fixture'");
    ASSERT_TRUE(static_cast<bool>(collision))<<"decimal-receipt-collision-query";
    ASSERT_EQ(1u,collision->rows.size());ASSERT_EQ(1u,collision->rows[0].size());
    ASSERT_TRUE(collision->rows[0][0]==std::optional<std::string>{"0"})<<"decimal-receipt-name-collision";
    create_attempted_=true;
    auto created=query("CREATE TEMPORARY TABLE odbcpp_decimal_receipt_fixture (id INT PRIMARY KEY,d DECIMAL(5,2),neighbor BIGINT) ENGINE=InnoDB");
    ASSERT_TRUE(static_cast<bool>(created))<<"decimal-receipt-create";created_=true;
  }
  void zero_warnings() {
    auto warnings=query("SHOW COUNT(*) WARNINGS");ASSERT_TRUE(static_cast<bool>(warnings))<<"decimal-receipt-warning-inventory";
    ASSERT_EQ(1u,warnings->rows.size());ASSERT_EQ(1u,warnings->rows[0].size());
    ASSERT_TRUE(warnings->rows[0][0]==std::optional<std::string>{"0"})<<"decimal-receipt-warning-count";
    EXPECT_TRUE(warnings->cell_errors.empty());EXPECT_FALSE(warnings->error);EXPECT_TRUE(warnings->additional_results.empty());
  }
  void TearDown() override {
    try {
      if (created_ && !cleanup_attempted_ && session_ && session_->is_connected()) {
        cleanup_attempted_=true;deadline_=rs::util::make_deadline(std::chrono::seconds(5));
        auto dropped=query("DROP TEMPORARY TABLE odbcpp_decimal_receipt_fixture");
        EXPECT_TRUE(static_cast<bool>(dropped))<<"decimal-receipt-cleanup";
      }
    } catch (...) { ADD_FAILURE()<<"decimal-receipt-cleanup-exception"; }
    if (session_) { session_->disconnect();if (create_attempted_) { created_=false; } }
  }
};

TEST_F(MySqlDecimalReceiptObservationIntegrationTest, ActualCastParameterMetadataRefusalIsObserved) {
  auto inserted=query("INSERT INTO odbcpp_decimal_receipt_fixture VALUES (1,'1.20',42)");
  ASSERT_TRUE(static_cast<bool>(inserted))<<"decimal-receipt-insert";ASSERT_EQ(1u,inserted->affected_rows);
  zero_warnings();ASSERT_FALSE(HasFailure());
  auto direct=query("SELECT d,neighbor FROM odbcpp_decimal_receipt_fixture ORDER BY id");
  ASSERT_TRUE(static_cast<bool>(direct))<<"decimal-receipt-direct";zero_warnings();ASSERT_FALSE(HasFailure());
  ASSERT_EQ(2u,direct->columns.size());ASSERT_TRUE(direct->columns[0].normalized_type);
  const auto baseline=*direct->columns[0].normalized_type;
  EXPECT_TRUE(baseline.known);EXPECT_EQ(ScalarType::Decimal,baseline.type);EXPECT_EQ(5u,baseline.column_size);EXPECT_EQ(2,baseline.decimal_digits);
  ASSERT_EQ(1u,direct->rows.size());ASSERT_EQ(2u,direct->rows[0].size());
  ASSERT_TRUE(direct->rows[0][0]==std::optional<std::string>{"1.20"})<<"decimal-receipt-direct-value";
  ASSERT_TRUE(direct->rows[0][1]==std::optional<std::string>{"42"})<<"decimal-receipt-direct-neighbor";
  ASSERT_TRUE(direct->cell_errors.empty());ASSERT_FALSE(direct->error);ASSERT_TRUE(direct->additional_results.empty());
  ASSERT_FALSE(HasFailure());
  const auto original_deadline=deadline_;const auto closes=transport_->closes;
  transport_->arm();
  const std::array parameters{QueryParameter{std::string{"1.20"},QueryParameterType::Text}};
  auto rejected=session_->execute_prepared("SELECT d,neighbor FROM odbcpp_decimal_receipt_fixture WHERE d <=> CAST(? AS DECIMAL(5,2)) ORDER BY id",parameters,original_deadline);
  transport_->finish();
  const auto raw_records=transport_->observer.records();
  const auto count=transport_->observer.record_count();
  for (std::size_t i=0;i<count;++i) {
    const auto& raw=raw_records[i];
    std::cout<<"decimal-receipt-raw charset="<<raw.charset<<" type="<<unsigned(raw.type)
             <<" width="<<raw.width<<" decimals="<<unsigned(raw.decimals)<<" flags="<<raw.flags<<'\n';
  }
  ASSERT_FALSE(static_cast<bool>(rejected))<<"decimal-receipt-intentional-refusal";
  ASSERT_TRUE(transport_->proof_complete)<<"decimal-receipt-fragment-inconclusive";
  ASSERT_EQ(1u,transport_->prepare_commands);EXPECT_EQ(0u,transport_->execute_commands);EXPECT_EQ(0u,transport_->close_commands);
  ASSERT_EQ(1u,transport_->observer.parameter_count());ASSERT_EQ(2u,transport_->observer.result_count());
  ASSERT_EQ(1u,count)<<"decimal-receipt-missing-parameter-observation";
  EXPECT_FALSE(transport_->observer.records_truncated());EXPECT_EQ(Observer::Progress::Inconclusive,transport_->observer.progress());
  const auto raw=raw_records[0];ASSERT_EQ(246u,raw.type)<<"decimal-receipt-unexpected-native-id";
  // Actual raw flags supply unsignedness. Preserve current result-normalizer
  // structural-before-profile precedence; a successful Decimal normalization
  // reaches the existing result-only parameter refusal. No native width/q or
  // charset/profile assumption and no Numeric runtime enabling.
  const auto profile=mysql::decimal_detail::metadata(raw.width,raw.decimals,raw.unsigned_value());
  const auto expected=profile?rs::util::make_error_code(DbErrorCode::UnsupportedFeature):profile.error();
  EXPECT_TRUE(rejected.error()==expected)<<"decimal-receipt-exact-parser-policy";
  EXPECT_TRUE(rejected.error_message()=="MySQL session operation failed")<<"decimal-receipt-safe-diagnostic";
  EXPECT_EQ(BackendOperation::ExecutePrepared,rejected.backend_error().operation);
  EXPECT_EQ(SessionState::Disconnected,rejected.session_snapshot().state);EXPECT_EQ(SessionDisposition::Retire,rejected.session_snapshot().disposition);
  EXPECT_FALSE(session_->is_connected());EXPECT_EQ(closes+1,transport_->closes);
  ASSERT_FALSE(transport_->deadlines.empty());
  for (const auto deadline:transport_->deadlines) { EXPECT_EQ(original_deadline,deadline); }
  const DecimalReceiptEvidence evidence{raw,transport_->observer.parameter_count(),transport_->observer.result_count(),
      count,transport_->prepare_commands,transport_->execute_commands,transport_->close_commands,
      transport_->closes>=closes?transport_->closes-closes:0,
      transport_->observer.records_truncated(),transport_->observer.progress()==Observer::Progress::Complete,
      transport_->proof_complete,!session_->is_connected() && rejected.session_snapshot().disposition==SessionDisposition::Retire};
  session_->disconnect();EXPECT_EQ(closes+1,transport_->closes);session_.reset();
  EXPECT_EQ(raw.width,raw_records[0].width);EXPECT_EQ(raw.charset,raw_records[0].charset);
  EXPECT_EQ(raw.flags,raw_records[0].flags);EXPECT_EQ((raw.flags&32u)!=0,raw.unsigned_value());
  EXPECT_TRUE(direct->rows[0][0]==std::optional<std::string>{"1.20"});
  // No warning/recovery query after retirement. Physical disconnect destroys
  // the owned temp table; TearDown always handles earlier failures too.
  ASSERT_FALSE(HasFailure());
  // Publish only after all actual receipt, refusal, deadline and owning checks.
  // Schema/support0 are labels; captured numeric fields remain actual facts.
  record_decimal_receipt_properties(evidence);
}
}
