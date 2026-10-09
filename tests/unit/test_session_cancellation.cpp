#include <gtest/gtest.h>
#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/transport/cancellation_wait.h"
#include "odbcpp/transport/start_tls_transport.h"
#include "odbcpp/transport/tls_configurable_transport.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
using namespace rs::core::database;
using rs::util::Deadline;
using rs::util::DbErrorCode;
using rs::util::Result;
using rs::core::transport::IOResult;
using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
Bytes literal(std::initializer_list<unsigned char> bytes) {
  Bytes result; for (auto byte : bytes) result.push_back(static_cast<std::byte>(byte)); return result;
}
const Bytes startup = literal({
  'R',0,0,0,8,0,0,0,0,
  'K',0,0,0,12,0,0,0,23,1,2,3,4,
  'Z',0,0,0,5,'I'});
const Bytes cancelled_idle = literal({
  'E',0,0,0,25,'S','E','R','R','O','R',0,'C','5','7','0','1','4',0,'M','s','t','o','p',0,0,
  'Z',0,0,0,5,'I'});
const Bytes normal_idle = literal({'C',0,0,0,13,'S','E','L','E','C','T',' ','0',0,'Z',0,0,0,5,'I'});
const Bytes expected_packet = literal({0,0,0,16,4,210,22,46,0,0,0,23,1,2,3,4});
struct Script {
  std::mutex mutex;
  std::condition_variable changed;
  Bytes startup_bytes{startup}, reply{cancelled_idle}, packet;
  bool query_sent{}, release{}, peer_throw{}, peer_eof{true}, supported{true};
  bool hold_eof{}, peer_waiting{}, release_eof{};
  bool hold_main_eof{}, hold_ready{}, before_ready_waiting{}, release_ready{};
  bool hold_normalization{}, normalization_waiting{}, release_normalization{};
  bool hold_resolver{}, resolver_waiting{}, release_resolver{};
  int resolver_query_number{};
  bool partial_write_failure{}, partial_write_waiting{}, release_write{};
  int partial_writes{};
  std::shared_ptr<rs::core::transport::CancellationWait> main_control;
  bool tls{}, peer_tls_proof{true};
  char peer_tls_reply{'S'};
  bool extra_secondary_data{};
  std::string peer_hostname;
  std::size_t main_fragment{0}, secondary_fragment{0};
  Bytes ssl_request;
  std::vector<Bytes> queued_replies;
  int secondary_connects{}, closes{}, peer_closes{}, queries{}, captures{};
  Deadline secondary_deadline{};
};
class Wire final : public rs::core::transport::ITransport,
                   public rs::core::transport::IServerCancelTransport,
                   public rs::core::transport::IStartTlsTransport,
                   public rs::core::transport::ITlsConfigurableTransport {
 public:
  Wire(std::shared_ptr<Script> script, bool secondary=false) : script_(std::move(script)), secondary_(secondary) {}
  Result<void> connect(std::string_view, std::uint16_t, Deadline) override { return {}; }
  Result<IOResult> send(std::span<const std::byte> bytes, Deadline) override {
    std::unique_lock lock(script_->mutex);
    if(!secondary_ && startup_sent_ && script_->partial_write_failure) {
      if(script_->partial_writes++==0) {++script_->queries;return IOResult{1,false};}
      script_->partial_write_waiting=true;script_->changed.notify_all();
      if(!script_->changed.wait_for(lock,2s,[&]{return script_->release_write;}))return {DbErrorCode::Timeout,"partial write barrier timeout"};
      return {DbErrorCode::NetworkError,"partial main write failed"};
    }
    if (script_->tls && !ssl_request_sent_) {
      ssl_request_sent_=true;
      if (secondary_)script_->ssl_request.assign(bytes.begin(),bytes.end());
      return IOResult{bytes.size(),false};
    }
    if (secondary_) {
      if (script_->peer_throw) throw std::runtime_error("injected secondary send");
      const auto count=script_->secondary_fragment?(std::min)(bytes.size(),script_->secondary_fragment):bytes.size();
      script_->packet.insert(script_->packet.end(),bytes.begin(),bytes.begin()+count);
      if(script_->packet.size()==16){script_->release=true;script_->changed.notify_all();}
      return IOResult{count,false};
    } else if (startup_sent_) {
      ++script_->queries; reply_offset_=0;
      if (script_->hold_resolver && script_->queries == script_->resolver_query_number) {
        script_->resolver_waiting = true;
        script_->changed.notify_all();
        if (!script_->changed.wait_for(lock, 2s, [&] { return script_->release_resolver; }))
          return {DbErrorCode::Timeout, "resolver transport barrier timeout"};
      }
      if(!script_->queued_replies.empty()) {
        script_->reply=std::move(script_->queued_replies.front());script_->queued_replies.erase(script_->queued_replies.begin());
      }
      script_->query_sent = true; script_->changed.notify_all();
    } else startup_sent_ = true;
    return IOResult{bytes.size(), false};
  }
  Result<IOResult> recv(std::span<std::byte> bytes, Deadline deadline) override {
    std::unique_lock lock(script_->mutex);
    if (script_->tls && ssl_request_sent_ && !ssl_reply_read_) {
      ssl_reply_read_=true;
      bytes[0]=static_cast<std::byte>(secondary_?script_->peer_tls_reply:'S');
      return IOResult{1,false};
    }
    if (secondary_) {
      if (script_->hold_eof) {
        script_->peer_waiting=true; script_->changed.notify_all();
        if (!script_->changed.wait_until(lock,deadline,[&]{return script_->release_eof;}))
          return {DbErrorCode::Timeout,"secondary EOF barrier timeout"};
      }
      if(script_->extra_secondary_data){bytes[0]=std::byte{42};return IOResult{1,false};}
      return script_->peer_eof ? Result<IOResult>{IOResult{0,true}} :
          Result<IOResult>{DbErrorCode::NetworkError, "secondary settlement unknown"};
    }
    const auto* source = &script_->startup_bytes;
    if (offset_ == script_->startup_bytes.size()) {
      while (!script_->release) {
        const auto cutoff=wait_?wait_->effective(deadline):deadline;
        if (Deadline::clock::now()>=cutoff)return {DbErrorCode::Timeout,"main drain timeout"};
        script_->changed.wait_until(lock,(std::min)(cutoff,Deadline::clock::now()+50ms));
      }
      source = &script_->reply;
    }
    auto& offset = source == &script_->reply ? reply_offset_ : offset_;
    if(source==&script_->reply && script_->hold_ready && offset==source->size()-6) {
      script_->before_ready_waiting=true;script_->changed.notify_all();
      if(!script_->changed.wait_until(lock,deadline,[&]{return script_->release_ready;}))return {DbErrorCode::Timeout,"READY barrier timeout"};
    }
    if (offset == source->size()) {
      if(script_->hold_main_eof) {
        while(Deadline::clock::now()<(wait_?wait_->effective(deadline):deadline))
          script_->changed.wait_until(lock,(std::min)(wait_?wait_->effective(deadline):deadline,Deadline::clock::now()+50ms));
        return {DbErrorCode::Timeout,"held partial frame reached deadline"};
      }
      return IOResult{0,true};
    }
    const auto count = (std::min)((std::min)(bytes.size(),source->size()-offset),script_->main_fragment?script_->main_fragment:bytes.size());
    std::copy_n(source->begin()+offset,count,bytes.begin()); offset+=count;
    return IOResult{count,false};
  }
  void close() noexcept override {
    std::lock_guard lock(script_->mutex);
    if (secondary_) ++script_->peer_closes; else ++script_->closes;
  }
  Result<void> connect_plain(std::string_view host,std::uint16_t port,Deadline deadline) override { return connect(host,port,deadline); }
  Result<void> upgrade_to_tls(std::string_view host,Deadline) override {
    if(secondary_){std::lock_guard lock(script_->mutex);script_->peer_hostname=std::string(host);proof_=script_->peer_tls_proof;}
    else proof_=true;
    return {};
  }
  bool peer_identity_verified() noexcept override {return proof_;}
  void set_ca_locations(const std::string&,const std::string&) override {}
  bool supports_server_cancel() const noexcept override { return script_->supported; }
  void cancellation_wait(std::shared_ptr<rs::core::transport::CancellationWait> wait) noexcept override {
    wait_=std::move(wait);
    if(!secondary_){std::lock_guard lock(script_->mutex);script_->main_control=wait_;}
  }
  std::unique_ptr<rs::core::transport::ITransport> cancellation_peer() const override {
    std::lock_guard lock(script_->mutex); ++script_->captures;
    return std::make_unique<Wire>(script_,true);
  }
  Result<void> connect_cancellation_peer(Deadline deadline) override {
    std::lock_guard lock(script_->mutex); ++script_->secondary_connects; script_->secondary_deadline=deadline;
    return {};
  }
 private:
  std::shared_ptr<Script> script_;
  bool secondary_{false}, startup_sent_{false}, ssl_request_sent_{false}, ssl_reply_read_{false}, proof_{false};
  std::size_t offset_{0}, reply_offset_{0};
  std::shared_ptr<rs::core::transport::CancellationWait> wait_;
};
ConnectionSettings settings() { ConnectionSettings result; result.port=5432; result.host="synthetic.invalid"; result.use_ssl=false; result.user="synthetic"; result.database="synthetic"; return result; }
bool wait_query(const std::shared_ptr<Script>& script) {
  std::unique_lock lock(script->mutex);
  return script->changed.wait_for(lock,2s,[&] {return script->query_sent;});
}
}

