#include <gtest/gtest.h>
#include "odbcpp/auth/pg_credential_consumer.h"
#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/transport/tls_transport.h"
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {
namespace a = rs::core::auth;
namespace d = rs::core::database;
namespace t = rs::core::transport;
using Pg = d::postgres::PgDatabaseConnection;
const d::SessionSnapshot idle{d::SessionState::Idle,d::SessionDisposition::Reusable};
const d::SessionSnapshot retired{d::SessionState::Disconnected,d::SessionDisposition::Retire};
constexpr std::string_view query =
    "SELECT CAST(current_database() AS VARCHAR(64)) AS db,"
    "CAST(TRIM(current_user) AS VARCHAR(64)) AS principal,1 AS scalar,"
    "CAST(NULL AS INTEGER) AS nil,"
    "(SELECT ssl FROM pg_stat_ssl WHERE pid=pg_backend_pid()) AS tls";

struct Disconnect final {
  Pg& connection;
  ~Disconnect() { connection.disconnect(); }
};

class OrdinaryPgMaterialTls : public ::testing::Test {
protected:
  void SetUp() override {
    // Explicit existing disposable fixture only; never discover a server or
    // substitute an endpoint/default port/credentials when selection is absent.
    const char* port = std::getenv("PGPORT");
    const char* directory = std::getenv("ODBCPP_POSTGRES_TLS_DIR");
    ASSERT_NE(nullptr,port);ASSERT_NE(nullptr,directory);
    std::size_t size=0;while(size<=5&&port[size]!='\0') { ++size; }
    ASSERT_GT(size,0U);ASSERT_LE(size,5U);
    unsigned value=0;const auto parsed=std::from_chars(port,port+size,value);
    ASSERT_EQ(std::errc{},parsed.ec);ASSERT_EQ(port+size,parsed.ptr);
    ASSERT_GT(value,0U);ASSERT_LE(value,65535U);port_=static_cast<std::uint16_t>(value);
    size=0;while(size<=4096&&directory[size]!='\0') { ++size; }
    ASSERT_GT(size,0U);ASSERT_LE(size,4096U);
    const std::filesystem::path selected(std::string(directory,size));
    ASSERT_TRUE(selected.is_absolute());
    for(const auto& part:selected) { ASSERT_FALSE(part==".."); }
    ca_=(selected/"server.crt").string();wrong_ca_=(selected/"wrong.crt").string();
  }

  a::Outcome<a::Binding> binding() const {
    return a::Binding::create({a::Service::PostgreSql,"127.0.0.1",port_,"postgres","postgres",
        "disposable-postgres-tls","127.0.0.1","verify-full"},
        {a::SourceKind::ExternalPassword,"disposable-postgres-static-password","fixture-v1"},
        a::Method::OrdinaryPassword);
  }
  static a::Outcome<a::Material> material(const a::Binding& selected,std::string_view password) {
    auto secret=a::SecretBytes::create(std::as_bytes(std::span{password.data(),password.size()}));
    if(!secret) { return secret.error(); }
    return a::Material::create(selected,a::MaterialKind::OrdinaryPassword,"postgres",
        std::move(secret).value(),a::Validity::ordinary());
  }
  d::ConnectionSettings settings(bool wrong_ca=false) const {
    d::ConnectionSettings selected;selected.host="127.0.0.1";selected.port=port_;
    selected.database="postgres";selected.user="postgres";selected.use_ssl=true;
    selected.ssl_ca_file=wrong_ca?wrong_ca_:ca_;
    selected.timeout=std::chrono::milliseconds{0};
    return selected; // No fallback password or competing CA directory.
  }
  static std::unique_ptr<t::TLSTransport> transport() {
    auto selected=std::make_unique<t::TLSTransport>();
    selected->set_verify(true);selected->set_hostname_verification(true);
    return selected;
  }
  static void expect_owned(const d::QueryResult& result) {
    ASSERT_EQ(5U,result.columns.size());ASSERT_EQ(1U,result.rows.size());
    ASSERT_EQ(5U,result.rows[0].size());EXPECT_TRUE(result.additional_results.empty());
    EXPECT_TRUE(result.cell_errors.empty());EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(std::optional<d::StatementKind>{d::StatementKind::SelectCursor},result.statement_kind);
    const std::array<std::string_view,5> names{"db","principal","scalar","nil","tls"};
    const std::array<d::ScalarType,5> types{d::ScalarType::VarChar,d::ScalarType::VarChar,
        d::ScalarType::Integer,d::ScalarType::Integer,d::ScalarType::Boolean};
    const std::array<std::uint64_t,5> widths{64,64,10,10,1};
    for(std::size_t i=0;i<5;++i) {
      EXPECT_TRUE(result.columns[i].name==names[i]);
      ASSERT_TRUE(result.columns[i].normalized_type.has_value());
      EXPECT_TRUE(result.columns[i].normalized_type->known);
      EXPECT_EQ(types[i],result.columns[i].normalized_type->type);
      EXPECT_EQ(widths[i],result.columns[i].normalized_type->column_size);
      EXPECT_EQ(0,result.columns[i].normalized_type->decimal_digits);
    }
    ASSERT_TRUE(result.rows[0][0].has_value());EXPECT_TRUE(*result.rows[0][0]=="postgres");
    ASSERT_TRUE(result.rows[0][1].has_value());EXPECT_TRUE(*result.rows[0][1]=="postgres");
    EXPECT_EQ(std::optional<std::string>{"1"},result.rows[0][2]);
    EXPECT_FALSE(result.rows[0][3].has_value());
    EXPECT_EQ(std::optional<std::string>{"1"},result.rows[0][4]);
  }
  std::uint16_t port_{};std::string ca_,wrong_ca_;
};

TEST_F(OrdinaryPgMaterialTls, VerifiedScramMaterialConnectQueryOwnsAfterDisconnect) {
  const auto deadline=rs::util::make_deadline(std::chrono::seconds{10});
  auto selected=binding();ASSERT_TRUE(selected);
  auto requested=a::Request::create(selected.value(),deadline,std::chrono::seconds{1});ASSERT_TRUE(requested);
  auto value=material(selected.value(),"postgres");ASSERT_TRUE(value);
  const auto config=settings();EXPECT_TRUE(config.password.empty());
  std::optional<d::QueryResult> owned;
  {
    auto wire=transport();auto* peer=wire.get();Pg pg(std::move(wire));Disconnect cleanup{pg};
    const auto connected=a::connect_bound_ordinary_password_until(pg,config,requested.value(),value.value());
    ASSERT_TRUE(connected) << connected.backend_error().safe_summary();
    EXPECT_EQ(idle,connected.session_snapshot());ASSERT_TRUE(pg.is_connected());
    ASSERT_TRUE(peer->peer_identity_verified());
    auto result=pg.execute_query(query,deadline);ASSERT_TRUE(result) << result.backend_error().safe_summary();
    EXPECT_EQ(idle,result.session_snapshot());expect_owned(result.value());ASSERT_FALSE(HasFailure());
    owned.emplace(std::move(result).value());
    pg.disconnect();EXPECT_FALSE(pg.is_connected());EXPECT_EQ(idle,connected.session_snapshot());
  }
  ASSERT_TRUE(owned.has_value());expect_owned(*owned); // After backend destruction too.
  EXPECT_EQ(deadline,requested.value().deadline());EXPECT_TRUE(config.password.empty());
}

TEST_F(OrdinaryPgMaterialTls, NativeRefusalRetiresAndFreshMaterialRecovers) {
  const auto deadline=rs::util::make_deadline(std::chrono::seconds{10});
  auto selected=binding();ASSERT_TRUE(selected);
  auto requested=a::Request::create(selected.value(),deadline,std::chrono::seconds{1});ASSERT_TRUE(requested);
  std::optional<d::BackendError> retained;
  {
    auto wrong=material(selected.value(),"synthetic-invalid-password");ASSERT_TRUE(wrong);
    auto wire=transport();Pg pg(std::move(wire));Disconnect cleanup{pg};
    const auto result=a::connect_bound_ordinary_password_until(pg,settings(),requested.value(),wrong.value());
    ASSERT_FALSE(result);EXPECT_EQ(d::BackendErrorClass::Authentication,result.backend_error().error_class);
    EXPECT_EQ(std::optional<std::string>{"28P01"},result.backend_error().native_state);
    EXPECT_EQ(retired,result.session_snapshot());EXPECT_FALSE(pg.is_connected());retained=result.backend_error();
  }
  {
    auto valid=material(selected.value(),"postgres");ASSERT_TRUE(valid);
    auto wire=transport();auto* peer=wire.get();Pg pg(std::move(wire));Disconnect cleanup{pg};
    const auto result=a::connect_bound_ordinary_password_until(pg,settings(true),requested.value(),valid.value());
    ASSERT_FALSE(result);EXPECT_EQ(d::BackendErrorClass::Tls,result.backend_error().error_class);
    EXPECT_EQ(retired,result.session_snapshot());EXPECT_FALSE(pg.is_connected());
    EXPECT_FALSE(peer->peer_identity_verified());
  }
  {
    auto fresh=material(selected.value(),"postgres");ASSERT_TRUE(fresh);
    auto wire=transport();auto* peer=wire.get();Pg pg(std::move(wire));Disconnect cleanup{pg};
    const auto result=a::connect_bound_ordinary_password_until(pg,settings(),requested.value(),fresh.value());
    ASSERT_TRUE(result) << result.backend_error().safe_summary();EXPECT_EQ(idle,result.session_snapshot());
    ASSERT_TRUE(peer->peer_identity_verified());ASSERT_TRUE(pg.is_connected());
    const auto rows=pg.execute_query(query,deadline);ASSERT_TRUE(rows) << rows.backend_error().safe_summary();
    EXPECT_EQ(idle,rows.session_snapshot());expect_owned(rows.value());ASSERT_FALSE(HasFailure());
    pg.disconnect();EXPECT_FALSE(pg.is_connected());
  }
  ASSERT_TRUE(retained.has_value());EXPECT_EQ(d::BackendErrorClass::Authentication,retained->error_class);
  EXPECT_EQ(std::optional<std::string>{"28P01"},retained->native_state);
  EXPECT_EQ(d::SessionState::Disconnected,retained->session_state);EXPECT_EQ(d::SessionDisposition::Retire,retained->disposition);
  EXPECT_EQ(deadline,requested.value().deadline());
}
} // namespace

