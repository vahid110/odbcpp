#include "runner.h"
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace rs::core::database;
enum class Fault { None, Connect, Row, Type, UnknownType, WrongType, RootError, Prepared, ErrorState, Recovery, Disconnect };
int disconnect_calls{};
class Synthetic final : public IDatabaseConnection {
 public:
  explicit Synthetic(Fault fault) : fault_(fault) {}
  BackendResult<void> connect(const ConnectionSettings&) override {
    open_ = true;
    return BackendResult<void>{fault_ == Fault::Connect ? SessionSnapshot{} : ready};
  }
  void disconnect() override { ++disconnect_calls; if (fault_ != Fault::Disconnect) open_ = false; }
  bool is_connected() const override { return open_; }
  SessionState session_state() const override { return open_ ? SessionState::Idle : SessionState::Disconnected; }
  std::string server_version() const override { return "synthetic"; }
  BackendResult<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) override {
    if (!open_ || deadline == rs::util::Deadline::max() || deadline <= rs::util::Clock::now())
      throw std::runtime_error("invalid operation");
    if (sql == "reject") {
      rejected_ = true;
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "private canary"};
      error.operation = BackendOperation::ExecuteDirect;
      error.session_state = SessionState::Idle;
      error.disposition = SessionDisposition::Reusable;
      if (fault_ == Fault::ErrorState) open_ = false;
      return error;
    }
    auto row = ResultRow{std::string("baseline"), std::nullopt, std::string{}};
    if (fault_ == Fault::Recovery && rejected_) row[0] = "wrong";
    return result(std::move(row));
  }
  BackendResult<QueryResult> execute_prepared(std::string_view, std::span<const QueryParameter> parameters,
      rs::util::Deadline deadline) override {
    if (!open_ || deadline == rs::util::Deadline::max() || deadline <= rs::util::Clock::now())
      throw std::runtime_error("invalid operation");
    ResultRow row;
    for (const auto& parameter : parameters) {
      if (parameter.type != QueryParameterType::Text) throw std::runtime_error("invalid parameter");
      row.push_back(parameter.value);
    }
    if (fault_ == Fault::Prepared) row[0] = "wrong";
    return result(std::move(row));
  }
 private:
  BackendResult<QueryResult> result(ResultRow row) {
    QueryResult result;
    for (std::size_t i = 0; i < row.size(); ++i)
      result.columns.push_back({"value", NativeTypeInfo{ScalarType::VarChar, 32, 0, true}});
    if (fault_ == Fault::Row) row.pop_back();
    if (fault_ == Fault::Type) result.columns[0].normalized_type.reset();
    if (fault_ == Fault::UnknownType) result.columns[0].normalized_type->known = false;
    if (fault_ == Fault::WrongType) result.columns[0].normalized_type->type = ScalarType::Integer;
    if (fault_ == Fault::RootError)
      result.error.emplace(rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "private canary");
    result.rows.push_back(std::move(row));
    return BackendResult<QueryResult>{std::move(result), ready};
  }
  static constexpr SessionSnapshot ready{SessionState::Idle, SessionDisposition::Reusable};
  Fault fault_;
  bool open_{};
  bool rejected_{};
};
}
int main() {
  odbcpp::test::SessionBaselineFixture fixture{
      "direct", "prepared", "reject",
      {{std::string("baseline"), QueryParameterType::Text}, {std::nullopt, QueryParameterType::Text},
       {std::string{}, QueryParameterType::Text}},
      {std::string("baseline"), std::nullopt, std::string{}},
      {ScalarType::VarChar, ScalarType::VarChar, ScalarType::VarChar}};
  ConnectionSettings settings;
  settings.host = "synthetic"; settings.port = 1; settings.database = "fixture";
  settings.user = "fixture"; settings.password = "private canary"; settings.use_ssl = false;
  for (auto [fault, expected] : {
      std::pair{Fault::None, std::string_view{}}, {Fault::Connect, "connect.state"},
      {Fault::Row, "direct.result"}, {Fault::Type, "direct.result"}, {Fault::RootError, "direct.result"},
      {Fault::UnknownType, "direct.result"}, {Fault::WrongType, "direct.result"},
      {Fault::Prepared, "prepared.result"}, {Fault::ErrorState, "error.recovery_state"},
      {Fault::Recovery, "recovery.result"}, {Fault::Disconnect, "disconnect.state"}}) {
    disconnect_calls = 0;
    const auto report = odbcpp::test::run_session_baseline(
        [fault] { return std::make_unique<Synthetic>(fault); }, settings, fixture);
    if (report != expected) { std::cerr << "Unexpected baseline check: " << report << '\n'; return 1; }
    if (disconnect_calls == 0) return 3;
  }
  fixture.operation_timeout = std::chrono::milliseconds::zero();
  if (odbcpp::test::run_session_baseline([] { return std::make_unique<Synthetic>(Fault::None); },
      settings, fixture) != "fixture") return 2;
  return 0;
}