TEST(SessionCancellationTest, LiteralCancelKeyConfirmedReadyIdleAndSameSessionRecovery) {
  auto script=std::make_shared<Script>();
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  auto connected=connection.connect(settings()); ASSERT_TRUE(connected) << connected.error_message();
  ASSERT_TRUE(connection.supports_server_cancellation());
  const auto deadline=Deadline::clock::now()+3s;
  auto operation=connection.arm_cancellation(1,deadline); ASSERT_TRUE(operation);
  auto execution=std::async(std::launch::async,[&] {return connection.execute_query("SELECT 0",deadline);});
  ASSERT_TRUE(wait_query(script));
  EXPECT_TRUE(operation->request());
  auto result=execution.get(); ASSERT_TRUE(result.has_error());
  EXPECT_EQ("57014",result.backend_error().native_state);
  const auto outcome=connection.finish_cancellation(operation);
  EXPECT_TRUE(outcome.claimed); EXPECT_TRUE(outcome.confirmed); EXPECT_FALSE(outcome.retire);
  EXPECT_EQ(SessionState::Idle,connection.session_state());
  EXPECT_EQ(expected_packet,script->packet); EXPECT_EQ(1,script->captures); EXPECT_EQ(1,script->secondary_connects);
  EXPECT_TRUE(operation->request()); EXPECT_EQ(1,script->secondary_connects);
  script->reply=normal_idle;
  auto recovered=connection.execute_query("SELECT 0",deadline);
  ASSERT_TRUE(recovered.has_value()); EXPECT_EQ(2,script->queries);
  EXPECT_EQ(0,script->closes); EXPECT_EQ(1,script->secondary_connects);
}

TEST(SessionCancellationTest, UnlimitedOriginalDeadlineStillFreezesFiniteSecondaryCutoff) {
  auto script=std::make_shared<Script>();
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  auto connected=connection.connect(settings()); ASSERT_TRUE(connected) << connected.error_message();
  auto operation=connection.arm_cancellation(2,Deadline::max()); ASSERT_TRUE(operation);
  auto execution=std::async(std::launch::async,[&] {return connection.execute_query("SELECT 0",Deadline::max());});
  ASSERT_TRUE(wait_query(script)); const auto before=Deadline::clock::now();
  ASSERT_TRUE(operation->request()); const auto after=Deadline::clock::now();
  EXPECT_GE(script->secondary_deadline,before+5s); EXPECT_LE(script->secondary_deadline,after+5s);
  EXPECT_LT(script->secondary_deadline,Deadline::max());
  EXPECT_TRUE(execution.get().has_error()); EXPECT_TRUE(connection.finish_cancellation(operation).confirmed);
}

TEST(SessionCancellationTest, BeforeWireCancellationIsZeroIoAndDuplicateDoesNotRetarget) {
  auto script=std::make_shared<Script>();
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  auto connected=connection.connect(settings()); ASSERT_TRUE(connected) << connected.error_message();
  EXPECT_FALSE(connection.arm_cancellation(0,Deadline::max()));
  auto operation=connection.arm_cancellation(3,Deadline::max()); ASSERT_TRUE(operation);
  EXPECT_FALSE(connection.arm_cancellation(4,Deadline::max()));
  EXPECT_TRUE(operation->request()); EXPECT_TRUE(operation->request());
  auto result=connection.execute_query("SELECT 0",Deadline::max()); EXPECT_TRUE(result.has_error());
  const auto outcome=connection.finish_cancellation(operation);
  EXPECT_TRUE(outcome.confirmed); EXPECT_FALSE(outcome.retire);
  EXPECT_EQ(0,script->queries); EXPECT_EQ(0,script->secondary_connects); EXPECT_TRUE(script->packet.empty());
}