#include "odbcpp/database/backend_provider.h"
namespace {
TEST_F(OrdinaryPgMaterialTls, ProviderCreatedDriverPasswordRouteUsesVerifiedTls) {
  const auto query_cutoff=rs::util::make_deadline(std::chrono::seconds{10});
  const auto& provider=d::configured_backend_provider();ASSERT_TRUE(provider.identity().id=="postgresql");
  auto selected=settings();selected.password="postgres";selected.timeout=std::chrono::seconds{10};
  auto wire=transport();auto* peer=wire.get();auto session=provider.create_session(std::move(wire));ASSERT_TRUE(session);
  struct Cleanup { d::IDatabaseConnection& session;~Cleanup() { session.disconnect(); } } cleanup{*session};
  // This is the real driver/provider virtual connect entry. Its existing API
  // computes one operation deadline internally; query has the earlier fixed cutoff.
  const auto connected=session->connect(selected);ASSERT_TRUE(connected) << connected.backend_error().safe_summary();
  EXPECT_EQ(idle,connected.session_snapshot());ASSERT_TRUE(session->is_connected());ASSERT_TRUE(peer->peer_identity_verified());
  auto result=session->execute_query(query,query_cutoff);ASSERT_TRUE(result) << result.backend_error().safe_summary();
  EXPECT_EQ(idle,result.session_snapshot());expect_owned(result.value());ASSERT_FALSE(HasFailure());
  auto owned=std::move(result).value();session->disconnect();EXPECT_FALSE(session->is_connected());
  expect_owned(owned);EXPECT_TRUE(selected.password=="postgres");
}
} // namespace