TEST(SessionCancellationTest, EscapedCancelNormalCompletionRaceRetiresWithoutReusingGeneration) {
  auto script=std::make_shared<Script>(); script->reply=normal_idle;
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  auto connected=connection.connect(settings()); ASSERT_TRUE(connected) << connected.error_message();
  const auto deadline=Deadline::clock::now()+3s;
  auto operation=connection.arm_cancellation(4,deadline); ASSERT_TRUE(operation);
  auto execution=std::async(std::launch::async,[&] {return connection.execute_query("SELECT 0",deadline);});
  ASSERT_TRUE(wait_query(script)); EXPECT_TRUE(operation->request());
  auto result=execution.get(); ASSERT_TRUE(result.has_value());
  auto outcome=connection.finish_cancellation(operation);
  EXPECT_TRUE(outcome.claimed); EXPECT_FALSE(outcome.confirmed); EXPECT_TRUE(outcome.retire);
  EXPECT_FALSE(connection.is_connected()); EXPECT_EQ(1,script->closes);
  EXPECT_FALSE(connection.arm_cancellation(5,Deadline::max()));
  EXPECT_TRUE(operation->request()); EXPECT_EQ(1,script->secondary_connects);
}

TEST(SessionCancellationTest, ThrowingSecondarySendAndUnknownEofRequireRetirement) {
  for (const int mode : {0,1,2}) {
    auto script=std::make_shared<Script>(); script->peer_throw=mode==1; script->peer_eof=false;script->extra_secondary_data=mode==2;
    postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
    auto connected=connection.connect(settings()); ASSERT_TRUE(connected) << connected.error_message();
    const auto deadline=Deadline::clock::now()+200ms;
    auto operation=connection.arm_cancellation(6,deadline); ASSERT_TRUE(operation);
    auto execution=std::async(std::launch::async,[&] {return connection.execute_query("SELECT 0",deadline);});
    ASSERT_TRUE(wait_query(script)); EXPECT_FALSE(operation->request());
    {std::lock_guard lock(script->mutex); script->release=true;script->changed.notify_all();}
    EXPECT_TRUE(execution.get().has_error());
    const auto outcome=connection.finish_cancellation(operation);
    EXPECT_TRUE(outcome.retire); EXPECT_FALSE(outcome.confirmed); EXPECT_FALSE(connection.is_connected());
    EXPECT_EQ(1,script->closes); EXPECT_EQ(1,script->peer_closes);
  }
}

TEST(SessionCancellationTest, MissingDuplicateMalformedKeysAndUnsupportedCarrierFailClosed) {
  for (const int mode : {0,1,2,3}) {
    auto script=std::make_shared<Script>();
    if (mode==0) script->startup_bytes=literal({'R',0,0,0,8,0,0,0,0,'Z',0,0,0,5,'I'});
    if (mode==1) script->startup_bytes.insert(script->startup_bytes.end()-6,startup.begin()+9,startup.begin()+22);
    if (mode==2) script->startup_bytes=literal({'R',0,0,0,8,0,0,0,0,'K',0,0,0,5,0,'Z',0,0,0,5,'I'});
    if (mode==3) script->supported=false;
    postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
    auto result=connection.connect(settings());
    if(mode==1||mode==2) EXPECT_TRUE(result.has_error()); else EXPECT_TRUE(result.has_value());
    EXPECT_FALSE(connection.supports_server_cancellation());
    EXPECT_FALSE(connection.arm_cancellation(7,Deadline::max())); EXPECT_EQ(0,script->captures);
  }
}

TEST(SessionCancellationTest, CompletedBeforeClaimIsIdleNoopAndExpiredClaimSendsNothing) {
  for (const bool expired : {false,true}) {
    auto script=std::make_shared<Script>(); script->reply=normal_idle; script->release=true;
    postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
    auto connected=connection.connect(settings()); ASSERT_TRUE(connected) << connected.error_message();
    const auto deadline=Deadline::clock::now()+2s;
    auto operation=connection.arm_cancellation(8,deadline); ASSERT_TRUE(operation);
    if (expired) { EXPECT_FALSE(connection.arm_cancellation(9,Deadline::min())); }
    auto result=connection.execute_query("SELECT 0",deadline); ASSERT_TRUE(result.has_value());
    auto outcome=connection.finish_cancellation(operation); EXPECT_FALSE(outcome.claimed); EXPECT_FALSE(outcome.confirmed); EXPECT_FALSE(outcome.retire);
    EXPECT_TRUE(operation->request()); // whole operation sealed; genuine idle no-op
    EXPECT_FALSE(operation->seal().claimed);
    EXPECT_EQ(0,script->secondary_connects); EXPECT_EQ(SessionState::Idle,connection.session_state());
  }
}

// Public adapter tests use the real PostgreSQL parser/session and the existing
// per-connection provider seam. Only transport I/O is synthetic.
#include "core/database/postgres/pg_backend_provider.h"
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"

namespace {
const Bytes integer_row = literal({
  'T',0,0,0,26,0,1,'v',0,0,0,0,0,0,0,0,0,0,23,0,4,255,255,255,255,0,0,
  'D',0,0,0,11,0,1,0,0,0,1,'7',
  'C',0,0,0,13,'S','E','L','E','C','T',' ','1',0,
  'Z',0,0,0,5,'I'});
// Literal unknown parameter OID90000 drives the real Pg catalog resolver.
// The three text fields encode original90000/base23/modifier-1 independently.
const Bytes resolver_rows = literal({
  'T',0,0,0,66,0,3,
  'o',0,0,0,0,0,0,0,0,0,0,25,255,255,255,255,255,255,0,0,
  'b',0,0,0,0,0,0,0,0,0,0,25,255,255,255,255,255,255,0,0,
  'm',0,0,0,0,0,0,0,0,0,0,25,255,255,255,255,255,255,0,0,
  'D',0,0,0,27,0,3,0,0,0,5,'9','0','0','0','0',0,0,0,2,'2','3',0,0,0,2,'-','1',
  'C',0,0,0,13,'S','E','L','E','C','T',' ','1',0,
  'Z',0,0,0,5,'I'});
Bytes prepared_unknown_rows() {
  auto result = literal({'1',0,0,0,4,'t',0,0,0,10,0,1,0,1,95,144,'2',0,0,0,4});
  result.insert(result.end(), integer_row.begin(), integer_row.end());
  return result;
}
class PublicationBarrierPg final : public postgres::PgDatabaseConnection {
 public:
  explicit PublicationBarrierPg(std::shared_ptr<Script> script) : PgDatabaseConnection(std::make_unique<Wire>(script)), script_(std::move(script)) {}
  NativeTypeInfo describe_type(std::uint32_t id,std::int16_t size,std::int32_t modifier) const override {
    {
      std::unique_lock lock(script_->mutex);
      if(script_->hold_normalization) {
        script_->normalization_waiting=true;script_->changed.notify_all();
        if(!script_->changed.wait_for(lock,2s,[&]{return script_->release_normalization;}))
          throw std::runtime_error("owning publication barrier timeout");
      }
    }
    return PgDatabaseConnection::describe_type(id,size,modifier);
  }
 private:
  std::shared_ptr<Script> script_;
};
class WireProvider final : public IBackendProvider {
 public:
  explicit WireProvider(std::shared_ptr<Script> script) : script_(std::move(script)),
      profile_({"synthetic", "PostgreSQL", "synthetic"}, {"synthetic.invalid",5432,std::string{"synthetic"},false}) {}
  const BackendIdentity& identity() const noexcept override {return profile_.identity();}
  const BackendConnectionDefaults& connection_defaults() const noexcept override {return profile_.connection_defaults();}
  const ISqlDialect& sql_dialect() const noexcept override {return profile_.sql_dialect();}
  BackendCapabilities capabilities() const noexcept override {return profile_.capabilities();}
  std::optional<std::string> normalize_error_sqlstate(std::string_view state,ErrorContext context) const override {return profile_.normalize_error_sqlstate(state,context);}
  std::span<const TypeDefinition> type_catalog(std::string_view version) const noexcept override {return profile_.type_catalog(version);}
  TransactionCapabilities transaction_capabilities() const noexcept override {return profile_.transaction_capabilities();}
  Result<ConnectionSettings> resolve_connection_options(ConnectionOptions options) const override {return profile_.resolve_connection_options(std::move(options));}
  std::unique_ptr<IDatabaseConnection> create_session(std::unique_ptr<rs::core::transport::ITransport>) const override {
    return std::make_unique<PublicationBarrierPg>(script_);
  }
 private:
  std::shared_ptr<Script> script_;
  postgres::PgBackendProvider profile_;
};
class PublicCancellationTest : public ::testing::Test {
 protected:
  std::shared_ptr<Script> script=std::make_shared<Script>();
  SQLHENV env{}; SQLHDBC dbc{}; SQLHSTMT stmt{}, sibling{};
  struct OutputGuard {std::uint32_t before{0x12345678};SQLINTEGER value{73};std::uint32_t after{0x87654321};} bound_output;
  SQLLEN bound_output_length{93};
  SQLINTEGER bound_input{7};SQLLEN bound_input_length{0};
  SQLUSMALLINT bound_parameter_status{SQL_PARAM_UNUSED};SQLULEN bound_processed{73};
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_ENV,nullptr,&env));
    ASSERT_EQ(SQL_SUCCESS,SQLSetEnvAttr(env,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0));
    auto connection=std::make_unique<rs::odbc::ODBCConnection>(nullptr,std::make_shared<WireProvider>(script));
    dbc=reinterpret_cast<SQLHDBC>(connection.get());
    rs::odbc::HandleRegistry::instance().register_handle(dbc,std::move(connection),env);
    ASSERT_EQ(SQL_SUCCESS,SQLDriverConnect(dbc,nullptr,reinterpret_cast<SQLCHAR*>(const_cast<char*>("SERVER=synthetic.invalid;PORT=5432;DATABASE=synthetic;UID=synthetic;SSL=0")),SQL_NTS,nullptr,0,nullptr,SQL_DRIVER_NOPROMPT));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc,&stmt));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc,&sibling));
  }
  SQLRETURN execute(SQLHSTMT handle,const char* sql="SELECT 7") {
    return SQLExecDirect(handle,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS);
  }
  std::string state() {
    SQLCHAR value[6]{};
    EXPECT_EQ(SQL_SUCCESS,SQLGetDiagRec(SQL_HANDLE_STMT,stmt,1,value,nullptr,nullptr,0,nullptr));
    return reinterpret_cast<const char*>(value);
  }
  void TearDown() override {
    if(sibling)SQLFreeHandle(SQL_HANDLE_STMT,sibling);
    if(stmt)SQLFreeHandle(SQL_HANDLE_STMT,stmt);
    if(dbc){SQLDisconnect(dbc);SQLFreeHandle(SQL_HANDLE_DBC,dbc);}
    if(env)SQLFreeHandle(SQL_HANDLE_ENV,env);
  }
};
}

TEST_F(PublicCancellationTest, CancelBypassesExecutingLockPreservesDiagnosticsAndSiblingRows) {
  script->reply=integer_row;script->release=true;
  ASSERT_EQ(SQL_SUCCESS,execute(sibling));
  script->reply=cancelled_idle;script->release=false;script->query_sent=false;script->hold_eof=true;
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt,1,SQL_C_SLONG,&bound_output.value,sizeof(bound_output.value),&bound_output_length));
  const auto target=rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCStatement>(stmt);
  target->set_error("22018","owning earlier diagnostic");
  auto execution=std::async(std::launch::async,[&]{return execute(stmt);});
  ASSERT_TRUE(wait_query(script));
  EXPECT_EQ(SQL_SUCCESS,SQLCancel(sibling));EXPECT_EQ(0,script->secondary_connects);
  const auto before=target->get_diagnostic_count();
  const auto header=target->get_last_return_code();
  auto cancel=std::async(std::launch::async,[&]{return SQLCancel(stmt);});
  {
    std::unique_lock lock(script->mutex);
    ASSERT_TRUE(script->changed.wait_for(lock,2s,[&]{return script->peer_waiting;}));
  }
  EXPECT_EQ(before,target->get_diagnostic_count());
  EXPECT_EQ(header,target->get_last_return_code());
  {std::lock_guard lock(script->mutex);script->release_eof=true;script->changed.notify_all();}
  EXPECT_EQ(SQL_SUCCESS,cancel.get());
  EXPECT_EQ(SQL_ERROR,execution.get()); EXPECT_EQ("HY008",state());
  EXPECT_EQ(SQL_SUCCESS,SQLFetch(sibling));
  SQLINTEGER value=-1;SQLLEN indicator=-1;
  EXPECT_EQ(SQL_SUCCESS,SQLGetData(sibling,1,SQL_C_SLONG,&value,sizeof(value),&indicator));
  EXPECT_EQ(7,value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)),indicator);
  EXPECT_EQ(SQL_ERROR,SQLFetch(stmt));EXPECT_EQ("HY010",state());
  EXPECT_EQ(73,bound_output.value);EXPECT_EQ(93,bound_output_length);
  EXPECT_EQ(0x12345678u,bound_output.before);EXPECT_EQ(0x87654321u,bound_output.after);
  EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,1,SQL_C_SLONG,&bound_output.value,sizeof(bound_output.value),&bound_output_length));EXPECT_EQ("24000",state());
  EXPECT_EQ(73,bound_output.value);EXPECT_EQ(93,bound_output_length);
  EXPECT_EQ(1,script->secondary_connects);
  script->reply=integer_row;script->release=true;
  EXPECT_EQ(SQL_SUCCESS,execute(stmt));EXPECT_EQ(SQL_SUCCESS,SQLFetch(stmt));
  EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt)); // idle cursor remains usable
  EXPECT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,SQL_C_SLONG,&value,sizeof(value),&indicator));EXPECT_EQ(7,value);
}

TEST_F(PublicCancellationTest, NormalCompletionRaceInvalidatesOnlyTargetAndRetiresPhysicalSession) {
  script->reply=integer_row;script->release=true;
  ASSERT_EQ(SQL_SUCCESS,execute(sibling));
  script->release=false;script->query_sent=false;
  auto execution=std::async(std::launch::async,[&]{return execute(stmt);});
  ASSERT_TRUE(wait_query(script));EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));
  EXPECT_EQ(SQL_SUCCESS,execution.get());
  EXPECT_EQ(SQL_ERROR,SQLFetch(stmt));EXPECT_EQ("HY010",state());
  EXPECT_EQ(SQL_SUCCESS,SQLFetch(sibling));
  SQLINTEGER value=-1;SQLLEN indicator=-1;
  EXPECT_EQ(SQL_SUCCESS,SQLGetData(sibling,1,SQL_C_SLONG,&value,sizeof(value),&indicator));EXPECT_EQ(7,value);
  EXPECT_EQ(SQL_ERROR,execute(stmt));EXPECT_EQ(2,script->queries);EXPECT_EQ(1,script->closes);
}

TEST_F(PublicCancellationTest, UnsupportedCompoundRefusesWithoutPacketAndSingleTrailingQuotedDelimiterWorks) {
  auto execution=std::async(std::launch::async,[&]{return execute(stmt,"SELECT 7; SELECT 7");});
  ASSERT_TRUE(wait_query(script));EXPECT_EQ(SQL_ERROR,SQLCancel(stmt));EXPECT_TRUE(script->packet.empty());
  {std::lock_guard lock(script->mutex);script->reply=integer_row;script->release=true;script->changed.notify_all();}
  EXPECT_EQ(SQL_SUCCESS,execution.get());ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
  script->reply=cancelled_idle;script->release=false;script->query_sent=false;
  auto single=std::async(std::launch::async,[&]{return execute(stmt,"SELECT ';' /* ; nested /* ; */ */; -- ;\n");});
  ASSERT_TRUE(wait_query(script));EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));
  EXPECT_EQ(SQL_ERROR,single.get());EXPECT_EQ("HY008",state());EXPECT_EQ(1,script->secondary_connects);
}

TEST_F(PublicCancellationTest, PendingAutomaticBeginCancellationNeverDispatchesUserQuery) {
  ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(dbc,SQL_ATTR_AUTOCOMMIT,reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF),0));
  auto execution=std::async(std::launch::async,[&]{return execute(stmt);});
  ASSERT_TRUE(wait_query(script));EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));
  EXPECT_EQ(SQL_ERROR,execution.get());EXPECT_EQ("HY008",state());
  EXPECT_EQ(1,script->queries);EXPECT_EQ(1,script->secondary_connects);
  SQLUINTEGER dead=SQL_CD_TRUE;
  EXPECT_EQ(SQL_SUCCESS,SQLGetConnectAttr(dbc,SQL_ATTR_CONNECTION_DEAD,&dead,sizeof(dead),nullptr));EXPECT_EQ(SQL_CD_FALSE,dead);
}

TEST_F(PublicCancellationTest, AllFunctionInventoriesAgreeAndWrongHandleNeverTargetsStatement) {
  SQLUSMALLINT scalar=SQL_FALSE;
  ASSERT_EQ(SQL_SUCCESS,SQLGetFunctions(dbc,SQL_API_SQLCANCEL,&scalar));EXPECT_EQ(SQL_TRUE,scalar);
  std::array<SQLUSMALLINT,100> legacy{};
  ASSERT_EQ(SQL_SUCCESS,SQLGetFunctions(dbc,SQL_API_ALL_FUNCTIONS,legacy.data()));EXPECT_EQ(SQL_TRUE,legacy[SQL_API_SQLCANCEL]);
  std::array<SQLUSMALLINT,SQL_API_ODBC3_ALL_FUNCTIONS_SIZE> all{};
  ASSERT_EQ(SQL_SUCCESS,SQLGetFunctions(dbc,SQL_API_ODBC3_ALL_FUNCTIONS,all.data()));EXPECT_TRUE(SQL_FUNC_EXISTS(all.data(),SQL_API_SQLCANCEL));
  EXPECT_EQ(SQL_INVALID_HANDLE,SQLCancel(reinterpret_cast<SQLHSTMT>(dbc)));
  EXPECT_EQ(SQL_INVALID_HANDLE,SQLCancel(nullptr));EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));
#ifdef SQL_API_SQLCANCELHANDLE
  scalar=SQL_TRUE;
  ASSERT_EQ(SQL_SUCCESS,SQLGetFunctions(dbc,SQL_API_SQLCANCELHANDLE,&scalar));EXPECT_EQ(SQL_FALSE,scalar);
#endif
  EXPECT_EQ(0,script->secondary_connects);EXPECT_EQ(0,script->queries);
}

TEST(SessionCancellationTest, SecondaryTlsRefusalOrUnverifiedPeerNeverReceivesProtectedCancelPacket) {
  for(const bool refused : {false,true}) {
    auto script=std::make_shared<Script>();script->tls=true;script->peer_tls_proof=false;
    if(refused)script->peer_tls_reply='N';
    postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
    auto options=settings();options.use_ssl=true;
    auto connected=connection.connect(options);ASSERT_TRUE(connected) << connected.error_message();
    const auto deadline=Deadline::clock::now()+2s;
    auto operation=connection.arm_cancellation(19,deadline);ASSERT_TRUE(operation);
    auto execution=std::async(std::launch::async,[&]{return connection.execute_query("SELECT 0",deadline);});
    ASSERT_TRUE(wait_query(script));EXPECT_FALSE(operation->request());
    EXPECT_EQ(literal({0,0,0,8,4,210,22,47}),script->ssl_request);EXPECT_TRUE(script->packet.empty());
    if(!refused)EXPECT_EQ("synthetic.invalid",script->peer_hostname);
    {std::lock_guard lock(script->mutex);script->release=true;script->changed.notify_all();}
    EXPECT_TRUE(execution.get().has_error());
    const auto outcome=connection.finish_cancellation(operation);
    EXPECT_FALSE(outcome.confirmed);EXPECT_TRUE(outcome.retire);EXPECT_FALSE(connection.is_connected());
  }
}

TEST_F(PublicCancellationTest, PreparedCancellationFailedTransactionRequiresExplicitRollbackThenSameStatementReuse) {
  ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(dbc,SQL_ATTR_AUTOCOMMIT,reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF),0));
  const auto begin=literal({'C',0,0,0,10,'B','E','G','I','N',0,'Z',0,0,0,5,'T'});
  auto rows_in_transaction=integer_row;rows_in_transaction.back()=std::byte{'T'};
  script->queued_replies={begin,rows_in_transaction};script->release=true;
  ASSERT_EQ(SQL_SUCCESS,execute(stmt));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
  script->release=false;script->query_sent=false;
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT ?")),SQL_NTS));
  auto& input=bound_input;auto& input_length=bound_input_length;
  auto& parameter_status=bound_parameter_status;auto& processed=bound_processed;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAM_STATUS_PTR,&parameter_status,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&input,sizeof(input),&input_length));
  script->reply=cancelled_idle;script->reply.back()=std::byte{'E'};
  auto execution=std::async(std::launch::async,[&]{return SQLExecute(stmt);});
  ASSERT_TRUE(wait_query(script));EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));
  EXPECT_EQ(SQL_ERROR,execution.get());EXPECT_EQ("HY008",state());
  EXPECT_EQ(SQL_PARAM_ERROR,parameter_status);EXPECT_EQ(1u,processed);
  EXPECT_EQ(SQL_ERROR,SQLDisconnect(dbc));
  script->reply=literal({'C',0,0,0,13,'R','O','L','L','B','A','C','K',0,'Z',0,0,0,5,'I'});script->release=true;
  ASSERT_EQ(SQL_SUCCESS,SQLEndTran(SQL_HANDLE_DBC,dbc,SQL_ROLLBACK));
  input=11;script->reply=literal({'1',0,0,0,4,'2',0,0,0,4});
  script->reply.insert(script->reply.end(),rows_in_transaction.begin(),rows_in_transaction.end());
  script->queued_replies={begin,script->reply};
  EXPECT_EQ(SQL_SUCCESS,SQLExecute(stmt));EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status);EXPECT_EQ(1u,processed);
  EXPECT_EQ(SQL_SUCCESS,SQLFetch(stmt));
  SQLINTEGER output=-1;SQLLEN length=-1;
  EXPECT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,SQL_C_SLONG,&output,sizeof(output),&length));EXPECT_EQ(7,output);
  EXPECT_EQ(1,script->secondary_connects);EXPECT_EQ(6,script->queries);
}

TEST_F(PublicCancellationTest, OriginalDeadlineWinsAndConfirmedDrainIsRequiredBeforeHy008) {
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(1),0));
  // Secondary EOF does not acknowledge cancellation. A partial main error with
  // no READY must time out and retire, never publish a reusable HY008 outcome.
  script->reply=cancelled_idle;script->reply.resize(script->reply.size()-6);script->hold_main_eof=true;
  auto execution=std::async(std::launch::async,[&]{return execute(stmt);});
  ASSERT_TRUE(wait_query(script));EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));
  EXPECT_EQ(SQL_ERROR,execution.get());EXPECT_EQ("HYT00",state());
  SQLUINTEGER dead=73;
  EXPECT_EQ(SQL_ERROR,SQLGetConnectAttr(dbc,SQL_ATTR_CONNECTION_DEAD,&dead,sizeof(dead),nullptr));EXPECT_EQ(73u,dead);
  SQLCHAR dbc_state[6]{};
  ASSERT_EQ(SQL_SUCCESS,SQLGetDiagRec(SQL_HANDLE_DBC,dbc,1,dbc_state,nullptr,nullptr,0,nullptr));
  EXPECT_STREQ("08003",reinterpret_cast<const char*>(dbc_state));
  EXPECT_EQ(1,script->closes);EXPECT_EQ(1,script->secondary_connects);
  // Reconnection is explicit, never a side effect of SQLCancel or a retry.
  script->hold_main_eof=false;script->reply=integer_row;script->release=true;
  ASSERT_EQ(SQL_SUCCESS,SQLDriverConnect(dbc,nullptr,reinterpret_cast<SQLCHAR*>(const_cast<char*>("SERVER=synthetic.invalid;PORT=5432;DATABASE=synthetic;UID=synthetic;SSL=0")),SQL_NTS,nullptr,0,nullptr,SQL_DRIVER_NOPROMPT));
  EXPECT_EQ(SQL_SUCCESS,execute(stmt));EXPECT_EQ(SQL_SUCCESS,SQLFetch(stmt));
  EXPECT_EQ(2,script->queries);EXPECT_EQ(1,script->secondary_connects);
}

TEST_F(PublicCancellationTest, NativeErrorObservedBeforeCancelRetainsOriginalDiagnosticWithoutPacket) {
  script->reply=literal({'E',0,0,0,25,'S','E','R','R','O','R',0,'C','2','2','0','1','2',0,'M','s','t','o','p',0,0,'Z',0,0,0,5,'I'});
  script->release=true;script->hold_ready=true;
  auto execution=std::async(std::launch::async,[&]{return execute(stmt);});
  {
    std::unique_lock lock(script->mutex);
    ASSERT_TRUE(script->changed.wait_for(lock,2s,[&]{return script->before_ready_waiting;}));
  }
  EXPECT_EQ(SQL_ERROR,SQLCancel(stmt));EXPECT_EQ(0,script->secondary_connects);
  {std::lock_guard lock(script->mutex);script->release_ready=true;script->changed.notify_all();}
  EXPECT_EQ(SQL_ERROR,execution.get());EXPECT_EQ("22012",state());EXPECT_TRUE(script->packet.empty());
  script->hold_ready=false;script->reply=integer_row;
  EXPECT_EQ(SQL_SUCCESS,execute(stmt));EXPECT_EQ(SQL_SUCCESS,SQLFetch(stmt));
}

TEST_F(PublicCancellationTest, EofBeforeReadyRetiresAndDoesNotBecomeConfirmedCancellation) {
  script->reply=cancelled_idle;script->reply.resize(script->reply.size()-6);
  auto execution=std::async(std::launch::async,[&]{return execute(stmt);});
  ASSERT_TRUE(wait_query(script));EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));
  EXPECT_EQ(SQL_ERROR,execution.get());EXPECT_NE("HY008",state());
  SQLUINTEGER dead=SQL_CD_FALSE;
  ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(dbc,SQL_ATTR_CONNECTION_DEAD,&dead,sizeof(dead),nullptr));EXPECT_EQ(SQL_CD_TRUE,dead);
  EXPECT_EQ(1,script->closes);
}

TEST(SessionCancellationTest, FragmentedStartupErrorReadyAndCancelWritesRetainOneCutoffAndOwningNativeState) {
  auto script=std::make_shared<Script>();script->main_fragment=2;script->secondary_fragment=3;
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  auto connected=connection.connect(settings());ASSERT_TRUE(connected) << connected.error_message();
  auto operation=connection.arm_cancellation(29,Deadline::max());ASSERT_TRUE(operation);
  auto execution=std::async(std::launch::async,[&]{return connection.execute_query("SELECT 0",Deadline::max());});
  ASSERT_TRUE(wait_query(script));EXPECT_TRUE(operation->request());
  const auto cutoff=script->secondary_deadline;EXPECT_TRUE(operation->request());EXPECT_EQ(cutoff,script->secondary_deadline);
  auto result=execution.get();ASSERT_TRUE(result.has_error());EXPECT_EQ("57014",result.backend_error().native_state);
  EXPECT_EQ(expected_packet,script->packet);EXPECT_EQ(1,script->secondary_connects);
  EXPECT_TRUE(connection.finish_cancellation(operation).confirmed);
  script->reply.assign(script->reply.size(),std::byte{0});
  EXPECT_EQ("57014",result.backend_error().native_state);EXPECT_EQ("Query error: stop",result.error_message());
}

TEST(SessionCancellationTest, DrainedBeginGapRetainsLocalClaimAndPreventsUserQuery) {
  auto script=std::make_shared<Script>();
  script->reply=literal({'C',0,0,0,10,'B','E','G','I','N',0,'Z',0,0,0,5,'T'});script->release=true;
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  auto connected=connection.connect(settings());ASSERT_TRUE(connected) << connected.error_message();
  auto operation=connection.arm_cancellation(39,Deadline::max());ASSERT_TRUE(operation);
  auto begin=connection.transaction(TransactionAction::Begin,Deadline::max());ASSERT_TRUE(begin.has_value());
  EXPECT_EQ(SessionState::Transaction,begin.session_snapshot().state);
  // This is the deterministic owner interval between legal BEGIN READY and
  // explicit admission of the user phase; it is not a completed/idle STMT.
  EXPECT_TRUE(operation->request());EXPECT_EQ(0,script->secondary_connects);
  EXPECT_FALSE(connection.continue_cancellation(operation));
  EXPECT_TRUE(connection.execute_query("SELECT must_not_dispatch",Deadline::max()).has_error());
  EXPECT_EQ(1,script->queries);EXPECT_TRUE(script->packet.empty());
  auto outcome=connection.finish_cancellation(operation);EXPECT_TRUE(outcome.confirmed);EXPECT_FALSE(outcome.retire);
  script->reply=literal({'C',0,0,0,13,'R','O','L','L','B','A','C','K',0,'Z',0,0,0,5,'I'});
  EXPECT_TRUE(connection.transaction(TransactionAction::Rollback,Deadline::max()).has_value());EXPECT_EQ(2,script->queries);
}

TEST_F(PublicCancellationTest, AfterReadyBeforePublicationLocalClaimInvalidatesTargetWithoutLatePacket) {
  script->reply=integer_row;script->release=true;
  ASSERT_EQ(SQL_SUCCESS,execute(sibling));
  script->hold_normalization=true;
  auto execution=std::async(std::launch::async,[&]{return execute(stmt);});
  {
    std::unique_lock lock(script->mutex);
    ASSERT_TRUE(script->changed.wait_for(lock,2s,[&]{return script->normalization_waiting;}));
  }
  EXPECT_EQ(SQL_SUCCESS,SQLCancel(stmt));EXPECT_EQ(0,script->secondary_connects);EXPECT_TRUE(script->packet.empty());
  {std::lock_guard lock(script->mutex);script->release_normalization=true;script->changed.notify_all();}
  EXPECT_EQ(SQL_SUCCESS,execution.get());
  EXPECT_EQ(SQL_ERROR,SQLFetch(stmt));EXPECT_EQ("HY010",state());
  EXPECT_EQ(SQL_SUCCESS,SQLFetch(sibling));
  SQLINTEGER value=-1;SQLLEN length=-1;
  EXPECT_EQ(SQL_SUCCESS,SQLGetData(sibling,1,SQL_C_SLONG,&value,sizeof(value),&length));EXPECT_EQ(7,value);
  script->hold_normalization=false;
  EXPECT_EQ(SQL_SUCCESS,execute(stmt));EXPECT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(0,script->closes);
}

TEST(SessionCancellationTest, PartialMainWriteClaimNeverSendsSecondaryPacketAndTransportFailureWins) {
  auto script=std::make_shared<Script>();script->partial_write_failure=true;
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  auto connected=connection.connect(settings());ASSERT_TRUE(connected) << connected.error_message();
  auto operation=connection.arm_cancellation(49,Deadline::max());ASSERT_TRUE(operation);
  auto execution=std::async(std::launch::async,[&]{return connection.execute_query("SELECT 0",Deadline::max());});
  {
    std::unique_lock lock(script->mutex);
    ASSERT_TRUE(script->changed.wait_for(lock,2s,[&]{return script->partial_write_waiting;}));
  }
  auto cancel=std::async(std::launch::async,[&]{return operation->request();});
  const auto bound=Deadline::clock::now()+1s;
  {
    std::unique_lock lock(script->mutex);
    while(script->main_control->effective(Deadline::max())==Deadline::max() && Deadline::clock::now()<bound)
      script->changed.wait_for(lock,1ms);
    ASSERT_LT(script->main_control->effective(Deadline::max()),Deadline::max());
    script->release_write=true;script->changed.notify_all();
  }
  auto result=execution.get();ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(DbErrorCode::NetworkError),result.error());
  EXPECT_EQ(SessionDisposition::Retire,result.session_snapshot().disposition);
  EXPECT_FALSE(connection.finish_cancellation(operation).confirmed);EXPECT_FALSE(cancel.get());
  EXPECT_TRUE(script->packet.empty());EXPECT_EQ(0,script->secondary_connects);EXPECT_EQ(1,script->closes);
}

TEST(SessionCancellationTest, DirectOwnerDestructionAndFailureSealRetainedEndpointAcrossFreshSession) {
  auto script=std::make_shared<Script>();std::shared_ptr<SessionCancellation> old;
  {
    postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
    auto connected=connection.connect(settings());ASSERT_TRUE(connected) << connected.error_message();
    old=connection.arm_cancellation(1,Deadline::max());ASSERT_TRUE(old);
  }
  EXPECT_TRUE(old->request());EXPECT_FALSE(old->seal().claimed);EXPECT_EQ(0,script->secondary_connects);
  // A different physical owner can use the same caller's numerical generation;
  // retained opaque endpoint identity, not a guessable integer/PID, controls it.
  postgres::PgDatabaseConnection fresh(std::make_unique<Wire>(script));
  auto connected=fresh.connect(settings());ASSERT_TRUE(connected) << connected.error_message();
  auto current=fresh.arm_cancellation(1,Deadline::max());ASSERT_TRUE(current);
  EXPECT_TRUE(old->request());EXPECT_EQ(0,script->secondary_connects);
  EXPECT_TRUE(current->request());EXPECT_TRUE(fresh.execute_query("SELECT no_dispatch",Deadline::max()).has_error());
  EXPECT_TRUE(fresh.finish_cancellation(current).confirmed);EXPECT_EQ(0,script->queries);
}

TEST(SessionCancellationTest, UnsolicitedCancelStateNeverConfirmsAndMalformedReadyRetires) {
  for(const bool malformed : {false,true}) {
    auto script=std::make_shared<Script>();script->release=true;
    if(malformed)script->reply.back()=std::byte{'X'};
    postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
    auto connected=connection.connect(settings());ASSERT_TRUE(connected) << connected.error_message();
    auto operation=connection.arm_cancellation(59,Deadline::max());ASSERT_TRUE(operation);
    auto result=connection.execute_query("SELECT 0",Deadline::max());ASSERT_TRUE(result.has_error());
    if(!malformed)EXPECT_EQ("57014",result.backend_error().native_state);
    const auto outcome=connection.finish_cancellation(operation);
    EXPECT_FALSE(outcome.confirmed);EXPECT_FALSE(outcome.claimed);EXPECT_EQ(0,script->secondary_connects);
    if(malformed){EXPECT_FALSE(connection.is_connected());EXPECT_EQ(1,script->closes);}
    else {EXPECT_TRUE(connection.is_connected());EXPECT_EQ(SessionState::Idle,connection.session_state());}
  }
}


TEST(SessionCancellationTest, ActualUnknownTypeResolverRefusesBeforeClaimOrCutoffAndRecovers) {
  auto script = std::make_shared<Script>();
  script->queued_replies = {prepared_unknown_rows(), resolver_rows};
  script->release = true;
  script->hold_resolver = true;
  script->resolver_query_number = 2;
  postgres::PgDatabaseConnection connection(std::make_unique<Wire>(script));
  ASSERT_TRUE(connection.connect(settings()));
  const auto deadline = Deadline::clock::now() + 10s;
  auto operation = connection.arm_cancellation(71, deadline);
  ASSERT_TRUE(operation);
  auto wire = std::dynamic_pointer_cast<SessionCancellationWire>(operation);
  ASSERT_TRUE(wire);
  const auto wait = wire->wait_control();
  const QueryParameter parameter{std::string{"7"}, QueryParameterType::Int32};
  auto execution = std::async(std::launch::async, [&] {
    return connection.execute_prepared("SELECT $1", std::span{&parameter, 1}, deadline);
  });
  bool entered = false;
  {
    std::unique_lock lock(script->mutex);
    entered = script->changed.wait_for(lock, 2s, [&] { return script->resolver_waiting; });
  }
  EXPECT_TRUE(entered);
  EXPECT_FALSE(operation->request());
  EXPECT_EQ(deadline, wait->effective(deadline));
  EXPECT_EQ(0, script->secondary_connects);
  EXPECT_TRUE(script->packet.empty());
  { std::lock_guard lock(script->mutex); script->release_resolver = true; script->changed.notify_all(); }
  auto result = execution.get();
  ASSERT_TRUE(result) << result.error_message();
  ASSERT_EQ(1u, result->normalized_parameter_types.size());
  EXPECT_TRUE(result->normalized_parameter_types[0].known);
  EXPECT_EQ(ScalarType::Integer, result->normalized_parameter_types[0].type);
  ASSERT_EQ(1u, result->rows.size());
  EXPECT_EQ(std::optional<std::string>{"7"}, result->rows[0][0]);
  const auto outcome = connection.finish_cancellation(operation);
  EXPECT_FALSE(outcome.claimed); EXPECT_FALSE(outcome.confirmed); EXPECT_FALSE(outcome.retire);
  EXPECT_EQ(2, script->queries); EXPECT_EQ(SessionState::Idle, connection.session_state());
  script->hold_resolver = false; script->reply = integer_row;
  auto recovery = connection.execute_query("SELECT 7", deadline);
  ASSERT_TRUE(recovery); EXPECT_EQ(3, script->queries); EXPECT_EQ(0, script->closes);
}

TEST_F(PublicCancellationTest, ResolverTransportRefusalLeavesDiagnosticsAndOriginalCompletion) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT ?")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &bound_input, sizeof(bound_input), &bound_input_length));
  script->queued_replies = {prepared_unknown_rows(), resolver_rows};
  script->release = true; script->hold_resolver = true; script->resolver_query_number = 2;
  auto execution = std::async(std::launch::async, [&] { return SQLExecute(stmt); });
  bool entered = false;
  { std::unique_lock lock(script->mutex);
    entered = script->changed.wait_for(lock, 2s, [&] { return script->resolver_waiting; }); }
  EXPECT_TRUE(entered);
  const auto target = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCStatement>(stmt);
  const auto diagnostics = target->get_diagnostic_count();
  const auto return_code = target->get_last_return_code();
  EXPECT_EQ(SQL_ERROR, SQLCancel(stmt));
  EXPECT_EQ(diagnostics, target->get_diagnostic_count());
  EXPECT_EQ(return_code, target->get_last_return_code());
  EXPECT_TRUE(script->packet.empty()); EXPECT_EQ(0, script->secondary_connects);
  { std::lock_guard lock(script->mutex); script->release_resolver = true; script->changed.notify_all(); }
  EXPECT_EQ(SQL_SUCCESS, execution.get()); EXPECT_EQ(2, script->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_SLONG, &bound_output.value, sizeof(bound_output.value), &bound_output_length));
  EXPECT_EQ(7, bound_output.value); EXPECT_EQ(0x12345678u, bound_output.before); EXPECT_EQ(0x87654321u, bound_output.after);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  script->hold_resolver = false; script->reply = integer_row;
  EXPECT_EQ(SQL_SUCCESS, execute(stmt)); EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(0, script->closes);
}

TEST_F(PublicCancellationTest, EarlierLocalPublicationClaimPreventsResolverDispatchAndPreservesSibling) {
  script->reply = integer_row; script->release = true;
  ASSERT_EQ(SQL_SUCCESS, execute(sibling));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT ?")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &bound_input, sizeof(bound_input), &bound_input_length));
  script->queued_replies = {prepared_unknown_rows(), resolver_rows};
  script->hold_normalization = true;
  auto execution = std::async(std::launch::async, [&] { return SQLExecute(stmt); });
  bool entered = false;
  { std::unique_lock lock(script->mutex);
    entered = script->changed.wait_for(lock, 2s, [&] { return script->normalization_waiting; }); }
  EXPECT_TRUE(entered); EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt));
  { std::lock_guard lock(script->mutex); script->release_normalization = true; script->changed.notify_all(); }
  EXPECT_EQ(SQL_SUCCESS, execution.get());
  EXPECT_EQ(2, script->queries); EXPECT_EQ(1u, script->queued_replies.size());
  EXPECT_TRUE(script->packet.empty()); EXPECT_EQ(0, script->secondary_connects);
  EXPECT_EQ(SQL_ERROR, SQLFetch(stmt)); EXPECT_EQ("HY010", state());
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling));
  EXPECT_EQ(SQL_SUCCESS, SQLGetData(sibling, 1, SQL_C_SLONG, &bound_output.value, sizeof(bound_output.value), &bound_output_length));
  EXPECT_EQ(7, bound_output.value); EXPECT_EQ(0x12345678u, bound_output.before); EXPECT_EQ(0x87654321u, bound_output.after);
  script->hold_normalization = false; script->queued_replies.clear(); script->reply = integer_row;
  EXPECT_EQ(SQL_SUCCESS, execute(stmt)); EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(3, script->queries); EXPECT_EQ(0, script->closes);
}
